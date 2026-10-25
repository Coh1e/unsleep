#include "unsleep/core.hpp"

#include <format>
#include <utility>

namespace unsleep {
namespace {

bool percent_known(const BatterySample& sample) noexcept {
    return sample.percent && *sample.percent >= 0 && *sample.percent <= 100;
}

int normalized_threshold(int percent) noexcept {
    return is_valid_threshold(percent) ? percent : default_battery_threshold;
}

const wchar_t* target_name(RequestTarget target) noexcept {
    return target == RequestTarget::display ? L"屏幕" : L"系统";
}

using TimePoint = Model::TimePoint;

// Saturating deadline arithmetic: a clock reading close to the end of its
// range must not overflow into a past deadline.
TimePoint add_length(TimePoint base, std::chrono::minutes length) noexcept {
    const auto delta = std::chrono::duration_cast<TimePoint::duration>(length);
    if (base > TimePoint::max() - delta) return TimePoint::max();
    return base + delta;
}

// Which stop reason wins when several conditions are observed in the same
// handling cycle, per the product order: sleep/resume, low battery or unknown
// battery, request failure, expiry, ordinary actions.
int reason_rank(StopReason reason) noexcept {
    switch (reason) {
    case StopReason::sleep: return 0;
    case StopReason::low_battery: return 1;
    case StopReason::battery_unknown: return 2;
    case StopReason::request_failed: return 3;
    case StopReason::expired: return 4;
    case StopReason::manual: return 5;
    case StopReason::none: return 6;
    }
    return 6;
}

// Ordinary actions are the least urgent stop requests; everything else outranks
// an already-reached deadline and keeps its own reason.
bool is_urgent(StopReason reason) noexcept {
    return reason != StopReason::manual && reason != StopReason::none;
}

} // namespace

bool is_valid_duration(Duration duration) noexcept {
    switch (duration) {
    case Duration::unlimited:
    case Duration::minutes_30:
    case Duration::hours_1:
    case Duration::hours_2:
        return true;
    }
    return false;
}

std::chrono::minutes duration_length(Duration duration) noexcept {
    switch (duration) {
    case Duration::unlimited: return std::chrono::minutes{0};
    case Duration::minutes_30: return std::chrono::minutes{30};
    case Duration::hours_1: return std::chrono::minutes{60};
    case Duration::hours_2: return std::chrono::minutes{120};
    }
    return std::chrono::minutes{0};
}

Model::Model(int low_battery_threshold, Clock clock)
    : threshold_(normalized_threshold(low_battery_threshold)),
      clock_(std::move(clock)) {
    if (!clock_) clock_ = [] { return std::chrono::steady_clock::now(); };
    now_ = clock_();
}

// Conservative gate: run only with clear evidence that battery protection is
// satisfied. A successful read that confirms AC power is sufficient on its
// own; battery presence and charge do not need to be known while plugged in.
// An absent battery is allowed, and an unplugged battery must report a known
// charge above the threshold. Unknown data never fabricates a charge value.
std::optional<StopReason> Model::gate(const BatterySample& sample) const {
    if (sample.read_failed) return StopReason::battery_unknown;
    if (sample.ac == AcState::plugged) return std::nullopt;
    if (sample.presence == BatteryPresence::absent) return std::nullopt;
    if (sample.ac == AcState::unknown) return StopReason::battery_unknown;
    if (!percent_known(sample)) return StopReason::battery_unknown;
    if (*sample.percent <= threshold_) return StopReason::low_battery;
    return std::nullopt;
}

void Model::advance_time() noexcept {
    // Monotonic and order-insensitive: an out-of-order or stale reading can
    // never move the clock backwards.
    const TimePoint reading = clock_();
    if (reading > now_) now_ = reading;
}

bool Model::expire_if_due() {
    if (!running_ || !deadline_) return false;
    if (now_ < *deadline_) return false;
    // Expiry speaks for itself in the tooltip and is never announced.
    enter_stop(StopReason::expired, true);
    return true;
}

void Model::refresh_time() {
    advance_time();
    static_cast<void>(expire_if_due());
}

std::optional<std::chrono::seconds> Model::remaining_time() const {
    if (!running_ || !deadline_) return std::nullopt;
    if (now_ >= *deadline_) return std::chrono::seconds{0};
    return std::chrono::ceil<std::chrono::seconds>(*deadline_ - now_);
}

std::wstring Model::remaining_text() const {
    const auto remaining = remaining_time();
    if (!remaining) return L"不限时";
    const auto total = remaining->count();
    if (total >= 60) return std::format(L"剩余 {} 分 {} 秒", total / 60, total % 60);
    return std::format(L"剩余 {} 秒", total);
}

DurationChange Model::set_duration(Duration duration) {
    // Ordinary action: apply an already-reached deadline first so selecting a
    // duration can never extend a finished session.
    refresh_time();
    if (!is_valid_duration(duration)) return DurationChange::rejected;
    if (!running_) return DurationChange::rejected_stopped;
    if (duration == duration_) return DurationChange::unchanged;
    duration_ = duration;
    if (duration == Duration::unlimited) {
        deadline_.reset();
        return DurationChange::updated;
    }
    deadline_ = add_length(now_, duration_length(duration));
    return DurationChange::updated;
}

bool Model::start() {
    if (running_) return true;
    reset_notifications();
    if (const auto reason = gate(sample_)) {
        enter_stop(*reason, false);
        return false;
    }
    advance_time();
    running_ = true;
    confirmed_ = false;
    release_unconfirmed_ = false;
    // Every start uses the product defaults: display may turn off, no limit.
    clear_display_state();
    display_release_unconfirmed_ = false;
    duration_ = Duration::unlimited;
    deadline_.reset();
    reason_ = StopReason::none;
    failed_request_ = RequestTarget::system;
    last_error_ = 0;
    actions_.clear();
    actions_.push_back(Action::apply_system_request);
    return true;
}

void Model::stop(StopReason reason) {
    if (running_ && deadline_ && !is_urgent(reason)) {
        // An ordinary stop (manual) is the least urgent event, so a deadline
        // that has already been reached is applied first and keeps its own
        // reason, exactly as the periodic tick would have done. The intent is
        // still stop: this branch can only end a session, never start one.
        //
        // The clock is read only when a deadline exists, matching the stated
        // contract: an unlimited session reaches enter_stop(manual) without
        // touching the monotonic clock at all, so its outcome cannot depend on
        // which instant the stop happens to be handled.
        advance_time();
        if (expire_if_due()) return;
    }

    if (!running_) {
        // While stopped only a more urgent reason may replace the recorded
        // one, so a concurrent low-battery/sleep observation still wins over
        // an earlier expiry and a later ordinary action never rewrites it.
        if (reason_ == StopReason::none ||
            (reason != StopReason::none && reason_rank(reason) < reason_rank(reason_))) {
            reason_ = reason;
        }
        return;
    }
    enter_stop(reason, true);
}

bool Model::toggle() {
    // The user's intent is fixed before any clock or battery handling: a
    // running session here means "stop", and a stopped one means "start".
    // The stop itself decides the reason (expired when the deadline has already
    // been reached, manual otherwise), so reaching the deadline in this cycle
    // can never turn the stop intent into a fresh start. Only a session that
    // is already stopped can start.
    if (!running_) return start();
    stop(StopReason::manual);
    return false;
}

DisplayToggle Model::toggle_display() {
    // Ordinary action: a reached deadline stops the session before the
    // selection can be applied, so time advancing never restores anything.
    refresh_time();
    // Screen keep-awake is a this-session option and only meaningful while the
    // keep-awake session is running.
    if (!running_) return DisplayToggle::rejected_stopped;

    if (display_selected_) {
        display_selected_ = false;
        display_pending_ = false;
        // Dropping the display request keeps the system request in place.
        if (display_confirmed_) {
            display_confirmed_ = false;
            actions_.push_back(Action::release_display_request);
        }
        return DisplayToggle::disabled;
    }

    display_selected_ = true;
    // The initial session state is queried by the adapter. When it is locked
    // or could not be read reliably, the selection is remembered but the
    // display request waits for an explicit unlock notification.
    if (display_suppressed()) return DisplayToggle::deferred_locked;
    display_pending_ = true;
    actions_.push_back(Action::apply_display_request);
    return DisplayToggle::enabled;
}

void Model::observe(const BatterySample& sample) {
    sample_ = sample;
    advance_time();
    if (!running_) return;
    // The battery gate is checked before the deadline so that a cycle which
    // observes both keeps the product priority (low battery over expiry).
    if (const auto reason = gate(sample_)) {
        enter_stop(*reason, true);
        return;
    }
    static_cast<void>(expire_if_due());
}

void Model::observe_session(SessionState state) {
    // Ordinary action: a reached deadline stops the session first, so an
    // unlock after the deadline never restores screen keep-awake.
    refresh_time();
    if (state == session_state_) return;
    session_state_ = state;
    if (!running_) return;

    if (state == SessionState::unlocked) {
        // Restore only for this run and only while the display option is
        // still selected. A pending request must not be queued twice.
        if (display_selected_ && !display_confirmed_ && !display_pending_) {
            display_pending_ = true;
            actions_.push_back(Action::apply_display_request);
        }
        return;
    }

    // A lock releases the display request but keeps the system request. If an
    // apply is still in flight, its late result is handled there.
    if (display_confirmed_) {
        display_confirmed_ = false;
        actions_.push_back(Action::release_display_request);
    }
}

ThresholdChange Model::set_threshold(int percent) {
    if (!is_valid_threshold(percent)) return ThresholdChange::rejected;
    if (percent == threshold_) return ThresholdChange::unchanged;
    threshold_ = percent;
    // Ordinary action: apply an already-reached deadline first, then the new
    // threshold. A stopped session (including an expired one) never restarts.
    refresh_time();
    if (!running_) return ThresholdChange::updated;
    if (const auto reason = gate(sample_)) {
        enter_stop(*reason, true);
        return ThresholdChange::stopped;
    }
    return ThresholdChange::updated;
}

void Model::on_request_result(bool accepted, int error_code) {
    if (!running_) {
        // A late success after stop must not revive the session; make sure the
        // request is released again instead.
        if (accepted) actions_.push_back(Action::release_request);
        return;
    }
    if (accepted) {
        confirmed_ = true;
        last_error_ = 0;
        return;
    }
    last_error_ = error_code;
    failed_request_ = RequestTarget::system;
    enter_stop(StopReason::request_failed, true);
    notify_fault(error_code != 0
        ? std::format(L"系统请求失败（错误 {}），防休眠已停止", error_code)
        : std::wstring(L"系统请求失败，防休眠已停止"));
}

void Model::on_display_result(bool accepted, int error_code) {
    display_pending_ = false;
    if (!accepted) {
        display_confirmed_ = false;
        if (!running_) return;
        last_error_ = error_code;
        failed_request_ = RequestTarget::display;
        enter_stop(StopReason::request_failed, true);
        notify_fault(error_code != 0
            ? std::format(L"屏幕请求失败（错误 {}），防休眠已停止", error_code)
            : std::wstring(L"屏幕请求失败，防休眠已停止"));
        return;
    }

    if (!running_) {
        // A late success after a stop must not keep anything awake. A full
        // release is safe and, unlike the display-only release, cannot
        // re-request the system while the session is stopped.
        actions_.push_back(Action::release_request);
        return;
    }
    if (!display_selected_ || display_suppressed()) {
        // A late success after a toggle-off or a lock must not keep the
        // display awake; the running system request stays in place.
        actions_.push_back(Action::release_display_request);
        return;
    }
    display_confirmed_ = true;
    display_release_unconfirmed_ = false;
}

void Model::on_release_result(bool released) {
    release_unconfirmed_ = !released;
    // Releasing both requests is all-or-nothing per the adapter contract; a
    // failed full release leaves the display state unconfirmed as well.
    display_release_unconfirmed_ = !released;
}

void Model::on_display_release_result(bool released, int error_code) {
    if (released) {
        display_release_unconfirmed_ = false;
        return;
    }
    // A late failure after stop is already covered by the full release and
    // must not revive or re-notify anything.
    if (!running_) return;
    last_error_ = error_code;
    failed_request_ = RequestTarget::display;
    enter_stop(StopReason::request_failed, true);
    notify_fault(error_code != 0
        ? std::format(L"屏幕请求释放失败（错误 {}），防休眠已停止", error_code)
        : std::wstring(L"屏幕请求释放失败，防休眠已停止"));
}

std::vector<Action> Model::take_actions() { return std::exchange(actions_, {}); }

std::vector<std::wstring> Model::take_notifications() { return std::exchange(notifications_, {}); }

void Model::reset_notifications() {
    low_battery_notified_ = false;
    unknown_notified_ = false;
    fault_notified_ = false;
}

void Model::notify_fault(std::wstring message) {
    if (fault_notified_) return;
    fault_notified_ = true;
    notifications_.push_back(std::move(message));
}

void Model::clear_display_state() noexcept {
    display_selected_ = false;
    display_confirmed_ = false;
    display_pending_ = false;
}

void Model::enter_stop(StopReason reason, bool was_running) {
    running_ = false;
    confirmed_ = false;
    reason_ = reason;
    // The deadline belongs to this session only; the selected duration stays
    // as the (disabled) menu state until the next start resets it.
    deadline_.reset();
    clear_display_state();
    display_release_unconfirmed_ = false;
    if (was_running) {
        // Dropping a not-yet-applied request is safe: the release call also
        // clears any request the system may already hold.
        actions_.clear();
        actions_.push_back(Action::release_request);
    }
    switch (reason) {
    case StopReason::low_battery:
        if (!low_battery_notified_) {
            low_battery_notified_ = true;
            notifications_.push_back(was_running ? L"电量低，防休眠已停止" : L"电量低，防休眠未开启");
        }
        break;
    case StopReason::battery_unknown:
        if (!unknown_notified_) {
            unknown_notified_ = true;
            notifications_.push_back(L"电池状态未知，防休眠已停止");
        }
        break;
    default:
        // Manual stops, sleep and request failures stay silent here; request
        // failures add their own one-time message.
        break;
    }
}

std::wstring Model::status_text() const {
    std::wstring text;
    if (running_) {
        const std::wstring remaining = remaining_text();
        if (!confirmed_) {
            text = display_selected_ ? L"正在请求防休眠与屏幕常亮…" : L"正在请求防休眠…";
        } else if (!display_selected_) {
            text = std::format(L"防休眠运行中（系统已接受请求，允许息屏，{}）", remaining);
        } else if (session_state_ == SessionState::unknown) {
            text = std::format(L"防休眠运行中（系统已接受请求，锁定状态未知，屏幕常亮已暂缓，{}）", remaining);
        } else if (session_state_ == SessionState::locked) {
            text = std::format(L"防休眠运行中（系统已接受请求，锁屏期间已释放屏幕请求，解锁后恢复，{}）", remaining);
        } else if (display_confirmed_) {
            text = std::format(L"防休眠运行中（系统及屏幕请求已接受，{}）", remaining);
        } else {
            text = std::format(L"防休眠运行中（系统已接受请求，正在请求屏幕常亮…，{}）", remaining);
        }
    } else {
        switch (reason_) {
        case StopReason::manual:
            text = L"已停止（手动停止）";
            break;
        case StopReason::expired:
            text = L"已停止（持续时间到期）";
            break;
        case StopReason::low_battery:
            text = std::format(L"已停止（电量不高于 {}%）", threshold_);
            break;
        case StopReason::battery_unknown:
            text = L"已停止（电池状态未知）";
            break;
        case StopReason::request_failed:
            text = last_error_ != 0
                ? std::format(L"已停止（{}请求失败，错误 {}）", target_name(failed_request_), last_error_)
                : std::format(L"已停止（{}请求失败）", target_name(failed_request_));
            break;
        case StopReason::sleep:
            text = L"已停止（系统睡眠）";
            break;
        case StopReason::none:
            text = L"已停止";
            break;
        }
    }
    if (release_unconfirmed_) text += L"；系统请求释放未确认";
    if (display_release_unconfirmed_) text += L"；屏幕请求释放未确认";
    if (!threshold_persisted_) text += L"；电池阈值设置未保存";
    return text;
}

} // namespace unsleep
