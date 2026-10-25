#pragma once

#include "unsleep/settings.hpp"

#include <array>
#include <chrono>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Core state model for Unsleep.
//
// The model never calls Windows APIs. It consumes explicit inputs (monotonic
// time, battery samples, session lock state, power-request/release results,
// user intents) and produces observable state plus a queue of actions the
// platform adapter must perform.
//
// Time comes from an injected monotonic clock (std::chrono::steady_clock by
// default). Wall clock and timezone changes never reach the model, and every
// ordinary input refreshes the clock and applies an already-reached deadline
// before it is handled, so no ordinary action can extend a finished session.
namespace unsleep {

enum class BatteryPresence { unknown, absent, present };
enum class AcState { unknown, plugged, unplugged };

struct BatterySample {
    BatteryPresence presence{BatteryPresence::unknown};
    AcState ac{AcState::unknown};
    std::optional<int> percent;
    bool read_failed{}; // system query itself failed
};

enum class StopReason {
    none,
    manual,
    expired,
    request_failed,
    battery_unknown,
    low_battery,
    sleep,
};

// Keep-awake duration for the current session. The product offers exactly
// these four choices; nothing else is selectable or persisted.
enum class Duration { unlimited, minutes_30, hours_1, hours_2 };

inline constexpr std::array<Duration, 4> durations{
    Duration::unlimited, Duration::minutes_30, Duration::hours_1, Duration::hours_2};

[[nodiscard]] bool is_valid_duration(Duration duration) noexcept;
// Length of a finite duration; zero for unlimited.
[[nodiscard]] std::chrono::minutes duration_length(Duration duration) noexcept;

// The target is explicit so the adapter can report system and display request
// results independently. `release_request` releases both requests; the
// display-only variants keep the system request in place.
enum class Action {
    apply_system_request,
    apply_display_request,   // system + display; needs a confirmed system request
    release_display_request, // lock or display toggle off; keeps the system request
    release_request,         // stop/exit; releases system and display requests
};

enum class RequestTarget { system, display };

enum class SessionState { unknown, unlocked, locked };

enum class ThresholdChange {
    rejected,  // not one of 10/20/30; the current threshold stays
    unchanged, // the requested value is already selected
    updated,   // the new threshold applies to the running session
    stopped,   // the new threshold ended the running session immediately
};

enum class DurationChange {
    rejected,         // not one of the four product choices; nothing changes
    rejected_stopped, // the duration is selectable only while running
    unchanged,        // the requested duration is already selected
    updated,          // the deadline was recomputed from the current moment
};

enum class DisplayToggle {
    rejected_stopped, // screen keep-awake is only selectable while running
    enabled,          // display request queued for an unlocked session
    deferred_locked,  // selection remembered; applied after an unlock event
    disabled,         // selection cleared and the display request released
};

class Model {
public:
    using TimePoint = std::chrono::steady_clock::time_point;
    // Monotonic time source. The adapter passes the real steady clock; tests
    // pass a controllable monotonic source so no real waiting is required.
    using Clock = std::function<TimePoint()>;

    // Battery protection threshold in percent; defaults to 20 per the
    // product baseline. Anything outside 10/20/30 falls back to the default.
    explicit Model(int low_battery_threshold = default_battery_threshold,
                   Clock clock = {});

    // User intents. start() reports whether the keep-awake session began.
    // Every successful start uses the defaults: system request, display may
    // turn off, no time limit.
    [[nodiscard]] bool start();
    // Stop request. An ordinary manual stop is the least urgent event, so a
    // deadline that has already been reached is applied first and the session
    // keeps the expired reason; sleep, battery and failure stops keep their own
    // reason immediately. The monotonic clock is read only when the running
    // session has a deadline, so stopping an unlimited session never reads it.
    // Stopping never starts a session.
    void stop(StopReason reason = StopReason::manual);
    // Flips the session: start when stopped, stop otherwise. The stop uses the
    // same rule as stop(), so a toggle at or past the deadline reports expired
    // and never restarts.
    [[nodiscard]] bool toggle();

    // Screen keep-awake for this session only. Never persisted. A stop of any
    // kind clears the selection, so the next start defaults to display-off.
    [[nodiscard]] DisplayToggle toggle_display();

    // Keep-awake duration for this session only; never persisted. Selecting a
    // finite duration restarts the countdown from the current moment. The
    // selection is retained as the (disabled) menu state after a stop, while
    // the next start always uses the product default of no time limit.
    [[nodiscard]] DurationChange set_duration(Duration duration);

    // User threshold selection. The adapter samples the battery immediately
    // before calling this, so a running session stops as soon as the new
    // threshold is reached. A stopped session never restarts from here.
    [[nodiscard]] ThresholdChange set_threshold(int percent);

    // Persistence feedback for the tooltip. A failed settings write keeps the
    // in-memory choice but is reported as not saved.
    void set_threshold_persisted(bool persisted) noexcept { threshold_persisted_ = persisted; }
    [[nodiscard]] bool threshold_persisted() const noexcept { return threshold_persisted_; }

    // Platform inputs.
    void observe(const BatterySample& sample);
    void observe_session(SessionState state);
    void on_request_result(bool accepted, int error_code = 0);
    void on_display_result(bool accepted, int error_code = 0);
    void on_release_result(bool released);
    void on_display_release_result(bool released, int error_code = 0);

    // Adapter contract for stale queued actions. A display-only release that
    // reaches the adapter after the session stopped must release everything;
    // issuing the display-only call would re-apply the system request.
    [[nodiscard]] bool requires_full_release(Action action) const noexcept {
        return action == Action::release_request ||
               (action == Action::release_display_request && !running_);
    }

    // Observable state. `running()` is the requested/confirmed keep-awake
    // session; `request_confirmed()` reports that the system accepted it.
    [[nodiscard]] bool running() const noexcept { return running_; }
    [[nodiscard]] bool request_confirmed() const noexcept { return confirmed_; }
    [[nodiscard]] bool release_unconfirmed() const noexcept { return release_unconfirmed_; }
    [[nodiscard]] bool display_selected() const noexcept { return display_selected_; }
    [[nodiscard]] bool display_confirmed() const noexcept { return display_confirmed_; }
    [[nodiscard]] bool display_release_unconfirmed() const noexcept { return display_release_unconfirmed_; }
    [[nodiscard]] SessionState session_state() const noexcept { return session_state_; }
    [[nodiscard]] bool display_suppressed() const noexcept { return session_state_ != SessionState::unlocked; }
    [[nodiscard]] StopReason stop_reason() const noexcept { return reason_; }
    [[nodiscard]] RequestTarget failed_request() const noexcept { return failed_request_; }
    [[nodiscard]] int threshold() const noexcept { return threshold_; }
    [[nodiscard]] Duration duration() const noexcept { return duration_; }
    // Time left in the running session, rounded up to whole seconds; empty
    // when stopped or unlimited.
    [[nodiscard]] std::optional<std::chrono::seconds> remaining_time() const;

    [[nodiscard]] std::vector<Action> take_actions();
    [[nodiscard]] std::vector<std::wstring> take_notifications();
    [[nodiscard]] std::wstring status_text() const;

private:
    // Monotonic clock bookkeeping. A stale (older) reading is ignored so an
    // out-of-order callback cannot rewind or extend the deadline.
    void advance_time() noexcept;
    // Stops the running session when its deadline has been reached.
    [[nodiscard]] bool expire_if_due();
    // Ordinary inputs update the clock and apply an already-reached deadline
    // before they are handled.
    void refresh_time();
    [[nodiscard]] std::wstring remaining_text() const;

    [[nodiscard]] std::optional<StopReason> gate(const BatterySample& sample) const;
    void stop_running(StopReason reason, RequestTarget failed, int error_code);
    void enter_stop(StopReason reason, bool was_running);
    void clear_display_state() noexcept;
    void reset_notifications();
    void notify_fault(std::wstring message);

    int threshold_{default_battery_threshold};
    bool threshold_persisted_{true};
    Clock clock_;
    TimePoint now_{};
    Duration duration_{Duration::unlimited};
    std::optional<TimePoint> deadline_{};
    BatterySample sample_{};
    SessionState session_state_{SessionState::unknown};
    bool running_{};
    bool confirmed_{};
    bool release_unconfirmed_{};
    bool display_selected_{};
    bool display_confirmed_{};
    bool display_pending_{};
    bool display_release_unconfirmed_{};
    StopReason reason_{StopReason::none};
    RequestTarget failed_request_{RequestTarget::system};
    int last_error_{};
    bool low_battery_notified_{};
    bool unknown_notified_{};
    bool fault_notified_{};
    std::vector<Action> actions_;
    std::vector<std::wstring> notifications_;
};

} // namespace unsleep
