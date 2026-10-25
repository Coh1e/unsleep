#include "unsleep/core.hpp"
#include "unsleep/settings.hpp"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

int checks = 0;

void check(bool value, const char* description) {
    ++checks;
    if (!value) throw std::runtime_error(description);
}

void check(bool value, const std::string& description) {
    ++checks;
    if (!value) throw std::runtime_error(description);
}

using namespace unsleep;

bool has(const std::wstring& text, std::wstring_view needle) {
    return text.find(needle) != std::wstring::npos;
}

bool contains(const std::vector<std::wstring>& list, std::wstring_view needle) {
    return std::ranges::any_of(list, [&](const std::wstring& text) { return has(text, needle); });
}

const BatterySample no_battery{BatteryPresence::absent, AcState::unplugged, std::nullopt, false};
const BatterySample unknown_battery{BatteryPresence::unknown, AcState::unknown, std::nullopt, false};
const BatterySample read_failure{BatteryPresence::unknown, AcState::unknown, std::nullopt, true};

BatterySample battery(AcState ac, std::optional<int> percent) {
    return BatterySample{BatteryPresence::present, ac, percent, false};
}

// Drives the adapter protocol: executes queued actions, feeding results back.
// The clock is a controllable monotonic source, so countdown behaviour is
// tested without any real waiting; the model itself only sees monotonic time.
struct Driver {
    Model::TimePoint clock_now{};
    Model model;
    int applies{};
    int releases{};
    int display_applies{};
    int display_releases{};
    bool apply_ok{true};
    bool release_ok{true};
    bool display_apply_ok{true};
    bool display_release_ok{true};
    std::vector<std::wstring> notifications;

    explicit Driver(int threshold = 20)
        : model(threshold, [this] { return clock_now; }) {}

    // Advances the monotonic clock without touching the model, exactly like
    // real time passing between two event-loop iterations.
    void advance(std::chrono::seconds seconds) { clock_now += seconds; }

    // A stale or out-of-order reading delivered late must not rewind the
    // model's clock or extend a deadline.
    void set_clock(Model::TimePoint moment) { clock_now = moment; }

    void drain() {
        for (;;) {
            const auto actions = model.take_actions();
            if (actions.empty()) break;
            for (const auto action : actions) {
                if (action == Action::apply_system_request) {
                    ++applies;
                    model.on_request_result(apply_ok, apply_ok ? 0 : 5);
                } else if (action == Action::apply_display_request) {
                    ++display_applies;
                    model.on_display_result(display_apply_ok, display_apply_ok ? 0 : 6);
                } else if (action == Action::release_display_request) {
                    ++display_releases;
                    model.on_display_release_result(display_release_ok, display_release_ok ? 0 : 7);
                } else {
                    ++releases;
                    model.on_release_result(release_ok);
                }
            }
        }
        for (auto& text : model.take_notifications()) notifications.push_back(std::move(text));
    }
};

void defaults_and_lifecycle() {
    Driver driver;
    driver.model.observe(no_battery);
    check(!driver.model.running(), "Initial state is stopped");
    check(driver.model.status_text() == L"已停止", "Initial status text");

    check(driver.model.start(), "Start succeeds with clear power data");
    check(driver.model.running(), "Start enters running state");
    check(!driver.model.request_confirmed(), "Request is not reported as confirmed before the system result");
    check(has(driver.model.status_text(), L"正在请求"), "Pending request is shown honestly");
    auto actions = driver.model.take_actions();
    check(actions.size() == 1 && actions[0] == Action::apply_system_request, "Start queues one system request");
    check(driver.model.take_notifications().empty(), "Normal start is silent");
    ++driver.applies;
    driver.model.on_request_result(true);
    driver.drain();
    check(driver.applies == 1, "System request applied once");
    check(driver.model.request_confirmed(), "System accepted the request");
    check(has(driver.model.status_text(), L"运行中"), "Running state shown");
    check(has(driver.model.status_text(), L"允许息屏"), "Default session allows the display to sleep");
    check(has(driver.model.status_text(), L"不限时"), "Default session has no time limit");

    const auto stop_actions = [&] {
        driver.model.stop();
        return driver.model.take_actions();
    }();
    check(!driver.model.running(), "Manual stop leaves the app stopped");
    check(stop_actions.size() == 1 && stop_actions[0] == Action::release_request, "Stop queues one release");
    driver.model.on_release_result(true);
    check(!driver.model.release_unconfirmed(), "Successful release is confirmed");
    check(driver.model.stop_reason() == StopReason::manual, "Manual stop reason recorded");
    check(has(driver.model.status_text(), L"手动"), "Manual stop reason displayed");
    check(driver.model.take_notifications().empty(), "Normal stop is silent");

    driver.model.stop();
    check(driver.model.take_actions().empty(), "Repeated stop is idempotent");
    check(driver.model.take_notifications().empty(), "Repeated stop does not notify");

    check(driver.model.start(), "Restart after stop succeeds");
    check(!driver.model.request_confirmed(), "Restart requires a fresh confirmation");
    driver.drain();
    check(driver.applies == 2, "Restart applies the system request again");
    check(driver.model.request_confirmed(), "Restart confirmed");

    check(driver.model.start(), "Repeated start keeps the running session");
    check(driver.model.take_actions().empty(), "Repeated start does not duplicate the request");

    const auto toggle_off = driver.model.toggle();
    check(!toggle_off && !driver.model.running(), "Toggle stops a running session");
    check(driver.model.stop_reason() == StopReason::manual, "Toggle stop is manual");
    const auto toggle_on = driver.model.toggle();
    check(toggle_on && driver.model.running(), "Toggle starts a stopped session");
}

void battery_gate_at_start() {
    {
        Driver driver;
        driver.model.observe(battery(AcState::plugged, 5));
        check(driver.model.start(), "Plugged-in low charge may run");
        driver.drain();
        check(driver.model.request_confirmed() && driver.notifications.empty(), "Plugged-in start is silent");
    }
    {
        Driver driver;
        driver.model.observe(battery(AcState::unplugged, 20));
        check(!driver.model.start(), "Unplugged at threshold refuses to start");
        check(driver.model.stop_reason() == StopReason::low_battery, "Refusal reason is low battery");
        check(driver.model.take_actions().empty(), "Refusal does not request the system");
        check(contains(driver.model.take_notifications(), L"电量低"), "Refusal notifies low battery once");
        check(has(driver.model.status_text(), L"20%"), "Status shows the threshold");
    }
    {
        Driver driver;
        driver.model.observe(battery(AcState::unplugged, 21));
        check(driver.model.start(), "Unplugged above threshold may run");
        driver.drain();
        check(driver.model.request_confirmed(), "Above-threshold start confirmed");
    }
    {
        Driver driver;
        driver.model.observe(battery(AcState::unplugged, 19));
        check(!driver.model.start(), "Unplugged below threshold refuses to start");
    }
    {
        Driver driver(10);
        driver.model.observe(battery(AcState::unplugged, 10));
        check(!driver.model.start(), "Custom threshold equal value refuses");
        driver.model.observe(battery(AcState::unplugged, 11));
        check(driver.model.start(), "Custom threshold above value allows");
    }
    {
        Driver driver(30);
        driver.model.observe(battery(AcState::unplugged, 30));
        check(!driver.model.start(), "Thirty percent threshold equal value refuses");
        driver.model.observe(battery(AcState::unplugged, 31));
        check(driver.model.start(), "Thirty percent threshold above value allows");
    }
    {
        Driver driver;
        driver.model.observe(unknown_battery);
        check(!driver.model.start(), "Unknown power source refuses to start");
        check(driver.model.stop_reason() == StopReason::battery_unknown, "Unknown source reason recorded");
        check(contains(driver.model.take_notifications(), L"电池状态未知"), "Unknown source notifies once");
    }
    {
        Driver driver;
        driver.model.observe(battery(AcState::unknown, 80));
        check(!driver.model.start(), "Unknown AC state refuses even with known charge");
    }
    {
        Driver driver;
        driver.model.observe(BatterySample{BatteryPresence::unknown, AcState::plugged, std::nullopt, false});
        check(driver.model.start(), "Confirmed AC allows start despite unknown battery presence");
        driver.drain();
        check(driver.model.request_confirmed(), "Unknown presence on AC is confirmed");
        check(driver.model.take_notifications().empty(), "Unknown presence on AC stays silent");
    }
    {
        Driver driver;
        driver.model.observe(
            BatterySample{BatteryPresence::unknown, AcState::plugged, std::nullopt, true});
        check(!driver.model.start(), "Read failure still refuses even with plugged AC flag");
        check(driver.model.stop_reason() == StopReason::battery_unknown, "Read failure keeps unknown reason");
    }
    {
        Driver driver;
        driver.model.observe(battery(AcState::unplugged, std::nullopt));
        check(!driver.model.start(), "Unknown charge while unplugged refuses");
    }
    {
        Driver driver;
        driver.model.observe(read_failure);
        check(!driver.model.start(), "Power read failure refuses to start");
        check(contains(driver.model.take_notifications(), L"电池状态未知"), "Read failure notifies as unknown");
        driver.model.observe(read_failure);
        check(driver.model.take_notifications().empty(), "Repeated unknown samples do not notify again");
    }
    {
        Driver driver;
        driver.model.observe(no_battery);
        check(driver.model.start(), "Absent battery may run");
    }
}

void battery_gate_while_running() {
    Driver driver;
    driver.model.observe(battery(AcState::unplugged, 25));
    check(driver.model.start(), "Start at 25 percent");
    driver.drain();
    check(driver.model.request_confirmed(), "25 percent session confirmed");

    driver.model.observe(battery(AcState::unplugged, 20));
    check(!driver.model.running(), "Reaching the threshold stops the session");
    check(driver.model.stop_reason() == StopReason::low_battery, "Runtime low battery reason");
    check(driver.releases == 0, "Release waits for the adapter");
    driver.drain();
    check(driver.releases == 1, "Low battery stop releases the system request");
    check(driver.notifications.size() == 1, "Low battery stop notifies exactly once");

    driver.model.observe(battery(AcState::unplugged, 19));
    driver.model.observe(battery(AcState::unplugged, 18));
    driver.drain();
    check(driver.notifications.size() == 1, "Further low samples do not notify again");
    check(driver.applies == 1, "Further samples never re-request the system");

    driver.model.observe(battery(AcState::plugged, 100));
    driver.model.observe(no_battery);
    driver.drain();
    check(!driver.model.running(), "Plugging in does not auto-restart");
    check(driver.applies == 1, "No automatic system request after recovery");

    check(driver.model.start(), "Manual restart after recovery is allowed");
    driver.drain();
    check(driver.model.request_confirmed(), "Manual restart confirmed");
    check(driver.notifications.size() == 1, "Manual restart is silent");

    driver.model.observe(battery(AcState::unplugged, 10));
    check(!driver.model.running(), "Second session also stops on low battery");
    driver.drain();
    check(driver.notifications.size() == 2, "Low battery notifies once per session");

    driver.model.observe(unknown_battery);
    check(driver.model.take_notifications().empty(), "Unknown sample while stopped stays silent");
    check(driver.model.stop_reason() == StopReason::low_battery, "Stopped state keeps the first reason");
}

void unknown_during_run() {
    Driver driver;
    driver.model.observe(battery(AcState::plugged, 50));
    check(driver.model.start(), "Start on AC");
    driver.drain();
    driver.model.observe(unknown_battery);
    check(!driver.model.running(), "Unknown data stops the running session");
    check(driver.model.stop_reason() == StopReason::battery_unknown, "Unknown stop reason");
    driver.drain();
    check(driver.releases == 1, "Unknown stop releases");
    check(driver.notifications.size() == 1 && has(driver.notifications.front(), L"电池状态未知"),
          "Unknown stop notifies once");
    driver.model.observe(unknown_battery);
    driver.model.observe(read_failure);
    driver.drain();
    check(driver.notifications.size() == 1, "Repeated unknown events do not notify again");
    check(!driver.model.running(), "Repeated unknown events do not restart");
}

void request_feedback() {
    Driver driver;
    driver.apply_ok = false;
    driver.model.observe(no_battery);
    check(driver.model.start(), "Start is requested");
    driver.drain();
    check(!driver.model.running(), "Failed system request stops the session");
    check(!driver.model.request_confirmed(), "Failed request is never reported as confirmed");
    check(driver.model.stop_reason() == StopReason::request_failed, "Request failure reason recorded");
    check(driver.releases == 1, "Failed request is reverted");
    check(driver.notifications.size() == 1 && has(driver.notifications.front(), L"系统请求失败"),
          "Request failure notifies once");
    check(has(driver.model.status_text(), L"系统请求失败"), "Request failure displayed");

    driver.model.on_request_result(false, 5);
    check(driver.model.take_notifications().empty(), "Repeated failure callbacks do not notify");
    check(driver.model.take_actions().empty(), "Repeated failure callbacks do not queue work");

    driver.apply_ok = true;
    check(driver.model.start(), "Start after failure is allowed");
    driver.drain();
    check(driver.model.request_confirmed(), "Start after failure confirmed");

    driver.release_ok = false;
    driver.model.stop(StopReason::sleep);
    driver.drain();
    check(driver.model.release_unconfirmed(), "Failed release is reported as unconfirmed");
    check(has(driver.model.status_text(), L"释放未确认"), "Unconfirmed release displayed");
    check(has(driver.model.status_text(), L"系统睡眠"), "Sleep stop reason displayed");
}

void late_callbacks_and_idempotency() {
    Driver driver;
    driver.model.observe(no_battery);
    check(driver.model.start(), "Start before late result");
    driver.model.stop();
    auto actions = driver.model.take_actions();
    check(actions.size() == 1 && actions[0] == Action::release_request, "Stop before result queues release");
    driver.model.on_request_result(true);
    actions = driver.model.take_actions();
    check(actions.size() == 1 && actions[0] == Action::release_request,
          "Late success after stop queues another release");
    check(!driver.model.running(), "Late success does not revive the session");
    driver.model.on_release_result(true);
    driver.model.on_release_result(true);
    check(driver.model.take_actions().empty(), "Repeated release results queue nothing");
    check(!driver.model.release_unconfirmed(), "Repeated successful releases stay confirmed");

    driver.model.observe(no_battery);
    check(driver.model.take_actions().empty(), "Observation while stopped queues nothing");
    check(!driver.model.running(), "Observation while stopped never starts");
}

void unknown_presence_on_ac_while_running() {
    Driver driver;
    driver.model.observe(battery(AcState::unplugged, 60));
    check(driver.model.start(), "Start on battery above threshold");
    driver.drain();
    check(driver.model.request_confirmed(), "Battery session confirmed");

    driver.model.observe(BatterySample{BatteryPresence::unknown, AcState::plugged, std::nullopt, false});
    check(driver.model.running(), "Unknown battery presence does not stop a confirmed-AC session");
    driver.drain();
    check(driver.releases == 0, "Unknown presence on AC does not release the request");
    check(driver.notifications.empty(), "Unknown presence on AC does not notify");
}

void manual_start_after_stopped_data_change() {
    Driver driver;
    driver.model.observe(battery(AcState::plugged, 80));
    check(driver.model.start(), "Initial start on AC");
    driver.drain();
    check(driver.model.request_confirmed(), "Initial start confirmed");
    check(driver.applies == 1, "Initial start applied once");

    driver.model.stop(StopReason::manual);
    driver.drain();
    check(!driver.model.running(), "Stopped before changing battery data");
    check(driver.releases == 1, "Manual stop released the request");

    // The periodic sample changes while stopped. The next manual start must
    // be gated by that latest data rather than a stale pre-stop sample.
    driver.model.observe(battery(AcState::unplugged, 5));
    check(!driver.model.start(), "Stopped-state low battery refuses the next manual start");
    check(driver.model.stop_reason() == StopReason::low_battery, "Refusal uses the latest low battery sample");
    check(driver.applies == 1, "Refused manual start applies nothing");
    check(driver.model.take_actions().empty(), "Refused manual start queues no request");

    driver.model.observe(unknown_battery);
    check(!driver.model.start(), "Stopped-state unknown battery refuses the next manual start");
    check(driver.model.stop_reason() == StopReason::battery_unknown, "Refusal uses the latest unknown sample");
    check(driver.applies == 1, "Unknown refusal applies nothing");

    // Plugging in after the stopped-state change allows a manual start again.
    driver.model.observe(battery(AcState::plugged, 5));
    check(driver.model.start(), "Manual start after plugging in succeeds");
    driver.drain();
    check(driver.applies == 2, "Manual restart applies the request again");
    check(driver.model.request_confirmed(), "Manual restart confirmed");
}

void threshold_matrix() {
    for (const int threshold : battery_thresholds) {
        const std::string label = std::to_string(threshold) + "%";
        {
            Driver driver(threshold);
            check(driver.model.threshold() == threshold, "Threshold accepted: " + label);
            driver.model.observe(battery(AcState::unplugged, threshold - 1));
            check(!driver.model.start(), "Below threshold refuses start: " + label);
            check(driver.model.stop_reason() == StopReason::low_battery,
                  "Below threshold reason: " + label);
            check(driver.model.take_actions().empty(), "Below threshold applies nothing: " + label);
            check(contains(driver.model.take_notifications(), L"电量低"),
                  "Below threshold notifies once: " + label);
        }
        {
            Driver driver(threshold);
            driver.model.observe(battery(AcState::unplugged, threshold));
            check(!driver.model.start(), "At threshold refuses start: " + label);
            check(driver.model.stop_reason() == StopReason::low_battery,
                  "At threshold reason: " + label);
        }
        {
            Driver driver(threshold);
            driver.model.observe(battery(AcState::unplugged, threshold + 1));
            check(driver.model.start(), "Above threshold starts: " + label);
            driver.drain();
            check(driver.model.request_confirmed(), "Above threshold confirmed: " + label);
        }
        {
            Driver driver(threshold);
            driver.model.observe(battery(AcState::plugged, 0));
            check(driver.model.start(), "Plugged in at zero charge starts: " + label);
            driver.drain();
            check(driver.model.request_confirmed(), "Plugged in start confirmed: " + label);
        }
        {
            Driver driver(threshold);
            driver.model.observe(no_battery);
            check(driver.model.start(), "No battery starts: " + label);
        }
        {
            Driver driver(threshold);
            driver.model.observe(battery(AcState::unknown, 80));
            check(!driver.model.start(), "Unknown source refuses: " + label);
            check(driver.model.stop_reason() == StopReason::battery_unknown,
                  "Unknown source reason: " + label);
            check(contains(driver.model.take_notifications(), L"电池状态未知"),
                  "Unknown source notifies: " + label);
        }
        {
            Driver driver(threshold);
            driver.model.observe(battery(AcState::unplugged, std::nullopt));
            check(!driver.model.start(), "Unknown charge refuses: " + label);
            check(driver.model.stop_reason() == StopReason::battery_unknown,
                  "Unknown charge reason: " + label);
        }
        {
            Driver driver(threshold);
            driver.model.observe(read_failure);
            check(!driver.model.start(), "Read failure refuses: " + label);
            check(driver.model.stop_reason() == StopReason::battery_unknown,
                  "Read failure reason: " + label);
        }
        {
            Driver driver(threshold);
            driver.model.observe(battery(AcState::unplugged, threshold + 5));
            check(driver.model.start(), "Runtime session starts: " + label);
            driver.drain();
            check(driver.model.request_confirmed(), "Runtime session confirmed: " + label);
            driver.model.observe(battery(AcState::unplugged, threshold));
            check(!driver.model.running(), "Runtime at threshold stops: " + label);
            check(driver.model.stop_reason() == StopReason::low_battery,
                  "Runtime threshold reason: " + label);
            driver.drain();
            check(driver.releases == 1, "Runtime threshold stop releases: " + label);
            check(driver.notifications.size() == 1, "Runtime threshold stop notifies once: " + label);
            driver.model.observe(battery(AcState::unplugged, threshold - 3));
            driver.model.observe(battery(AcState::plugged, 100));
            driver.model.observe(no_battery);
            driver.drain();
            check(!driver.model.running(), "Recovery does not auto restart: " + label);
            check(driver.applies == 1, "Recovery applies no new request: " + label);
            check(driver.notifications.size() == 1, "Recovery stays silent: " + label);
            check(driver.model.start(), "Manual restart after recovery: " + label);
        }
    }
}

void threshold_change_behavior() {
    // Raising the threshold above the current charge stops the running session
    // immediately, using the freshly observed sample.
    Driver driver(20);
    driver.model.observe(battery(AcState::unplugged, 25));
    check(driver.model.start(), "Running at 25% with a 20% threshold");
    driver.drain();
    check(driver.model.set_threshold(30) == ThresholdChange::stopped,
          "Raising the threshold to 30% stops the session");
    check(!driver.model.running(), "New threshold stopped the session");
    check(driver.model.stop_reason() == StopReason::low_battery, "New threshold stop reason");
    const auto actions = driver.model.take_actions();
    check(actions.size() == 1 && actions[0] == Action::release_request,
          "New threshold stop queues one release");
    check(driver.model.take_notifications().size() == 1, "New threshold stop notifies once");
    check(driver.model.threshold() == 30, "Selected threshold is kept");
    check(has(driver.model.status_text(), L"30%"), "Status shows the new threshold");

    // Threshold changes while stopped apply but never restart or notify.
    driver.model.observe(battery(AcState::plugged, 100));
    check(driver.model.set_threshold(20) == ThresholdChange::updated,
          "Stopped threshold change applies");
    check(driver.model.set_threshold(10) == ThresholdChange::updated,
          "Second stopped threshold change applies");
    driver.drain();
    check(!driver.model.running(), "Stopped threshold changes do not restart");
    check(driver.model.take_notifications().empty(), "Stopped threshold changes stay silent");

    // A raised threshold gates the next manual start.
    Driver gated(30);
    gated.model.observe(battery(AcState::unplugged, 25));
    check(!gated.model.start(), "Raised threshold gates the next manual start");
    check(gated.model.stop_reason() == StopReason::low_battery, "Gated start reason is low battery");

    // Lowering the threshold keeps a running session alive.
    Driver lowered(20);
    lowered.model.observe(battery(AcState::unplugged, 25));
    check(lowered.model.start(), "Running at 25% before lowering the threshold");
    lowered.drain();
    check(lowered.model.set_threshold(10) == ThresholdChange::updated,
          "Lowering the threshold reports an update");
    check(lowered.model.running(), "Lowering the threshold keeps the session");
    lowered.drain();
    check(lowered.releases == 0, "Lowering the threshold releases nothing");
    check(lowered.notifications.empty(), "Lowering the threshold stays silent");

    // Unsupported and repeated selections never change the model.
    Driver invalid(20);
    invalid.model.observe(no_battery);
    check(invalid.model.start(), "Session for invalid selection checks");
    invalid.drain();
    check(invalid.model.set_threshold(25) == ThresholdChange::rejected, "Unsupported threshold rejected");
    check(invalid.model.threshold() == 20, "Rejected threshold keeps the current value");
    check(invalid.model.set_threshold(20) == ThresholdChange::unchanged,
          "Selecting the current threshold is unchanged");
    check(invalid.model.running(), "Rejected selections leave the session running");

    // Save failure keeps the in-memory value and shows it only in the tooltip.
    Driver saved;
    saved.model.observe(no_battery);
    check(saved.model.threshold_persisted(), "Threshold starts as persisted");
    check(!has(saved.model.status_text(), L"未保存"), "Persisted threshold has no unsaved note");
    saved.model.set_threshold_persisted(false);
    check(!saved.model.threshold_persisted(), "Failed save recorded");
    check(has(saved.model.status_text(), L"电池阈值设置未保存"), "Unsaved note appears in the tooltip text");
    check(saved.model.take_notifications().empty(), "Unsaved note creates no popup notification");
    saved.model.set_threshold_persisted(true);
    check(!has(saved.model.status_text(), L"未保存"), "Successful save clears the unsaved note");
}

void invalid_threshold_falls_back() {
    Driver low(5);
    check(low.model.threshold() == 20, "Unsupported threshold 5 falls back to 20");
    Driver high(100);
    check(high.model.threshold() == 20, "Unsupported threshold 100 falls back to 20");
    Driver zero(0);
    check(zero.model.threshold() == 20, "Unsupported threshold 0 falls back to 20");
    Driver negative(-10);
    check(negative.model.threshold() == 20, "Negative threshold falls back to 20");
    zero.model.observe(battery(AcState::unplugged, 19));
    check(!zero.model.start(), "Fallback threshold still gates 19%");
    zero.model.observe(battery(AcState::unplugged, 21));
    check(zero.model.start(), "Fallback threshold allows 21%");
}

void display_defaults_and_toggle() {
    Driver driver;
    driver.model.observe(no_battery);
    driver.model.observe_session(SessionState::unlocked);
    check(!driver.model.display_selected(), "Screen keep-awake defaults to off");
    check(driver.model.toggle_display() == DisplayToggle::rejected_stopped,
          "Screen keep-awake cannot be selected while stopped");
    check(driver.model.take_actions().empty(), "Stopped display toggle queues nothing");
    check(!driver.model.display_selected(), "Rejected display toggle keeps the option off");

    check(driver.model.start(), "Start before enabling screen keep-awake");
    auto actions = driver.model.take_actions();
    check(actions.size() == 1 && actions[0] == Action::apply_system_request,
          "Start applies only the system request");
    driver.model.on_request_result(true);
    check(!driver.model.display_selected(), "Start keeps screen keep-awake off");
    check(has(driver.model.status_text(), L"允许息屏"), "Default running status allows the display to sleep");
    check(!has(driver.model.status_text(), L"屏幕请求"), "No screen request is claimed by default");

    check(driver.model.toggle_display() == DisplayToggle::enabled,
          "Enabling screen keep-awake queues a request");
    check(driver.model.display_selected(), "Selection is recorded before the system result");
    check(!driver.model.display_confirmed(), "Selection alone is not reported as a confirmed request");
    actions = driver.model.take_actions();
    check(actions.size() == 1 && actions[0] == Action::apply_display_request,
          "Enabling queues the display request");
    check(has(driver.model.status_text(), L"正在请求屏幕常亮"), "Pending display request is shown honestly");
    driver.model.on_display_result(true);
    check(driver.model.display_confirmed(), "Accepted display request is confirmed");
    check(has(driver.model.status_text(), L"系统及屏幕请求已接受"),
          "Confirmed display request is shown separately from the system request");
    check(!has(driver.model.status_text(), L"允许息屏"), "Confirmed display request no longer allows screen off");

    check(driver.model.toggle_display() == DisplayToggle::disabled, "Disabling keeps the option off");
    check(!driver.model.display_selected() && !driver.model.display_confirmed(),
          "Disabling clears selection and confirmation");
    actions = driver.model.take_actions();
    check(actions.size() == 1 && actions[0] == Action::release_display_request,
          "Disabling releases only the display request");
    check(driver.model.running() && driver.model.request_confirmed(),
          "Disabling keeps the system request running");
    driver.model.on_display_release_result(true);
    check(!driver.model.display_release_unconfirmed(), "Display release is confirmed");
    check(has(driver.model.status_text(), L"允许息屏"), "Running status returns to display-off");
}

void display_lock_and_unlock() {
    Driver driver;
    driver.model.observe(no_battery);
    driver.model.observe_session(SessionState::unlocked);
    check(driver.model.start(), "Start before lock checks");
    driver.drain();
    check(driver.model.toggle_display() == DisplayToggle::enabled, "Enable display before locking");
    driver.drain();
    check(driver.model.display_confirmed(), "Display request confirmed before locking");

    int releases_before_lock = driver.releases;
    driver.model.observe_session(SessionState::locked);
    check(driver.model.session_state() == SessionState::locked, "Lock state getter reports locked");
    check(driver.model.running(), "Lock does not stop the keep-awake session");
    check(driver.model.request_confirmed(), "Lock keeps the system request confirmed");
    check(driver.model.display_selected(), "Lock keeps this session's display selection");
    check(!driver.model.display_confirmed(), "Lock withdraws the confirmed display request");
    check(has(driver.model.status_text(), L"锁屏期间已释放屏幕请求"), "Lock status reports the display release");
    auto actions = driver.model.take_actions();
    check(actions.size() == 1 && actions[0] == Action::release_display_request,
          "Lock releases only the display request");
    check(driver.releases == releases_before_lock, "Lock queues no full release before the adapter result");
    driver.model.observe_session(SessionState::locked);
    check(driver.model.take_actions().empty(), "Repeated lock events queue nothing");
    driver.model.on_display_release_result(true);
    check(!driver.model.display_release_unconfirmed(), "Lock display release is confirmed");

    driver.model.observe_session(SessionState::unlocked);
    actions = driver.model.take_actions();
    check(actions.size() == 1 && actions[0] == Action::apply_display_request,
          "Unlock restores the display request when still selected");
    driver.model.observe_session(SessionState::unlocked);
    check(driver.model.take_actions().empty(), "Repeated unlock events do not duplicate the request");
    driver.model.on_display_result(true);
    check(driver.model.display_confirmed(), "Restored display request is confirmed");
    check(driver.model.running() && driver.model.request_confirmed(), "Unlock keeps the system session");

    // A lock that arrives while the display request is still in flight must
    // not leave the display held after the late success.
    check(driver.model.toggle_display() == DisplayToggle::disabled, "Disable before the pending-lock case");
    driver.drain();
    check(driver.model.toggle_display() == DisplayToggle::enabled, "Request display again");
    actions = driver.model.take_actions();
    check(actions.size() == 1 && actions[0] == Action::apply_display_request,
          "Display request is pending");
    driver.model.observe_session(SessionState::locked);
    check(driver.model.take_actions().empty(), "Lock while pending queues no second request");
    driver.model.on_display_result(true);
    check(!driver.model.display_confirmed(), "Late display success after lock is not confirmed");
    check(driver.model.running() && driver.model.request_confirmed(), "Late display success keeps the system session");
    actions = driver.model.take_actions();
    check(actions.size() == 1 && actions[0] == Action::release_display_request,
          "Late display success after lock is withdrawn");
    driver.model.on_display_release_result(true);

    // Lock/unlock while stopped never applies or restores anything.
    driver.model.stop(StopReason::manual);
    driver.drain();
    check(!driver.model.display_selected(), "Stop clears the display selection");
    driver.model.observe_session(SessionState::locked);
    check(driver.model.take_actions().empty(), "Lock while stopped queues nothing");
    driver.model.observe_session(SessionState::unlocked);
    check(driver.model.take_actions().empty(), "Unlock while stopped queues nothing");
    check(!driver.model.running(), "Session events while stopped never restart");
}

void display_stop_paths() {
    // Manual and sleep stops clear the option; the next start is display-off.
    {
        Driver driver;
        driver.model.observe(no_battery);
        driver.model.observe_session(SessionState::unlocked);
        check(driver.model.start(), "Manual stop path starts");
        driver.drain();
        check(driver.model.toggle_display() == DisplayToggle::enabled, "Enable before manual stop");
        driver.drain();
        driver.model.stop(StopReason::manual);
        check(!driver.model.display_selected(), "Manual stop clears the display option");
        auto actions = driver.model.take_actions();
        check(actions.size() == 1 && actions[0] == Action::release_request,
              "Manual stop releases both requests");
        driver.drain();
        check(driver.model.start(), "Restart after manual stop");
        actions = driver.model.take_actions();
        check(actions.size() == 1 && actions[0] == Action::apply_system_request,
              "Restart defaults to display-off");
    }
    {
        Driver driver;
        driver.model.observe(no_battery);
        driver.model.observe_session(SessionState::unlocked);
        check(driver.model.start(), "Sleep stop path starts");
        driver.drain();
        check(driver.model.toggle_display() == DisplayToggle::enabled, "Enable before sleep stop");
        driver.drain();
        driver.model.stop(StopReason::sleep);
        check(!driver.model.display_selected(), "Sleep stop clears the display option");
        auto actions = driver.model.take_actions();
        check(actions.size() == 1 && actions[0] == Action::release_request,
              "Sleep stop releases both requests");
        driver.drain();
        check(!driver.model.running(), "Sleep stop stays stopped");
    }
    // Low-battery stop (including a threshold-driven stop) clears the option.
    {
        Driver driver;
        driver.model.observe(battery(AcState::unplugged, 50));
        driver.model.observe_session(SessionState::unlocked);
        check(driver.model.start(), "Battery stop path starts");
        driver.drain();
        check(driver.model.toggle_display() == DisplayToggle::enabled, "Enable before low battery");
        driver.drain();
        check(driver.model.display_confirmed(), "Display confirmed before low battery");
        driver.model.observe(battery(AcState::unplugged, 20));
        check(!driver.model.running(), "Low battery stops the session with display enabled");
        check(!driver.model.display_selected(), "Low battery clears the display option");
        auto actions = driver.model.take_actions();
        check(actions.size() == 1 && actions[0] == Action::release_request,
              "Low battery releases both requests");
        check(contains(driver.model.take_notifications(), L"电量低"), "Low battery notifies once");
        driver.model.observe(battery(AcState::plugged, 100));
        check(driver.model.start(), "Manual restart after low battery");
        actions = driver.model.take_actions();
        check(actions.size() == 1 && actions[0] == Action::apply_system_request,
              "Restart after low battery defaults to display-off");
    }
    // A running threshold change that stops the session also clears the option.
    {
        Driver driver(20);
        driver.model.observe(battery(AcState::unplugged, 25));
        driver.model.observe_session(SessionState::unlocked);
        check(driver.model.start(), "Threshold stop path starts");
        driver.drain();
        check(driver.model.toggle_display() == DisplayToggle::enabled, "Enable before threshold stop");
        driver.drain();
        check(driver.model.set_threshold(30) == ThresholdChange::stopped,
              "Raised threshold stops the session");
        check(!driver.model.display_selected(), "Threshold stop clears the display option");
        auto actions = driver.model.take_actions();
        check(actions.size() == 1 && actions[0] == Action::release_request,
              "Threshold stop releases both requests");
    }
}

void display_failures() {
    // Display request failure stops the session, revokes the option and makes
    // the next start default to display-off.
    Driver driver;
    driver.model.observe(no_battery);
    driver.model.observe_session(SessionState::unlocked);
    check(driver.model.start(), "Start before display failure");
    driver.drain();
    driver.display_apply_ok = false;
    check(driver.model.toggle_display() == DisplayToggle::enabled, "Request display before failure");
    driver.drain();
    check(!driver.model.running(), "Failed display request stops the session");
    check(driver.model.stop_reason() == StopReason::request_failed, "Display failure reason recorded");
    check(driver.model.failed_request() == RequestTarget::display, "Failure target is the display request");
    check(!driver.model.display_selected() && !driver.model.display_confirmed(),
          "Failed display request clears the selection");
    check(driver.releases == 1, "Failed display request is reverted with a full release");
    check(driver.notifications.size() == 1 && has(driver.notifications.front(), L"屏幕请求失败"),
          "Display failure notifies once with the right request");
    check(has(driver.model.status_text(), L"屏幕请求失败"), "Display failure is shown honestly");
    check(!has(driver.model.status_text(), L"系统请求失败"), "Display failure is not mislabelled as a system failure");
    driver.model.on_display_result(false, 6);
    check(driver.model.take_notifications().empty(), "Repeated display failure callbacks do not notify");
    check(driver.model.take_actions().empty(), "Repeated display failure callbacks queue nothing");
    driver.display_apply_ok = true;
    check(driver.model.start(), "Start after display failure");
    driver.drain();
    check(driver.model.request_confirmed(), "Start after display failure is confirmed");
    check(!driver.model.display_selected(), "Start after display failure defaults to display-off");

    // A failed display release during lock stops the session, clears the
    // option and asks for a full release instead of retrying the display-only
    // call.
    Driver release;
    release.model.observe(no_battery);
    release.model.observe_session(SessionState::unlocked);
    check(release.model.start(), "Start before display release failure");
    release.drain();
    check(release.model.toggle_display() == DisplayToggle::enabled, "Enable before release failure");
    release.drain();
    release.display_release_ok = false;
    release.model.observe_session(SessionState::locked);
    release.drain();
    check(!release.model.running(), "Failed display release during lock stops the session");
    check(release.model.stop_reason() == StopReason::request_failed,
          "Failed display release records the failure reason");
    check(release.model.failed_request() == RequestTarget::display,
          "Failed display release reports the display target");
    check(!release.model.display_selected() && !release.model.display_confirmed(),
          "Failed display release clears the selection");
    check(release.releases == 1, "Failed display release attempts one full release");
    check(release.display_releases == 1, "Failed display release is not retried");
    check(release.notifications.size() == 1 && has(release.notifications.front(), L"屏幕请求释放失败"),
          "Failed display release notifies once with the release error");
    check(has(release.model.status_text(), L"屏幕请求失败"),
          "Failed display release is shown with the display target");
    release.model.on_display_release_result(false, 7);
    check(release.model.take_notifications().empty(),
          "Repeated display release failures do not notify again");
    check(release.model.take_actions().empty(),
          "Repeated display release failures do not queue retries");
    check(release.model.start(), "Restart after a failed display release");
    release.drain();
    check(!release.model.display_selected(), "Restart after a failed display release defaults to display-off");

    // The same failure while manually turning the option off stops the session.
    Driver manual;
    manual.model.observe(no_battery);
    manual.model.observe_session(SessionState::unlocked);
    check(manual.model.start(), "Start before manual display-off failure");
    manual.drain();
    check(manual.model.toggle_display() == DisplayToggle::enabled, "Enable before manual display-off failure");
    manual.drain();
    manual.display_release_ok = false;
    check(manual.model.toggle_display() == DisplayToggle::disabled,
          "Manual display-off failure still clears the selection first");
    manual.drain();
    check(!manual.model.running(), "Failed manual display-off stops the session");
    check(manual.model.stop_reason() == StopReason::request_failed,
          "Failed manual display-off records the failure reason");
    check(manual.releases == 1 && manual.display_releases == 1,
          "Failed manual display-off attempts one full release and no retry");
    check(manual.notifications.size() == 1 && has(manual.notifications.front(), L"屏幕请求释放失败"),
          "Failed manual display-off notifies once");

    // If the follow-up full release also fails, both release notes stay honest.
    Driver release_full;
    release_full.model.observe(no_battery);
    release_full.model.observe_session(SessionState::unlocked);
    check(release_full.model.start(), "Start before failed follow-up full release");
    release_full.drain();
    check(release_full.model.toggle_display() == DisplayToggle::enabled,
          "Enable before failed follow-up full release");
    release_full.drain();
    release_full.display_release_ok = false;
    release_full.release_ok = false;
    release_full.model.observe_session(SessionState::locked);
    release_full.drain();
    check(!release_full.model.running(), "Failed full release after display failure stays stopped");
    check(release_full.model.release_unconfirmed() && release_full.model.display_release_unconfirmed(),
          "Failed follow-up full release leaves both releases unconfirmed");
    check(has(release_full.model.status_text(), L"系统请求释放未确认") &&
              has(release_full.model.status_text(), L"屏幕请求释放未确认"),
          "Failed follow-up full release is shown honestly");

    // A late display success after a stop must not revive the session.
    Driver late;
    late.model.observe(no_battery);
    late.model.observe_session(SessionState::unlocked);
    check(late.model.start(), "Start before late display success");
    late.drain();
    check(late.model.toggle_display() == DisplayToggle::enabled, "Request display before late success");
    auto actions = late.model.take_actions();
    check(actions.size() == 1 && actions[0] == Action::apply_display_request, "Display request pending");
    late.model.stop(StopReason::manual);
    actions = late.model.take_actions();
    check(actions.size() == 1 && actions[0] == Action::release_request, "Stop before the late result releases both");
    late.model.on_display_result(true);
    check(!late.model.running(), "Late display success does not revive the session");
    check(!late.model.display_selected() && !late.model.display_confirmed(),
          "Late display success is not recorded as the selection");
    actions = late.model.take_actions();
    check(actions.size() == 1 && actions[0] == Action::release_request,
          "Late display success after stop is released again without re-requesting the system");
}

void stale_action_targets() {
    Driver driver;
    driver.model.observe(no_battery);
    driver.model.observe_session(SessionState::unlocked);
    check(driver.model.start(), "Start before stale-action target checks");
    driver.drain();
    check(!driver.model.requires_full_release(Action::release_display_request),
          "A running display release stays display-only");
    check(driver.model.requires_full_release(Action::release_request),
          "A full release always targets the system request");

    driver.model.stop(StopReason::manual);
    check(driver.model.requires_full_release(Action::release_display_request),
          "A display-only action arriving after stop targets a full release");
    driver.drain();
    check(!driver.model.running() && !driver.model.release_unconfirmed(),
          "The stale action releases everything without reviving the session");

    driver.model.on_display_release_result(false, 9);
    check(driver.model.take_notifications().empty(),
          "A late display release failure after stop does not notify");
    check(driver.model.take_actions().empty(),
          "A late display release failure after stop queues no retry");
    check(!driver.model.display_release_unconfirmed(),
          "A late display release failure after stop does not invent a second release state");
}

void display_unknown_session_state() {
    Driver driver;
    driver.model.observe(no_battery);
    check(driver.model.session_state() == SessionState::unknown,
          "Initial session lock state is not assumed unlocked");
    driver.model.observe_session(SessionState::locked);
    check(driver.model.session_state() == SessionState::locked,
          "A locked initial state is applied from unknown");
    driver.model.observe_session(SessionState::unknown);
    check(driver.model.session_state() == SessionState::unknown,
          "An unknown state can be applied explicitly");
    check(driver.model.start(), "Start with an unknown initial session state");
    driver.drain();
    check(driver.model.toggle_display() == DisplayToggle::deferred_locked,
          "Display selection waits while the lock state is unknown");
    check(driver.model.display_selected(), "Deferred display selection is remembered");
    check(!driver.model.display_confirmed(), "Deferred display request is not reported as success");
    check(driver.model.take_actions().empty(), "Deferred display selection queues no request");
    check(has(driver.model.status_text(), L"锁定状态未知"), "Unknown lock state is reported");
    driver.model.observe_session(SessionState::unknown);
    check(driver.model.take_actions().empty(), "Repeated unknown state queues nothing");
    driver.model.observe_session(SessionState::unlocked);
    auto actions = driver.model.take_actions();
    check(actions.size() == 1 && actions[0] == Action::apply_display_request,
          "Unlock applies the deferred selection");
    driver.model.on_display_result(true);
    check(driver.model.display_confirmed(), "Deferred display request can be confirmed");

    // A full release failure leaves both releases unconfirmed.
    Driver full;
    full.model.observe(no_battery);
    full.model.observe_session(SessionState::unlocked);
    check(full.model.start(), "Start before a failed full release");
    full.drain();
    check(full.model.toggle_display() == DisplayToggle::enabled, "Enable before a failed full release");
    full.drain();
    full.release_ok = false;
    full.model.stop(StopReason::manual);
    full.drain();
    check(full.model.release_unconfirmed(), "Failed full release is unconfirmed");
    check(full.model.display_release_unconfirmed(), "Failed full release also leaves the display unconfirmed");
    check(has(full.model.status_text(), L"系统请求释放未确认"), "System release note is shown");
    check(has(full.model.status_text(), L"屏幕请求释放未确认"), "Display release note is shown");
    check(!full.model.display_selected(), "Failed full release still clears the display option");
}


// ---------------------------------------------------------------------------
// Duration (C2): four tiers, deadline boundaries, retiming, restart reset.
// ---------------------------------------------------------------------------

using std::chrono::minutes;
using std::chrono::seconds;

std::optional<std::int64_t> remaining_seconds(Model& model) {
    // Reading the countdown is also a check point: make sure the model's clock
    // has caught up with the controllable clock first.
    model.observe(no_battery);
    const auto remaining = model.remaining_time();
    if (!remaining) return std::nullopt;
    return remaining->count();
}

Driver running_driver(int threshold = 20) {
    Driver driver(threshold);
    driver.model.observe(no_battery);
    driver.model.observe_session(SessionState::unlocked);
    check(driver.model.start(), "Duration fixture starts");
    driver.drain();
    check(driver.model.request_confirmed(), "Duration fixture confirmed");
    return driver;
}

void duration_tiers() {
    check(is_valid_duration(Duration::unlimited) && is_valid_duration(Duration::minutes_30) &&
              is_valid_duration(Duration::hours_1) && is_valid_duration(Duration::hours_2),
          "The four product durations are valid");
    check(!is_valid_duration(static_cast<Duration>(4)) && !is_valid_duration(static_cast<Duration>(-1)),
          "Durations outside the product set are invalid");
    check(duration_length(Duration::unlimited) == minutes{0}, "Unlimited has zero length");
    check(duration_length(Duration::minutes_30) == minutes{30}, "30 minutes length");
    check(duration_length(Duration::hours_1) == minutes{60}, "1 hour length");
    check(duration_length(Duration::hours_2) == minutes{120}, "2 hours length");

    Driver driver = running_driver();
    // Defaults: running, screen may sleep, no time limit.
    check(driver.model.duration() == Duration::unlimited, "Start defaults to unlimited duration");
    check(!driver.model.remaining_time(), "Unlimited session has no remaining time");
    check(has(driver.model.status_text(), L"不限时"), "Unlimited tooltip says unlimited");

    struct Tier {
        Duration duration;
        std::int64_t seconds;
    };
    const Tier tiers[] = {
        {Duration::minutes_30, 30 * 60},
        {Duration::hours_1, 60 * 60},
        {Duration::hours_2, 120 * 60},
    };
    for (const auto& tier : tiers) {
        const std::string label = std::to_string(tier.seconds) + "s";
        check(driver.model.set_duration(tier.duration) == DurationChange::updated,
              "Selecting a finite duration updates the session: " + label);
        check(driver.model.duration() == tier.duration, "Selected duration is reported: " + label);
        check(remaining_seconds(driver.model) == tier.seconds,
              "Finite duration counts down from now: " + label);
        check(has(driver.model.status_text(), L"剩余"), "Finite duration shows remaining time: " + label);
        check(!has(driver.model.status_text(), L"不限时"), "Finite duration does not claim unlimited: " + label);
        driver.drain();
        check(driver.applies == 1 && driver.releases == 0,
              "Switching duration does not re-request or release: " + label);
        check(driver.notifications.empty(), "Switching duration stays silent: " + label);
    }

    check(driver.model.set_duration(Duration::unlimited) == DurationChange::updated,
          "Switching back to unlimited updates the session");
    check(!driver.model.remaining_time(), "Unlimited clears the remaining time");
    check(has(driver.model.status_text(), L"不限时"), "Unlimited tooltip after clearing the timer");
    check(driver.model.running() && driver.model.request_confirmed(), "Clearing the timer keeps the session");
    driver.drain();
    check(driver.releases == 0, "Clearing the timer releases nothing");

    check(driver.model.set_duration(Duration::unlimited) == DurationChange::unchanged,
          "Selecting the current duration is unchanged");
    check(driver.model.set_duration(static_cast<Duration>(9)) == DurationChange::rejected,
          "An unsupported duration value is rejected");
    check(driver.model.duration() == Duration::unlimited,
          "A rejected duration keeps the current selection");
    check(!driver.model.remaining_time(), "A rejected duration leaves no deadline behind");
}

void duration_deadline_boundaries() {
    for (const auto length : {minutes{30}, minutes{60}, minutes{120}}) {
        const std::string label = std::to_string(length.count()) + "m";
        Driver driver = running_driver();
        const Duration tier = length == minutes{30}   ? Duration::minutes_30
                              : length == minutes{60} ? Duration::hours_1
                                                      : Duration::hours_2;
        check(driver.model.set_duration(tier) == DurationChange::updated,
              "Deadline fixture selects a finite duration: " + label);
        driver.drain();

        // Just before the deadline the session keeps running.
        driver.advance(std::chrono::duration_cast<seconds>(length) - seconds{1});
        check(driver.model.running(), "Session survives until the deadline: " + label);
        check(remaining_seconds(driver.model) == 1, "One second remains before the deadline: " + label);
        driver.model.observe(no_battery);
        driver.drain();
        check(driver.model.running(), "A sample before the deadline does not stop: " + label);
        check(driver.releases == 0, "Nothing is released before the deadline: " + label);

        // Exactly at the deadline the session stops.
        driver.advance(seconds{1});
        driver.model.observe(no_battery);
        check(!driver.model.running(), "Reaching the deadline stops the session: " + label);
        check(driver.model.stop_reason() == StopReason::expired, "Deadline stop reason is expiry: " + label);
        check(!driver.model.remaining_time(), "Stopped session has no remaining time: " + label);
        const auto actions = driver.model.take_actions();
        check(actions.size() == 1 && actions[0] == Action::release_request,
              "Expiry queues one full release: " + label);
        driver.model.on_release_result(true);
        driver.drain();
        check(driver.notifications.empty(), "Expiry is silent: " + label);
        check(has(driver.model.status_text(), L"已停止"), "Expiry shows a stopped state: " + label);
        check(has(driver.model.status_text(), L"到期"), "Expiry stop reason is displayed: " + label);

        // The app stays available and a manual start is a new session.
        check(driver.model.start(), "Restart after expiry is allowed: " + label);
        check(driver.model.duration() == Duration::unlimited,
              "Restart after expiry defaults to unlimited: " + label);
        check(!driver.model.remaining_time(), "Restart after expiry has no countdown: " + label);
        driver.drain();
    }

    // Past the deadline is the same outcome, and it must not revive anything.
    {
        Driver driver = running_driver();
        check(driver.model.set_duration(Duration::hours_2) == DurationChange::updated,
              "Overshoot fixture selects 2 hours");
        driver.drain();
        driver.advance(minutes{121});
        driver.model.observe(no_battery);
        check(!driver.model.running(), "A sample after the deadline stops the session");
        check(driver.model.stop_reason() == StopReason::expired, "Overshoot stop reason is expiry");
        driver.drain();
        check(driver.releases == 1 && driver.applies == 1, "Overshoot releases once and never re-requests");
        check(driver.notifications.empty(), "Overshoot stays silent");

        // A later ordinary stop request must not rewrite the expiry reason.
        driver.model.stop(StopReason::manual);
        check(driver.model.stop_reason() == StopReason::expired,
              "A later ordinary stop does not replace the expiry reason");
        driver.model.stop(StopReason::sleep);
        check(driver.model.stop_reason() == StopReason::sleep,
              "A concurrent sleep observation still wins over expiry");
    }

    // Duration expires for a session whose system request failed: one release.
    {
        Driver driver = running_driver();
        check(driver.model.set_duration(Duration::minutes_30) == DurationChange::updated,
              "Failed-request fixture selects 30 minutes");
        driver.drain();
        driver.advance(minutes{30});
        check(driver.model.toggle_display() == DisplayToggle::rejected_stopped,
              "An ordinary action after the deadline selects no display option");
        check(!driver.model.running(), "An ordinary action at the deadline stops the session");
        check(driver.model.stop_reason() == StopReason::expired,
              "Ordinary action cannot extend a reached deadline");
        check(!driver.model.display_selected(), "A route through the deadline selects nothing");
        driver.drain();
        check(driver.display_applies == 0, "No display request escapes after the deadline");
    }
}

void duration_retiming_and_reset() {
    Driver driver = running_driver();
    check(driver.model.set_duration(Duration::minutes_30) == DurationChange::updated, "Start a 30 minute session");
    driver.drain();
    driver.advance(minutes{10});
    check(remaining_seconds(driver.model) == 20 * 60, "The countdown decreases with time");
    check(has(driver.model.status_text(), L"剩余 20 分 0 秒"), "The tooltip shows the decreasing remainder");

    // Switching restarts the countdown from the current moment.
    check(driver.model.set_duration(Duration::hours_1) == DurationChange::updated,
          "Switching to 1 hour updates the session");
    check(remaining_seconds(driver.model) == 60 * 60, "Switching re-times from the current moment");
    driver.advance(minutes{20});
    check(remaining_seconds(driver.model) == 40 * 60, "The retimed countdown decreases");
    check(driver.model.running(), "Retiming keeps the session running");
    driver.drain();
    check(driver.applies == 1 && driver.releases == 0, "Retiming never re-requests or releases");
    check(driver.notifications.empty(), "Retiming stays silent");

    // Switching to unlimited removes the deadline entirely.
    check(driver.model.set_duration(Duration::unlimited) == DurationChange::updated, "Switch to unlimited");
    driver.advance(minutes{600});
    driver.model.observe(no_battery);
    check(driver.model.running(), "An unlimited session never expires");
    check(!driver.model.remaining_time(), "An unlimited session has no deadline");
    driver.drain();
    check(driver.releases == 0, "Unlimited releases nothing");

    // A stale (older) reading cannot rewind time or extend the deadline.
    Driver stale = running_driver();
    check(stale.model.set_duration(Duration::minutes_30) == DurationChange::updated, "Stale fixture is 30 minutes");
    stale.drain();
    stale.advance(minutes{5});
    const auto before = remaining_seconds(stale.model);
    check(before == 25 * 60, "The stale fixture counts down before the out-of-order reading");
    // An out-of-order moment delivered late must be ignored by the model.
    stale.set_clock(stale.clock_now - minutes{10});
    check(remaining_seconds(stale.model) == before, "A stale moment does not extend the deadline");
    check(stale.model.running(), "A stale moment does not stop the session either");
    stale.set_clock(stale.clock_now + minutes{10});
    check(remaining_seconds(stale.model) == 25 * 60, "Time resumes from the last real reading");

    // A manual stop and a new start reset the timer; the selection is kept
    // only as the disabled menu state and is never persisted.
    Driver reset = running_driver();
    check(reset.model.set_duration(Duration::hours_2) == DurationChange::updated, "Reset fixture is 2 hours");
    reset.drain();
    reset.model.stop(StopReason::manual);
    check(!reset.model.remaining_time(), "Manual stop clears the deadline");
    check(reset.model.duration() == Duration::hours_2, "Stop keeps the duration as disabled menu state");
    reset.drain();
    check(reset.releases == 1, "Manual stop releases once");
    check(reset.model.set_duration(Duration::minutes_30) == DurationChange::rejected_stopped,
          "The duration cannot be changed while stopped");
    check(reset.model.duration() == Duration::hours_2, "A rejected duration change keeps the selection");
    check(reset.model.start(), "Start after a manual stop");
    check(reset.model.duration() == Duration::unlimited, "Restarting defaults to unlimited");
    check(!reset.model.remaining_time(), "Restarting does not restore the old countdown");
    reset.drain();
    check(reset.applies == 2, "Restarting applies exactly one system request");
    check(reset.model.set_duration(static_cast<Duration>(42)) == DurationChange::rejected,
          "An illegal duration is rejected while running too");
    check(reset.model.running(), "An illegal duration leaves the session running");
}

void duration_expiry_cross_events() {
    // Lock/display cross: a lock just before the deadline still releases the
    // display, and the unlock after the deadline must not restore it.
    {
        Driver driver = running_driver();
        check(driver.model.toggle_display() == DisplayToggle::enabled, "Enable display before the deadline");
        driver.drain();
        check(driver.model.display_confirmed(), "Display confirmed before the deadline");
        check(driver.model.set_duration(Duration::minutes_30) == DurationChange::updated,
              "Deadline fixture for the lock cross");
        driver.drain();

        driver.advance(minutes{29});
        driver.model.observe_session(SessionState::locked);
        check(driver.model.running(), "Lock before the deadline keeps the system request");
        check(!driver.model.display_confirmed(), "Lock before the deadline releases the display");
        driver.drain();

        driver.advance(minutes{2});
        driver.model.observe_session(SessionState::unlocked);
        check(!driver.model.running(), "Unlock after the deadline does not revive the session");
        check(driver.model.stop_reason() == StopReason::expired, "Unlock after the deadline reports expiry");
        check(!driver.model.display_selected(), "Unlock after the deadline restores no display option");
        const auto actions = driver.model.take_actions();
        check(actions.size() == 1 && actions[0] == Action::release_request,
              "Unlock after the deadline releases both requests");
        driver.model.on_release_result(true);
        driver.drain();
        check(driver.display_applies == 1, "No display request is applied by the unlock");
        check(driver.notifications.empty(), "This cross path stays silent");
    }

    // Battery cross: a low battery and the deadline in the same cycle keep the
    // product priority (low battery wins and notifies once).
    {
        Driver driver(20);
        driver.model.observe(battery(AcState::unplugged, 50));
        check(driver.model.set_duration(Duration::minutes_30) == DurationChange::rejected_stopped,
              "The duration is not selectable while stopped");
        check(driver.model.start(), "Battery cross session starts");
        driver.drain();
        check(driver.model.set_duration(Duration::minutes_30) == DurationChange::updated,
              "Battery cross selects 30 minutes");
        driver.drain();
        driver.advance(minutes{30});
        driver.model.observe(battery(AcState::unplugged, 20));
        check(!driver.model.running(), "Low battery in the deadline cycle stops the session");
        check(driver.model.stop_reason() == StopReason::low_battery,
              "Low battery wins over expiry in the same cycle");
        driver.drain();
        check(driver.releases == 1, "The combined cycle releases once");
        check(driver.notifications.size() == 1, "The combined cycle notifies once for the battery");
    }

    // Sleep after the deadline also reports sleep (product priority).
    {
        Driver driver = running_driver();
        check(driver.model.set_duration(Duration::minutes_30) == DurationChange::updated,
              "Sleep cross selects 30 minutes");
        driver.drain();
        driver.advance(minutes{45});
        driver.model.stop(StopReason::sleep);
        check(!driver.model.running(), "Sleep after the deadline stays stopped");
        check(driver.model.stop_reason() == StopReason::sleep, "Sleep outranks expiry in the stop reason");
        driver.drain();
        check(driver.releases == 1, "Sleep after the deadline releases once");
    }

    // Threshold change after the deadline changes only the threshold.
    {
        Driver driver = running_driver();
        check(driver.model.set_duration(Duration::hours_1) == DurationChange::updated,
              "Threshold cross selects 1 hour");
        driver.drain();
        driver.advance(minutes{61});
        check(driver.model.set_threshold(30) == ThresholdChange::updated,
              "A threshold change after the deadline still applies");
        check(!driver.model.running(), "A threshold change after the deadline never restarts");
        check(driver.model.stop_reason() == StopReason::expired, "The expiry reason is kept");
        check(driver.model.threshold() == 30, "The new threshold is kept");
        driver.drain();
        check(driver.applies == 1, "No system request is re-applied by the threshold change");
    }

    // Late callbacks after expiry never revive or re-arm anything.
    {
        Driver driver = running_driver();
        check(driver.model.toggle_display() == DisplayToggle::enabled, "Late-callback fixture enables the display");
        driver.drain();
        check(driver.model.set_duration(Duration::minutes_30) == DurationChange::updated,
              "Late-callback fixture selects 30 minutes");
        driver.drain();
        driver.advance(minutes{31});
        driver.model.stop(StopReason::manual);
        check(!driver.model.running(), "Fixture is stopped before the late results");
        driver.drain();
        const int applies_before = driver.applies;
        driver.model.on_request_result(true);
        driver.model.on_display_result(true);
        check(!driver.model.running(), "Late successes after expiry do not revive the session");
        check(!driver.model.remaining_time(), "Late successes after expiry re-arm no countdown");
        const auto actions = driver.model.take_actions();
        check(!actions.empty() && std::ranges::all_of(actions, [](Action action) {
                  return action == Action::release_request;
              }),
              "Late successes after expiry ask for a full release only");
        driver.drain();
        check(driver.applies == applies_before, "Late successes after expiry apply nothing new");
        check(driver.notifications.empty(), "Late successes after expiry are silent");
    }
}

void duration_clock_and_overflow() {
    // A wall-clock jump cannot reach the model: the deadline follows monotonic
    // time only, so a session that never advanced monotonically keeps running.
    Driver driver = running_driver();
    check(driver.model.set_duration(Duration::hours_2) == DurationChange::updated, "Clock fixture selects 2 hours");
    driver.drain();
    for (int index = 0; index < 5; ++index) {
        driver.model.observe(no_battery);
        check(driver.model.running(), "Repeated samples at the same moment keep the session");
    }
    check(remaining_seconds(driver.model) == 120 * 60, "The deadline is unchanged without monotonic time");

    // A clock reading near the end of the range saturates instead of wrapping
    // into a deadline in the past.
    Driver edge;
    edge.model.observe(no_battery);
    edge.set_clock(Model::TimePoint::max() - minutes{30});
    check(edge.model.start(), "An edge-of-range clock still starts");
    edge.drain();
    check(edge.model.set_duration(Duration::hours_2) == DurationChange::updated,
          "A 2 hour deadline near the range end is accepted");
    check(edge.model.running(), "Saturating arithmetic keeps the session running");
    check(remaining_seconds(edge.model) == 30 * 60, "The saturated deadline counts down normally");
    edge.set_clock(Model::TimePoint::max());
    edge.model.observe(no_battery);
    check(!edge.model.running(), "Reaching the saturated deadline stops the session");
    check(edge.model.stop_reason() == StopReason::expired, "The saturated deadline reports expiry");

    // Very old clock readings and a fresh start after them stay consistent.
    Driver epoch;
    epoch.model.observe(no_battery);
    check(epoch.model.start(), "An epoch-zero clock starts normally");
    epoch.drain();
    check(epoch.model.set_duration(Duration::minutes_30) == DurationChange::updated,
          "An epoch-zero clock accepts a finite duration");
    check(remaining_seconds(epoch.model) == 30 * 60, "An epoch-zero clock counts down normally");
    epoch.advance(seconds{1});
    epoch.model.observe(no_battery);
    check(remaining_seconds(epoch.model) == 30 * 60 - 1, "The epoch-zero countdown decreases");
}

// ---------------------------------------------------------------------------
// Event interlacing (C4): sleep/resume, lock/expiry, battery/sleep, failure,
// late callbacks and explicit restart after every stop reason.
// ---------------------------------------------------------------------------

// One interleaving step: what the adapter reported and what stayed visible.
struct Interleave {
    Driver driver;
    explicit Interleave(int threshold = 20) : driver(threshold) {
        driver.model.observe(no_battery);
        driver.model.observe_session(SessionState::unlocked);
    }
    void run() {
        check(driver.model.start(), "Interleave fixture starts");
        driver.drain();
        check(driver.model.request_confirmed(), "Interleave fixture confirmed");
    }
    // Starts a confirmed session and selects the display option for it.
    void run_with_display() {
        run();
        check(driver.model.toggle_display() == DisplayToggle::enabled,
              "Interleave fixture requests the display");
        driver.drain();
        check(driver.model.display_confirmed(), "Interleave fixture display confirmed");
    }
};

void events_sleep_and_resume() {
    // Suspend stops the session and releases both requests.
    {
        Interleave fixture;
        fixture.run_with_display();
        fixture.driver.model.stop(StopReason::sleep);
        check(!fixture.driver.model.running(), "Suspend stops the running session");
        check(fixture.driver.model.stop_reason() == StopReason::sleep, "Suspend stop reason is sleep");
        check(!fixture.driver.model.display_selected(), "Suspend clears the this-session display option");
        check(!fixture.driver.model.remaining_time(), "Suspend clears any countdown");
        const auto actions = fixture.driver.model.take_actions();
        check(actions.size() == 1 && actions[0] == Action::release_request,
              "Suspend queues one full release");
        fixture.driver.drain();
        check(fixture.driver.notifications.empty(), "Suspend is not announced");
        check(has(fixture.driver.model.status_text(), L"系统睡眠"), "Suspend reason is displayed");
    }

    // Resume after a suspend: still stopped, release already confirmed, silent.
    {
        Interleave fixture;
        fixture.run();
        fixture.driver.model.stop(StopReason::sleep);
        fixture.driver.drain();
        const int applies = fixture.driver.applies;
        const int releases = fixture.driver.releases;
        fixture.driver.model.stop(StopReason::sleep);
        fixture.driver.drain();
        check(!fixture.driver.model.running(), "Automatic resume does not restart the session");
        check(fixture.driver.model.stop_reason() == StopReason::sleep, "Resume keeps the sleep reason");
        check(fixture.driver.applies == applies, "Resume re-applies no system request");
        check(fixture.driver.releases == releases, "Resume queues no second release");
        check(fixture.driver.notifications.empty(), "Resume is silent");
        check(!fixture.driver.model.release_unconfirmed(), "Resume invents no release state");
    }

    // Resume with no earlier suspend notification (the pre-sleep event was
    // missed) must still stop a running session.
    {
        Interleave fixture;
        fixture.run();
        fixture.driver.model.stop(StopReason::sleep);
        check(!fixture.driver.model.running(), "A resume-only event stops a running session");
        check(fixture.driver.model.stop_reason() == StopReason::sleep, "Resume-only stop reason is sleep");
        fixture.driver.drain();
        check(fixture.driver.releases == 1, "Resume-only stop releases both requests");
        check(fixture.driver.applies == 1, "Resume-only stop never re-applies the request");
    }

    // Repeated resume notifications while stopped change nothing at all.
    {
        Interleave fixture;
        fixture.run();
        fixture.driver.model.stop(StopReason::sleep);
        fixture.driver.drain();
        for (int index = 0; index < 3; ++index) fixture.driver.model.stop(StopReason::sleep);
        fixture.driver.drain();
        check(!fixture.driver.model.running(), "Repeated resumes never restart");
        check(fixture.driver.applies == 1 && fixture.driver.releases == 1,
              "Repeated resumes queue no extra API work");
        check(fixture.driver.notifications.empty(), "Repeated resumes stay silent");
    }

    // Explict start after a sleep stop is a fresh, silent, default session.
    {
        Interleave fixture;
        fixture.run();
        fixture.driver.model.stop(StopReason::sleep);
        fixture.driver.drain();
        check(fixture.driver.model.start(), "An explicit start after sleep is allowed");
        fixture.driver.drain();
        check(fixture.driver.model.running() && fixture.driver.model.request_confirmed(),
              "The explicit start after sleep is confirmed");
        check(fixture.driver.model.stop_reason() == StopReason::none, "A new session clears the stop reason");
        check(fixture.driver.model.duration() == Duration::unlimited,
              "The explicit start after sleep defaults to unlimited");
        check(!fixture.driver.model.display_selected(),
              "The explicit start after sleep defaults to display-off");
        check(fixture.driver.notifications.empty(), "The explicit start after sleep is silent");
        check(fixture.driver.applies == 2, "The explicit start after sleep applies one request");
    }

    // Sleep beats every other reason observed in the same cycle.
    {
        Interleave fixture;
        fixture.run();
        fixture.driver.model.observe(battery(AcState::unplugged, 20));
        fixture.driver.model.stop(StopReason::sleep);
        check(fixture.driver.model.stop_reason() == StopReason::sleep,
              "Sleep outranks low battery in the same cycle");
        fixture.driver.drain();
        check(fixture.driver.releases == 1, "The combined sleep cycle releases once");
    }

    // A pending display request plus a sleep stop releases everything; the
    // late display success must not hold anything awake.
    {
        Interleave fixture;
        fixture.run();
        check(fixture.driver.model.toggle_display() == DisplayToggle::enabled,
              "Sleep-during-pending fixture requests the display");
        fixture.driver.model.stop(StopReason::sleep);
        const auto actions = fixture.driver.model.take_actions();
        check(actions.size() == 1 && actions[0] == Action::release_request,
              "A sleep stop during a pending display apply releases everything");
        fixture.driver.model.on_display_result(true);
        const auto late = fixture.driver.model.take_actions();
        check(!late.empty() && std::ranges::all_of(late, [](Action action) {
                  return action == Action::release_request;
              }),
              "A late display success after sleep only releases");
        check(!fixture.driver.model.running() && !fixture.driver.model.display_confirmed(),
              "A late display success after sleep revives nothing");
    }
}

void events_lock_expiry_and_battery() {
    // Lock, then the deadline passes, then unlock: nothing is restored.
    {
        Interleave fixture;
        fixture.run_with_display();
        check(fixture.driver.model.set_duration(Duration::minutes_30) == DurationChange::updated,
              "Lock-expiry selects 30 minutes");
        fixture.driver.drain();
        fixture.driver.advance(minutes{10});
        fixture.driver.model.observe_session(SessionState::locked);
        check(fixture.driver.model.running(), "Lock keeps the system request running");
        check(!fixture.driver.model.display_confirmed(), "Lock releases the display request");
        fixture.driver.drain();
        fixture.driver.advance(minutes{25});
        fixture.driver.model.observe_session(SessionState::unlocked);
        check(!fixture.driver.model.running(), "Unlock after the deadline does not revive the session");
        check(fixture.driver.model.stop_reason() == StopReason::expired,
              "Unlock after the deadline reports expiry");
        check(!fixture.driver.model.display_selected(), "Unlock after the deadline restores no display option");
        fixture.driver.drain();
        check(fixture.driver.display_applies == 1, "Unlock after the deadline applies no display request");
        check(fixture.driver.notifications.empty(), "The lock-expiry route is silent");
    }

    // Battery observation in the same cycle as the deadline: battery wins.
    {
        Interleave fixture;
        fixture.run();
        check(fixture.driver.model.set_duration(Duration::minutes_30) == DurationChange::updated,
              "Battery-expiry selects 30 minutes");
        fixture.driver.drain();
        fixture.driver.advance(minutes{30});
        fixture.driver.model.observe(no_battery);
        check(!fixture.driver.model.running(), "The deadline alone stops the session");
        check(fixture.driver.model.stop_reason() == StopReason::expired, "The deadline alone reports expiry");
        fixture.driver.drain();
        check(fixture.driver.notifications.empty(), "Expiry itself is silent");
        // A low battery that only becomes visible once the sample arrives is
        // the conservative stop for that sample: it notifies once and never
        // restarts. The already-recorded expiry keeps its reason because the
        // session had already ended; no second release is requested.
        fixture.driver.model.observe(battery(AcState::unplugged, 20));
        check(!fixture.driver.model.running(), "A later low battery never restarts the session");
        check(fixture.driver.model.stop_reason() == StopReason::expired,
              "A completed stop keeps its recorded expiry reason");
        fixture.driver.drain();
        check(fixture.driver.notifications.empty(),
              "A low battery after the stop is already recorded is not re-announced");
        check(fixture.driver.releases == 1, "The later low battery asks for no second release");
    }

    // Low battery and the deadline in the true same cycle: one notification.
    {
        Interleave fixture;
        fixture.driver.model.observe(battery(AcState::unplugged, 50));
        check(fixture.driver.model.start(), "Battery-deadline fixture starts on battery");
        fixture.driver.drain();
        check(fixture.driver.model.set_duration(Duration::minutes_30) == DurationChange::updated,
              "Battery-deadline fixture selects 30 minutes");
        fixture.driver.drain();
        fixture.driver.advance(minutes{30});
        fixture.driver.model.observe(battery(AcState::unplugged, 20));
        check(!fixture.driver.model.running(), "Same-cycle low battery and deadline stop the session");
        check(fixture.driver.model.stop_reason() == StopReason::low_battery,
              "Same-cycle low battery wins over the deadline");
        fixture.driver.drain();
        check(fixture.driver.releases == 1 && fixture.driver.notifications.size() == 1,
              "Same-cycle stop releases once and notifies once");
        check(has(fixture.driver.model.status_text(), L"20%"), "Same-cycle stop shows the battery threshold");
    }

    // Low battery then sleep: sleep keeps priority for the reason, but the
    // single low-battery notice has already been delivered and is not repeated.
    {
        Interleave fixture;
        fixture.run();
        fixture.driver.model.observe(battery(AcState::unplugged, 20));
        check(fixture.driver.model.stop_reason() == StopReason::low_battery, "Low battery stops first");
        fixture.driver.model.stop(StopReason::sleep);
        check(fixture.driver.model.stop_reason() == StopReason::sleep, "Sleep then takes the reason priority");
        fixture.driver.drain();
        check(fixture.driver.notifications.size() == 1, "Only one battery notice is delivered");
        check(fixture.driver.releases == 1, "One release covers both stop causes");
    }

    // Battery and sleep in the same cycle through the model's stop path.
    {
        Interleave fixture;
        fixture.run();
        fixture.driver.model.stop(StopReason::sleep);
        fixture.driver.model.observe(battery(AcState::unplugged, 20));
        check(fixture.driver.model.stop_reason() == StopReason::sleep,
              "Battery observed after sleep does not replace the sleep reason");
        fixture.driver.drain();
        check(fixture.driver.releases == 1 && fixture.driver.notifications.empty(),
              "The sleep stop stays silent and releases once");
    }
}

void events_failure_and_late_results() {
    // A request failure stops; a following resume can only change the reason,
    // never restart, and the single fault notice is not repeated.
    {
        Interleave fixture;
        fixture.driver.apply_ok = false;
        check(fixture.driver.model.start(), "Failure fixture requests the system");
        fixture.driver.drain();
        check(!fixture.driver.model.running(), "A failed request stops the session");
        check(fixture.driver.model.stop_reason() == StopReason::request_failed,
              "A failed request records the failure reason");
        check(fixture.driver.notifications.size() == 1, "A failed request notifies once");
        fixture.driver.applies = 0;
        fixture.driver.model.stop(StopReason::sleep);
        check(fixture.driver.model.stop_reason() == StopReason::sleep,
              "Sleep outranks a recorded request failure");
        fixture.driver.drain();
        check(!fixture.driver.model.running() && fixture.driver.applies == 0,
              "A resume after a failure applies nothing");
        check(fixture.driver.notifications.size() == 1, "A resume after a failure does not re-notify");
    }

    // Manual stop then a late success and a late failure: silent, no revival.
    {
        Interleave fixture;
        fixture.run();
        fixture.driver.model.stop(StopReason::manual);
        fixture.driver.model.on_request_result(true);
        fixture.driver.model.on_display_result(true);
        fixture.driver.drain();
        check(!fixture.driver.model.running(), "Late successes after a manual stop revive nothing");
        check(!fixture.driver.model.request_confirmed() && !fixture.driver.model.display_confirmed(),
              "Late successes after a manual stop confirm nothing");
        check(fixture.driver.notifications.empty(), "Late successes after a manual stop are silent");
        const int applies = fixture.driver.applies;
        fixture.driver.model.on_display_release_result(false, 7);
        fixture.driver.drain();
        check(fixture.driver.notifications.empty(), "A late release failure does not notify");
        check(fixture.driver.applies == applies, "A late release failure applies nothing");
        check(fixture.driver.model.stop_reason() == StopReason::manual,
              "Late results keep the manual stop reason");
    }

    // An expired session with a late success stays expired and stopped.
    {
        Interleave fixture;
        fixture.run();
        check(fixture.driver.model.set_duration(Duration::minutes_30) == DurationChange::updated,
              "Late-result fixture selects 30 minutes");
        fixture.driver.drain();
        fixture.driver.advance(minutes{31});
        fixture.driver.model.observe(no_battery);
        check(!fixture.driver.model.running(), "The fixture expires");
        fixture.driver.model.on_request_result(true);
        fixture.driver.drain();
        check(!fixture.driver.model.running(), "A late success after expiry revives nothing");
        check(!fixture.driver.model.remaining_time(), "A late success after expiry re-arms no countdown");
        check(fixture.driver.notifications.empty(), "A late success after expiry is silent");
    }
}

void toggle_at_deadline_keeps_the_intent() {
    // The decisive case: the running session reaches its deadline in the same
    // cycle as the toggle. The user intent is stop, so it must stop, not turn
    // into a fresh unlimited start. The ordinary toggle is the least urgent
    // event, so a reached deadline keeps its own reason.
    {
        Interleave fixture;
        fixture.run();
        check(fixture.driver.model.set_duration(Duration::minutes_30) == DurationChange::updated,
              "Toggle fixture selects 30 minutes");
        fixture.driver.drain();
        fixture.driver.advance(minutes{30});
        check(fixture.driver.model.running(), "The session is still running before the toggle is handled");
        const int applies = fixture.driver.applies;
        const bool toggled = fixture.driver.model.toggle();
        check(!toggled, "A toggle on a running session reports stopped");
        check(!fixture.driver.model.running(), "A toggle at the deadline stops and does not restart");
        check(fixture.driver.model.stop_reason() == StopReason::expired,
              "A toggle exactly at the deadline reports the expiry reason");
        check(!fixture.driver.model.remaining_time(), "A toggle at the deadline leaves no countdown");
        check(fixture.driver.model.duration() == Duration::minutes_30,
              "A toggle at the deadline keeps the disabled menu selection");
        fixture.driver.drain();
        check(fixture.driver.applies == applies, "A toggle at the deadline applies no new system request");
        check(fixture.driver.releases == 1, "A toggle at the deadline releases the request once");
        check(fixture.driver.notifications.empty(), "A toggle at the deadline is silent");
    }

    // The same cycle with an expired-by-ordinary-action session: the model's
    // refresh must not turn the stop intent into a start either.
    {
        Interleave fixture;
        fixture.run();
        check(fixture.driver.model.set_duration(Duration::hours_1) == DurationChange::updated,
              "Second toggle fixture selects 1 hour");
        fixture.driver.drain();
        fixture.driver.advance(minutes{61});
        check(!fixture.driver.model.toggle(), "A toggle past the deadline reports stopped");
        check(!fixture.driver.model.running(), "A toggle past the deadline stays stopped");
        check(fixture.driver.model.stop_reason() == StopReason::expired,
              "A toggle past the deadline reports the expiry reason");
        fixture.driver.drain();
        check(fixture.driver.releases == 1 && fixture.driver.applies == 1,
              "A toggle past the deadline releases once and applies nothing");
    }

    // A stopped session toggled on is a genuine, explicit start.
    {
        Interleave fixture;
        check(fixture.driver.model.toggle(), "A toggle on a stopped session starts it");
        check(fixture.driver.model.running(), "A stopped session toggles into running");
        fixture.driver.drain();
        check(fixture.driver.model.request_confirmed(), "The toggled start is confirmed");
        check(fixture.driver.model.duration() == Duration::unlimited,
              "The toggled start uses the unlimited default");
        check(!fixture.driver.model.display_selected(), "The toggled start uses the display-off default");
        check(fixture.driver.notifications.empty(), "The toggled start is silent");
        check(!fixture.driver.model.toggle(), "Toggling the fresh session stops it again");
        check(fixture.driver.model.stop_reason() == StopReason::manual, "The second toggle is a manual stop");
    }

    // A stopped session is started only when the battery gate allows it; a
    // refused start never applies a request and reports the battery reason.
    {
        Interleave fixture;
        fixture.driver.model.observe(battery(AcState::unplugged, 20));
        check(!fixture.driver.model.toggle(), "A toggle start on a low battery is refused");
        check(!fixture.driver.model.running(), "A refused toggle start stays stopped");
        check(fixture.driver.model.stop_reason() == StopReason::low_battery,
              "A refused toggle start reports the battery reason");
        fixture.driver.drain();
        check(fixture.driver.notifications.size() == 1, "A refused toggle start notifies low battery once");
        check(fixture.driver.applies == 0, "A refused toggle start applies no request");
        check(fixture.driver.releases == 0, "A refused toggle start releases nothing");
        fixture.driver.model.observe(battery(AcState::plugged, 20));
        check(fixture.driver.model.toggle(), "A toggle start on AC is allowed");
        fixture.driver.drain();
        check(fixture.driver.applies == 1 && fixture.driver.model.request_confirmed(),
              "The allowed toggle start applies exactly one request");
        check(fixture.driver.notifications.size() == 1, "The allowed toggle start adds no notice");
    }
}

void stop_at_deadline_keeps_the_expiry_reason() {
    // Priority boundary: a deadline already reached outranks an ordinary stop.
    // The direct stop (the Win32 adapter takes that path when the tray menu is
    // used while running) and the toggle must agree on this, and neither may
    // start anything. Both are checked before, exactly at and past the
    // deadline.
    enum class Moment { before, exactly, past };
    struct Path {
        bool via_toggle;
        const char* label;
    };
    const Path paths[] = {{false, "direct stop"}, {true, "toggle"}};
    const std::pair<Moment, const char*> moments[] = {
        {Moment::before, "before"}, {Moment::exactly, "at"}, {Moment::past, "past"}};

    for (const auto& path : paths) {
        for (const auto& moment : moments) {
            const std::string label = std::string(path.label) + " " + moment.second + " the deadline";
            const std::string tagged = std::string(path.label) + " " + moment.second;
            Interleave fixture;
            fixture.run_with_display();
            check(fixture.driver.model.set_duration(Duration::minutes_30) == DurationChange::updated,
                  "Deadline fixture selects 30 minutes: " + tagged);
            fixture.driver.drain();
            switch (moment.first) {
            case Moment::before: fixture.driver.advance(minutes{29}); break;
            case Moment::exactly: fixture.driver.advance(minutes{30}); break;
            case Moment::past: fixture.driver.advance(minutes{31}); break;
            }
            check(fixture.driver.model.running(), "The session is still running: " + label);
            const int applies = fixture.driver.applies;
            if (path.via_toggle) {
                check(!fixture.driver.model.toggle(), "A toggle reports stopped: " + label);
            } else {
                fixture.driver.model.stop(StopReason::manual);
            }
            check(!fixture.driver.model.running(), "The session is stopped: " + label);
            // The intent is always stop; only the recorded reason depends on
            // whether the deadline had already been reached.
            const StopReason expected =
                moment.first == Moment::before ? StopReason::manual : StopReason::expired;
            check(fixture.driver.model.stop_reason() == expected, "Stop reason matches time: " + label);
            check(fixture.driver.model.status_text().find(
                      moment.first == Moment::before ? L"手动" : L"到期") != std::wstring::npos,
                  "The tooltip explains the stop: " + label);
            check(!fixture.driver.model.remaining_time(), "No countdown is left: " + label);
            check(!fixture.driver.model.display_selected(), "The display option is cleared: " + label);
            check(fixture.driver.model.duration() == Duration::minutes_30,
                  "The disabled duration selection is kept: " + label);
            fixture.driver.drain();
            check(fixture.driver.applies == applies, "No system request is applied: " + label);
            check(fixture.driver.releases == 1, "The request is released exactly once: " + label);
            check(fixture.driver.notifications.empty(), "The stop is silent: " + label);
            // Time and samples after the stop never restart it.
            fixture.driver.advance(minutes{200});
            fixture.driver.model.observe(no_battery);
            fixture.driver.drain();
            check(!fixture.driver.model.running(), "Nothing restarts the stopped session: " + label);
            check(fixture.driver.model.stop_reason() == expected, "The reason is not rewritten: " + label);
            check(fixture.driver.applies == applies, "No request appears after the stop: " + label);
            // Only an explicit start brings it back, as a fresh default session.
            check(fixture.driver.model.start(), "An explicit start is allowed: " + label);
            fixture.driver.drain();
            check(fixture.driver.model.running() && fixture.driver.model.request_confirmed(),
                  "The explicit start is confirmed: " + label);
            check(fixture.driver.model.stop_reason() == StopReason::none &&
                      fixture.driver.model.duration() == Duration::unlimited,
                  "The explicit start is a new default session: " + label);
            check(fixture.driver.applies == applies + 1, "The explicit start applies once: " + label);
        }
    }

    // Urgent reasons keep their own reason even at a reached deadline: sleep and
    // battery protection are never demoted to an ordinary expiry.
    struct Urgent {
        StopReason reason;
        const char* label;
    };
    const Urgent urgent[] = {{StopReason::sleep, "sleep"},
                             {StopReason::low_battery, "low_battery"},
                             {StopReason::battery_unknown, "battery_unknown"},
                             {StopReason::request_failed, "request_failed"}};
    for (const auto& test_case : urgent) {
        const std::string label = std::string("an urgent stop at the deadline stays ") + test_case.label;
        Interleave fixture;
        fixture.run();
        check(fixture.driver.model.set_duration(Duration::minutes_30) == DurationChange::updated,
              "Urgent deadline fixture selects 30 minutes: " + label);
        fixture.driver.drain();
        fixture.driver.advance(minutes{31});
        const int applies = fixture.driver.applies;
        fixture.driver.model.stop(test_case.reason);
        check(!fixture.driver.model.running(), "The urgent stop stops the session: " + label);
        check(fixture.driver.model.stop_reason() == test_case.reason, "The urgent reason is kept: " + label);
        fixture.driver.drain();
        check(fixture.driver.applies == applies, "The urgent stop applies no request: " + label);
        check(fixture.driver.releases == 1, "The urgent stop releases once: " + label);
    }

    // A stop on an unlimited session reads no deadline at all; it is manual.
    {
        Interleave fixture;
        fixture.run();
        fixture.driver.advance(minutes{500});
        fixture.driver.model.stop(StopReason::manual);
        check(!fixture.driver.model.running(), "An unlimited session stops on demand");
        check(fixture.driver.model.stop_reason() == StopReason::manual,
              "An unlimited session has no expiry to report");
        fixture.driver.drain();
        check(fixture.driver.releases == 1, "The unlimited stop releases once");
    }
}

void unlimited_stop_does_not_read_the_clock() {
    // The documented contract for stop(): the monotonic clock is read only
    // when the running session has a deadline. A clock that never advances
    // must not change the outcome of an ordinary stop on an unlimited session,
    // and a deadline that is somehow pending must still win.
    Interleave fixture;
    fixture.run();
    check(fixture.driver.model.running(), "The unlimited fixture is running");
    check(!fixture.driver.model.remaining_time(), "The unlimited session has no countdown");
    const int applies = fixture.driver.applies;
    fixture.driver.model.stop(StopReason::manual);
    check(!fixture.driver.model.running(), "An unlimited session stops");
    check(fixture.driver.model.stop_reason() == StopReason::manual,
          "An unlimited ordinary stop reports manual without reading the clock");
    fixture.driver.drain();
    check(fixture.driver.applies == applies, "An unlimited stop applies nothing");
    check(fixture.driver.releases == 1, "An unlimited stop releases once");
    check(fixture.driver.notifications.empty(), "An unlimited stop is silent");

    // Finite sessions keep the expiry boundary; the guard only skips the
    // read when there is no deadline to compare.
    Interleave finite;
    finite.run();
    check(finite.driver.model.set_duration(Duration::hours_1) == DurationChange::updated,
          "The finite fixture arms one hour");
    finite.driver.advance(minutes{61});
    finite.driver.model.stop(StopReason::manual);
    check(finite.driver.model.stop_reason() == StopReason::expired,
          "A finite session past the deadline still reports expired");
}

void restart_after_every_stop_reason() {
    // Every stop reason leaves the app stopped; only an explicit start begins
    // a new default session, and it never inherits the previous session.
    struct Case {
        StopReason reason;
        Duration duration;
        bool expect_notice;
        const char* label;
    };
    const Case cases[] = {
        {StopReason::manual, Duration::minutes_30, false, "manual"},
        {StopReason::expired, Duration::hours_1, false, "expired"},
        {StopReason::low_battery, Duration::hours_2, true, "low_battery"},
        {StopReason::battery_unknown, Duration::minutes_30, true, "battery_unknown"},
        {StopReason::sleep, Duration::hours_2, false, "sleep"},
    };
    for (const auto& test_case : cases) {
        const std::string label = test_case.label;
        Interleave fixture;
        fixture.run_with_display();
        check(fixture.driver.model.set_duration(test_case.duration) == DurationChange::updated,
              "Finite duration before the stop: " + label);
        fixture.driver.drain();
        fixture.driver.model.stop(test_case.reason);
        check(!fixture.driver.model.running(), "The session is stopped: " + label);
        check(fixture.driver.model.stop_reason() == test_case.reason,
              "The stop reason is recorded: " + label);
        check(!fixture.driver.model.display_selected(), "The stop clears the display option: " + label);
        check(!fixture.driver.model.remaining_time(), "The stop clears the countdown: " + label);
        fixture.driver.drain();
        check(fixture.driver.notifications.size() == (test_case.expect_notice ? 1U : 0U),
              "Only the announced reasons notify: " + label);
        // Time passing and observations while stopped never restart.
        fixture.driver.advance(minutes{200});
        fixture.driver.model.observe(no_battery);
        fixture.driver.drain();
        check(!fixture.driver.model.running(), "Time and samples never restart: " + label);
        check(fixture.driver.applies == 1, "No automatic request after the stop: " + label);
        // An explicit start is a new default session and is silent about the
        // previous stop; it never replays the old duration or display option.
        check(fixture.driver.model.start(), "An explicit start is allowed: " + label);
        fixture.driver.drain();
        check(fixture.driver.model.running() && fixture.driver.model.request_confirmed(),
              "The explicit start is confirmed: " + label);
        check(fixture.driver.model.duration() == Duration::unlimited,
              "The explicit start defaults to unlimited: " + label);
        check(!fixture.driver.model.display_selected(),
              "The explicit start defaults to display-off: " + label);
        check(!fixture.driver.model.remaining_time(), "The explicit start has no countdown: " + label);
        check(fixture.driver.model.stop_reason() == StopReason::none,
              "The explicit start clears the stop reason: " + label);
        check(fixture.driver.applies == 2, "The explicit start applies exactly once: " + label);
    }

    // A real request failure goes through the adapter result, not a direct
    // stop call: it stops, revokes the option, notifies once and never restarts.
    {
        Interleave fixture;
        fixture.driver.apply_ok = false;
        check(fixture.driver.model.start(), "Failure start is requested");
        fixture.driver.drain();
        check(!fixture.driver.model.running(), "The failed request stops the session");
        check(fixture.driver.model.stop_reason() == StopReason::request_failed,
              "The failed request records the failure reason");
        check(!fixture.driver.model.display_selected(), "The failed request clears the display option");
        check(!fixture.driver.model.remaining_time(), "The failed request leaves no countdown");
        check(fixture.driver.releases == 1, "The failed request is reverted once");
        check(fixture.driver.notifications.size() == 1, "The failed request notifies once");
        fixture.driver.apply_ok = true;
        fixture.driver.advance(minutes{200});
        fixture.driver.model.observe(no_battery);
        fixture.driver.drain();
        check(!fixture.driver.model.running(), "Time and samples never restart after a failure");
        check(fixture.driver.applies == 1, "No automatic request after a failure");
        check(fixture.driver.model.start(), "An explicit start after a failure is allowed");
        fixture.driver.drain();
        check(fixture.driver.model.request_confirmed(), "The explicit start after a failure is confirmed");
        check(fixture.driver.notifications.size() == 1, "The explicit start after a failure adds no notice");
    }

    // A stopped session is not restarted by observations, but a more urgent
    // cause still upgrades the recorded reason so the tooltip stays accurate.
    Interleave stopped;
    stopped.driver.model.stop(StopReason::manual);
    stopped.driver.drain();
    stopped.driver.model.observe(unknown_battery);
    check(stopped.driver.model.stop_reason() == StopReason::manual,
          "A stopped session ignores battery samples until it is running again");
    stopped.driver.model.stop(StopReason::sleep);
    check(stopped.driver.model.stop_reason() == StopReason::sleep,
          "A later sleep takes priority while stopped");
    stopped.driver.model.stop(StopReason::manual);
    check(stopped.driver.model.stop_reason() == StopReason::sleep,
          "A later manual stop cannot soften the sleep reason");
    stopped.driver.model.observe(battery(AcState::unplugged, 5));
    check(stopped.driver.model.stop_reason() == StopReason::sleep,
          "A low battery sample while stopped adds nothing for a completed stop");
    stopped.driver.drain();
    check(!stopped.driver.model.running(), "Stopped reason changes never restart");
    check(stopped.driver.applies == 0, "Stopped reason changes apply nothing");
    check(stopped.driver.notifications.empty(), "Stopped observations stay silent");
}

std::string read_all(const std::filesystem::path& file) {
    std::ifstream input(file, std::ios::binary);
    std::string text;
    char character{};
    while (input.get(character)) text.push_back(character);
    return text;
}

void write_all(const std::filesystem::path& file, std::string_view text) {
    std::ofstream output(file, std::ios::binary | std::ios::trunc);
    output.write(text.data(), static_cast<std::streamsize>(text.size()));
    output.close();
    if (!output) throw std::runtime_error("test fixture write failed");
}

void settings_parsing() {
    check(parse_threshold("threshold=10\n") == 10, "Parse 10");
    check(parse_threshold("threshold=20") == 20, "Parse 20 without a newline");
    check(parse_threshold("threshold=30\r\n") == 30, "Parse 30 with CRLF");
    check(!parse_threshold("threshold=25\n"), "Reject 25");
    check(!parse_threshold("threshold=0\n"), "Reject 0");
    check(!parse_threshold("threshold=100\n"), "Reject 100");
    check(!parse_threshold("threshold=\n"), "Reject empty value");
    check(!parse_threshold("threshold=10\nextra\n"), "Reject extra lines");
    check(!parse_threshold("threshold=030\n"), "Reject padded value");
    check(!parse_threshold("Threshold=30\n"), "Reject wrong case");
    check(!parse_threshold("30"), "Reject bare value");
    check(!parse_threshold(""), "Reject empty text");
    check(!parse_threshold("{\"threshold\":30}"), "Reject unrelated formats");
    check(format_threshold(20) == "threshold=20\n", "Format is the minimal line");
    check(is_valid_threshold(10) && is_valid_threshold(20) && is_valid_threshold(30),
          "Supported thresholds are 10/20/30");
    check(!is_valid_threshold(15) && !is_valid_threshold(-1) && !is_valid_threshold(31),
          "Other thresholds are unsupported");
}

void settings_file_roundtrip() {
    const std::filesystem::path dir{UNSLEEP_TEST_TMP_DIR};
    std::error_code error;
    std::filesystem::remove_all(dir, error);
    std::filesystem::create_directories(dir, error);
    check(!error, "Project-local settings test area can be created");
    const auto file = dir / settings_file_name;

    check(!std::filesystem::exists(file), "Settings test area starts clean");
    auto loaded = load_threshold(file);
    check(loaded.status == ThresholdLoadStatus::missing, "Missing file reported as missing");
    check(loaded.threshold == 20, "Missing file falls back to 20");

    for (const int threshold : battery_thresholds) {
        check(save_threshold(file, threshold) == ThresholdSaveStatus::saved,
              "Save supported threshold " + std::to_string(threshold));
        check(read_all(file) == format_threshold(threshold), "Saved file uses the minimal format");
        loaded = load_threshold(file);
        check(loaded.status == ThresholdLoadStatus::loaded, "Saved file loads");
        check(loaded.threshold == threshold, "Round-trip threshold matches");
    }
    check(save_threshold(file, 25) == ThresholdSaveStatus::invalid_value, "Save rejects 25");
    check(read_all(file) == format_threshold(30), "Rejected save leaves the file untouched");

    for (const std::string_view text : {std::string_view{"threshold=25\n"}, std::string_view{"garbage\n"},
                                        std::string_view{""}, std::string_view{"threshold=20\nextra\n"},
                                        std::string_view{"{\"threshold\":30}"}}) {
        write_all(file, text);
        loaded = load_threshold(file);
        check(loaded.status == ThresholdLoadStatus::invalid, "Invalid file reported as invalid");
        check(loaded.threshold == 20, "Invalid file falls back to 20");
    }

    // A directory in place of the file is a read failure, not a missing file.
    const auto directory_path = dir / L"settings-directory";
    std::filesystem::create_directories(directory_path, error);
    loaded = load_threshold(directory_path);
    check(loaded.status == ThresholdLoadStatus::read_failed, "Unreadable path reported as read failure");
    check(loaded.threshold == 20, "Unreadable path falls back to 20");

    // Write failures are reported, never swallowed.
    check(save_threshold(directory_path, 10) == ThresholdSaveStatus::write_failed,
          "Writing to a directory path fails");
    write_all(file, format_threshold(20));
    // Removing every write bit maps to FILE_ATTRIBUTE_READONLY on Windows.
    constexpr auto all_write_perms = std::filesystem::perms::owner_write |
                                     std::filesystem::perms::group_write |
                                     std::filesystem::perms::others_write;
    std::filesystem::permissions(file, all_write_perms, std::filesystem::perm_options::remove, error);
    if (!error) {
        check(save_threshold(file, 30) == ThresholdSaveStatus::write_failed,
              "Writing to a read-only file fails");
        check(read_all(file) == format_threshold(20), "Failed write leaves the previous value");
        std::filesystem::permissions(file, all_write_perms, std::filesystem::perm_options::add, error);
    }

    check(settings_path_for(L"D:/Projects/unsleep/build-native/unsleep.exe") ==
              std::filesystem::path(L"D:/Projects/unsleep/build-native/unsleep.settings"),
          "Settings path sits next to the executable");
    check(settings_path_for(std::filesystem::path(L"unsleep.exe")) ==
              std::filesystem::path(L"unsleep.settings"), "Relative executable path stays relative");

    std::filesystem::remove_all(dir, error);
}

} // namespace

int main() {
    try {
        defaults_and_lifecycle();
        battery_gate_at_start();
        battery_gate_while_running();
        unknown_during_run();
        unknown_presence_on_ac_while_running();
        manual_start_after_stopped_data_change();
        request_feedback();
        late_callbacks_and_idempotency();
        threshold_matrix();
        threshold_change_behavior();
        invalid_threshold_falls_back();
        display_defaults_and_toggle();
        display_lock_and_unlock();
        display_stop_paths();
        display_failures();
        stale_action_targets();
        display_unknown_session_state();
        duration_tiers();
        duration_deadline_boundaries();
        duration_retiming_and_reset();
        duration_expiry_cross_events();
        duration_clock_and_overflow();
        events_sleep_and_resume();
        events_lock_expiry_and_battery();
        events_failure_and_late_results();
        toggle_at_deadline_keeps_the_intent();
        stop_at_deadline_keeps_the_expiry_reason();
        unlimited_stop_does_not_read_the_clock();
        restart_after_every_stop_reason();
        settings_parsing();
        settings_file_roundtrip();
        std::cout << "PASS: " << checks << " checks\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
