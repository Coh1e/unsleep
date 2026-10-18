#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace unsleep {
struct Period {
    int begin{};
    int end{};
    [[nodiscard]] bool contains(int minute) const noexcept { return begin <= minute && minute < end; }
};
enum class Icons { automatic, old, light, dark };
enum class Remote { none, exit, on, off, toggle_show, toggle, active_thirty };
struct Options {
    int interval_seconds{59};
    bool start_off{}, use_shift{}, allow_screensaver{}, execution_state{}, full_keypress{};
    bool on_ac{}, notify{}, no_icon{}, not_when_locked{true}, show_dialog{}, on_taskbar{}, replace{};
    Icons icons{Icons::automatic};
    Remote remote{Remote::none};
    int active_for{-1}, inactive_for{-1}, exit_after{-1}, cpu{-1}, key{-1};
    std::vector<Period> periods;
    std::wstring watch_window;
    std::wstring raw;
    std::wstring description;
};
[[nodiscard]] Options parse_options(std::wstring_view raw);
[[nodiscard]] std::wstring period_text(const Period& period);

struct Inputs {
    int minute_of_day{};
    bool shift_down{}, ac_power{true}, matching_window{true};
    std::optional<int> cpu_percent;
};
enum class KeepAwake { none, key, system, display };
struct Effects {
    KeepAwake keep_awake{KeepAwake::none};
    std::uint16_t key{};
    bool key_up_only{}, exit{}, skipped_tail{};
    std::vector<std::wstring> notifications;
};
class Model {
public:
    explicit Model(Options options);
    [[nodiscard]] Effects tick(const Inputs& inputs);
    void toggle();
    void set_active(bool value);
    void timed_active(int minutes);
    void timed_inactive(int minutes);
    void revert(int minute_of_day);
    void refresh_schedule(int minute_of_day);
    void set_locked(bool value) noexcept { locked_ = value; }
    [[nodiscard]] bool active() const noexcept { return active_; }
    [[nodiscard]] bool manual_override() const noexcept { return manual_; }
    [[nodiscard]] bool can_revert() const noexcept { return revert_available_; }
    [[nodiscard]] int active_remaining() const noexcept { return active_remaining_; }
    [[nodiscard]] int inactive_remaining() const noexcept { return inactive_remaining_; }
    [[nodiscard]] int exit_remaining() const noexcept { return exit_remaining_; }
    [[nodiscard]] int cpu_threshold() const noexcept { return cpu_threshold_; }
    [[nodiscard]] int seconds_to_key() const noexcept { return seconds_to_key_; }
    [[nodiscard]] const Options& options() const noexcept { return options_; }
    [[nodiscard]] std::wstring status(int minute_of_day) const;
    [[nodiscard]] std::vector<std::wstring> take_notifications();
private:
    void notify(std::wstring text);
    [[nodiscard]] const Period* current_period(int minute_of_day) const noexcept;
    Options options_;
    bool active_{true}, manual_{}, revert_available_{true}, locked_{};
    int active_remaining_{-1}, inactive_remaining_{-1}, exit_remaining_{-1};
    int cpu_threshold_{-1}, seconds_to_key_{59}, minute_ticks_{}, cpu_samples_{};
    std::wstring watch_window_;
    std::vector<std::wstring> notifications_;
};
}
