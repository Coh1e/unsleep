#include "unsleep/core.hpp"
#include <algorithm>
#include <cwctype>
#include <format>
#include <limits>
#include <stdexcept>
#include <utility>

namespace unsleep {
namespace {
int integer_prefix(std::wstring_view text) {
    while (!text.empty() && std::iswspace(text.front())) text.remove_prefix(1);
    bool negative = false;
    if (!text.empty() && (text.front() == L'-' || text.front() == L'+')) {
        negative = text.front() == L'-'; text.remove_prefix(1);
    }
    std::uint64_t number = 0;
    bool any = false;
    for (wchar_t ch : text) {
        if (ch < L'0' || ch > L'9') break;
        any = true;
        const auto digit = static_cast<unsigned>(ch - L'0');
        constexpr auto maximum = static_cast<std::uint64_t>(std::numeric_limits<int>::max());
        if (number > (maximum - digit) / 10) throw std::invalid_argument("Numeric option overflows a signed 32-bit integer");
        number = number * 10 + digit;
    }
    if (!any) return 0; // _wtoi-compatible defined nonnumeric result.
    if (negative && number > 1) throw std::invalid_argument("Negative option values below -1 are unsupported");
    return negative ? -static_cast<int>(number) : static_cast<int>(number);
}
std::optional<std::wstring_view> suffix(std::wstring_view raw, std::wstring_view flag) {
    const auto at = raw.find(flag);
    if (at == raw.npos) return std::nullopt;
    return raw.substr(at + flag.size());
}
}

Options parse_options(std::wstring_view input) {
    Options o;
    while (!input.empty() && std::iswspace(input.front())) input.remove_prefix(1);
    o.raw = input;
    std::ranges::transform(o.raw, o.raw.begin(), [](wchar_t ch) { return static_cast<wchar_t>(std::towlower(ch)); });
    const std::wstring_view raw = o.raw;
    const auto has = [&](std::wstring_view flag) { return raw.find(flag) != raw.npos; };
    const auto numeric = [&](std::wstring_view flag, int fallback) { const auto value = suffix(raw, flag); return value ? integer_prefix(*value) : fallback; };
    if (!raw.empty() && raw.front() >= L'1' && raw.front() <= L'9') o.interval_seconds = integer_prefix(raw);
    o.start_off = has(L"-startoff"); o.use_shift = has(L"-useshift");
    o.allow_screensaver = has(L"-allowss"); o.execution_state = has(L"-stes");
    o.full_keypress = has(L"-keypress"); o.on_ac = has(L"-onac"); o.notify = has(L"-notify");
    o.no_icon = has(L"-noicon"); o.show_dialog = has(L"-showdlg"); o.on_taskbar = has(L"-ontaskbar");
    o.replace = has(L"-replace");
    if (has(L"-oldicons")) o.icons = Icons::old;
    if (has(L"-lighticons")) o.icons = Icons::light;
    if (has(L"-darkicons")) o.icons = Icons::dark;
    if (has(L"-appexit")) o.remote = Remote::exit;
    else if (has(L"-appon")) o.remote = Remote::on;
    else if (has(L"-appoff")) o.remote = Remote::off;
    else if (has(L"-apptoggleshowdlg")) o.remote = Remote::toggle_show;
    else if (has(L"-apptoggle")) o.remote = Remote::toggle;
    else if (has(L"-replaceinactiveafter30")) o.remote = Remote::active_thirty;
    o.active_for = numeric(L"-activefor:", -1); o.inactive_for = numeric(L"-inactivefor:", -1);
    o.exit_after = numeric(L"-exitafter:", -1); o.cpu = numeric(L"-cpu:", -1); o.key = numeric(L"-key:", -1);
    if (auto hours = suffix(raw, L"-activehours:"); hours && hours->size() >= 24) {
        int start = -1;
        for (int hour = 0; hour < 24; ++hour) {
            if ((*hours)[static_cast<std::size_t>(hour)] == L'x') { if (start < 0) start = hour * 60; }
            else if (start >= 0) { o.periods.push_back({start, hour * 60 - 1}); start = -1; }
        }
        if (start >= 0) o.periods.push_back({start, 1439});
    }
    if (auto periods = suffix(raw, L"-activeperiods:")) {
        o.periods.clear();
        auto text = periods->substr(0, periods->find_first_of(L" \t.:"));
        while (text.size() >= 9) {
            auto part = text.substr(0, 9);
            if (part[4] == L'-') {
                const int h1 = integer_prefix(part.substr(0,2)), m1 = integer_prefix(part.substr(2,2));
                const int h2 = integer_prefix(part.substr(5,2)), m2 = integer_prefix(part.substr(7,2));
                if (h1 >= 0 && h1 < 24 && h2 >= 0 && h2 < 24 && m1 >= 0 && m1 < 60 && m2 >= 0 && m2 < 60 && h1*60+m1 < h2*60+m2)
                    o.periods.push_back({h1*60+m1,h2*60+m2});
            }
            if (text.size() < 10) break;
            text.remove_prefix(10);
        }
    }
    if (auto watch = suffix(raw, L"-watchwindow:")) o.watch_window = *watch;
    auto append = [&](std::wstring text) { o.description += std::move(text) + L", "; };
    if (!raw.empty() && raw.front() >= L'1' && raw.front() <= L'9') append(std::format(L"Every {}s",o.interval_seconds));
    if(o.no_icon) append(L"no icon");
    if(has(L"-notwhenlocked")) append(L"not when locked");
    if(has(L"-oldicons")) append(L"use old icons"); if(has(L"-lighticons")) append(L"use light icons"); if(has(L"-darkicons")) append(L"use dark icons");
    if(o.start_off) append(L"start off"); if(o.use_shift) append(L"use shift"); if(o.allow_screensaver) append(L"allow screensaver");
    if(o.execution_state) append(L"STES"); if(o.full_keypress) append(L"full keypress"); if(o.on_ac) append(L"only on AC power"); if(o.notify) append(L"notify on active change");
    if(suffix(raw,L"-exitafter:")) append(std::format(L"exit after {}m",o.exit_after));
    if(suffix(raw,L"-activefor:")) append(std::format(L"active for {}m",o.active_for));
    if(!o.periods.empty()) { std::wstring list; for(const auto& period:o.periods) { if(!list.empty()) list+=L", "; list+=period_text(period); } append(L"active during periods: "+list); }
    if(suffix(raw,L"-cpu:")) append(std::format(L"while CPU > {}%",o.cpu));
    if(!o.watch_window.empty()) append(L"active when '"+o.watch_window+L"' is present");
    if(suffix(raw,L"-key:")) append(std::format(L"use key {}",o.key));
    if(suffix(raw,L"-inactivefor:")) append(std::format(L"inactive for {}m",o.inactive_for));
    if(o.description.ends_with(L", ")) o.description.resize(o.description.size()-2);
    return o;
}

std::wstring period_text(const Period& p) { return std::format(L"{:02}:{:02}-{:02}:{:02}", p.begin/60,p.begin%60,p.end/60,p.end%60); }
Model::Model(Options options) : options_(std::move(options)), seconds_to_key_(options_.interval_seconds) {
    if(options_.interval_seconds < 1) throw std::invalid_argument("Positive interval required");
    if(options_.start_off) { toggle(); notifications_.clear(); }
    active_remaining_=options_.active_for; exit_remaining_=options_.exit_after;
    cpu_threshold_=options_.cpu; watch_window_=options_.watch_window;
    if(options_.raw.find(L"-inactivefor:") != std::wstring::npos) { toggle(); inactive_remaining_=options_.inactive_for; }
    notifications_.clear();
}
void Model::notify(std::wstring text) { if(options_.notify) notifications_.push_back(std::move(text)); }
const Period* Model::current_period(int minute) const noexcept { for(const auto& p:options_.periods) if(p.contains(minute)) return &p; return nullptr; }
void Model::toggle() {
    active_remaining_=inactive_remaining_=exit_remaining_=-1;
    active_=!active_; watch_window_.clear(); cpu_threshold_=-1; manual_=true; revert_available_=true;
}
void Model::set_active(bool value) { if(active_ != value) { notify(value ? L"active" : L"Inactive"); toggle(); } }
void Model::timed_active(int minutes) { if(minutes < 1) throw std::invalid_argument("Positive duration required"); set_active(true); active_remaining_=minutes; }
void Model::timed_inactive(int minutes) { if(minutes < 1) throw std::invalid_argument("Positive duration required"); set_active(false); inactive_remaining_=minutes; }
void Model::revert(int minute) { manual_=false; revert_available_=false; refresh_schedule(minute); }
void Model::refresh_schedule(int minute) {
    if(manual_ || options_.periods.empty()) return;
    const bool next=current_period(minute)!=nullptr;
    if(next != active_) { active_=next; notify(next ? L"active" : L"inactive"); }
}
Effects Model::tick(const Inputs& in) {
    Effects effect;
    refresh_schedule(in.minute_of_day);
    --seconds_to_key_;
    if(seconds_to_key_==0) {
        seconds_to_key_=options_.interval_seconds;
        if(active_) {
            if(options_.allow_screensaver) effect.keep_awake=KeepAwake::system;
            else if(options_.execution_state) effect.keep_awake=KeepAwake::display;
            else {
                if(options_.not_when_locked && locked_) {
                    effect.skipped_tail=true; effect.notifications=take_notifications(); return effect;
                }
                if(!in.shift_down) {
                    if((!watch_window_.empty() && !in.matching_window) || (!options_.periods.empty() && !current_period(in.minute_of_day)) || (options_.on_ac && !in.ac_power)) {
                        effect.skipped_tail=true; effect.notifications=take_notifications(); return effect;
                    }
                    effect.keep_awake=KeepAwake::key;
                    effect.key=static_cast<std::uint16_t>(options_.key==-1 ? (options_.use_shift ? 0x10 : 0x7e) : options_.key);
                    effect.key_up_only=!options_.full_keypress; // Host OS fallback is applied by the Win32 adapter.
                }
            }
        }
    }
    if(++minute_ticks_ >= 60) {
        minute_ticks_=0;
        if(active_remaining_ >= 0 && --active_remaining_ == 0) { notify(L"inactive"); set_active(false); }
        if(exit_remaining_ >= 0 && --exit_remaining_ == 0) { effect.exit=true; effect.notifications=take_notifications(); return effect; }
        if(inactive_remaining_ >= 0 && --inactive_remaining_ == 0) { notify(L"active"); set_active(true); }
    }
    if(cpu_threshold_ != -1 && active_) {
        if(in.cpu_percent && *in.cpu_percent < cpu_threshold_ && cpu_samples_ > 3) { cpu_threshold_=-1; set_active(false); }
        if(cpu_samples_ < std::numeric_limits<int>::max()) ++cpu_samples_;
    }
    effect.notifications=take_notifications();
    return effect;
}
std::vector<std::wstring> Model::take_notifications() { return std::exchange(notifications_,{}); }
std::wstring Model::status(int minute) const {
    std::wstring result;
    if(!watch_window_.empty()) result=L"Active when a window title containing '"+watch_window_+L"' is found";
    else if(active_remaining_ != -1) result=std::format(L"Will go inactive in {} minute(s)",active_remaining_);
    else if(exit_remaining_ != -1) result=std::format(L"Will exit in {} minute(s)",exit_remaining_);
    else if(inactive_remaining_ != -1) result=std::format(L"Will go active in {} minute(s)",inactive_remaining_);
    else if(cpu_threshold_ != -1) result=std::format(L"Active while CPU > {}%",cpu_threshold_);
    else if(manual_) result=active_ ? L"[Manual Override]: active" : L"[Manual Override]: inactive";
    else if(options_.periods.empty()) result=active_ ? L"active" : L"inactive";
    else if(const auto* period=current_period(minute)) result=L"Active during "+period_text(*period);
    else result=L"Inactive (not in a period)";
    if(!options_.description.empty()) result+=L"\r\n\r\n(cmd line: "+options_.description+L")";
    return result;
}
}
