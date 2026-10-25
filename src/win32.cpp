#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shellapi.h>
#include <wtsapi32.h>

#include "unsleep/core.hpp"
#include "unsleep/settings.hpp"

#include <algorithm>
#include <filesystem>
#include <format>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using namespace unsleep;

constexpr wchar_t window_class[] = L"UnsleepTrayHost";
constexpr wchar_t window_title[] = L"Unsleep";
constexpr wchar_t instance_mutex[] = L"Local\\Unsleep.SingleInstance";
constexpr UINT tray_message = WM_APP + 1;
// Posted to itself once the tray exists, to surface a failed suspend
// registration; see the suspend_note_message case in handle_message().
constexpr UINT suspend_note_message = WM_APP + 2;
constexpr UINT_PTR tick_timer = 1;
constexpr int tick_interval_ms = 500; // deadline/power check well inside the 1 s target and 5 s ceiling
constexpr UINT tray_id = 1;
constexpr UINT toggle_command = 40001;
constexpr UINT exit_command = 40002;
constexpr UINT display_command = 40006;
constexpr UINT threshold_commands[] = {40003, 40004, 40005};
constexpr UINT duration_commands[] = {40010, 40011, 40012, 40013};
constexpr int menu_resource = 129;

constexpr Duration duration_for_command(UINT id) noexcept {
    for (std::size_t index = 0; index < std::size(duration_commands); ++index) {
        if (duration_commands[index] == id) return durations[index];
    }
    return Duration::unlimited;
}

constexpr bool is_duration_command(UINT id) noexcept {
    for (const UINT candidate : duration_commands) {
        if (candidate == id) return true;
    }
    return false;
}

constexpr UINT command_for_duration(Duration duration) noexcept {
    for (std::size_t index = 0; index < durations.size(); ++index) {
        if (durations[index] == duration) return duration_commands[index];
    }
    return 0;
}

const char* duration_name(Duration duration) noexcept {
    switch (duration) {
    case Duration::unlimited: return "unlimited";
    case Duration::minutes_30: return "30m";
    case Duration::hours_1: return "1h";
    case Duration::hours_2: return "2h";
    }
    return "unknown";
}

constexpr int threshold_for_command(UINT id) noexcept {
    for (std::size_t index = 0; index < std::size(threshold_commands); ++index) {
        if (threshold_commands[index] == id) return battery_thresholds[index];
    }
    return 0;
}

constexpr UINT command_for_threshold(int percent) noexcept {
    for (std::size_t index = 0; index < battery_thresholds.size(); ++index) {
        if (battery_thresholds[index] == percent) return threshold_commands[index];
    }
    return 0;
}

constexpr int icon_app = 128;
// Tray icons. Two palettes by taskbar background (light-*.ico holds dark
// strokes for a light taskbar, dark-*.ico holds light strokes for a dark one)
// and one id per configuration combination. The table is fixed and small on
// purpose: the runtime picks an id with a lookup instead of drawing an icon.
// Kept in step with resources/unsleep.rc and resources/icons/DESIGN.md.
constexpr int icon_light_inactive = 130;
constexpr int icon_dark_inactive = 144;

// The window class carries the application icon (resource 128) so Explorer and
// Task Manager show the product icon, not a tray variant.
static_assert(icon_app == 128);

// Configuration badges carried by the icon. 0 is the plain state; SCREEN means
// "screen keep-awake selected and the display request is in force", PAUSED
// means "selected but deferred because the session is locked or unknown",
// DURATION means "a finite duration is selected".
constexpr unsigned badge_screen = 1;
constexpr unsigned badge_paused = 2;
constexpr unsigned badge_duration = 4;

// Index = badge combination; value = resource id for a light taskbar, which
// holds DARK strokes. A dark taskbar uses the same index with +5 added below.
constexpr int light_running_icons[] = {140, 150, 151, 152, 153, 154};
constexpr int dark_running_icons[] = {145, 155, 156, 157, 158, 159};

// Maps a combination to the table slot. The tables list 0, SCREEN, PAUSED,
// DURATION, SCREEN|DURATION and PAUSED|DURATION, so a combination with both the
// paused and the accepted screen mark collapses onto the paused entry: a
// deferred request must never look accepted.
constexpr unsigned badge_slot(unsigned combo) noexcept {
    switch (combo) {
    case 0: return 0;
    case badge_screen: return 1;
    case badge_paused: return 2;
    case badge_duration: return 3;
    case badge_screen | badge_duration: return 4;
    default: return 5; // paused + duration, and any paused+screen combination
    }
}

template <typename T, std::size_t N>
void copy_text(T (&destination)[N], std::wstring_view text) noexcept {
    const auto count = std::min(text.size(), N - 1);
    std::copy_n(text.data(), count, destination);
    destination[count] = L'\0';
}

void copy_text(wchar_t* destination, std::size_t size, std::wstring_view text) noexcept {
    const auto count = std::min(text.size(), size - 1);
    std::copy_n(text.data(), count, destination);
    destination[count] = L'\0';
}

// The two central trace sinks. They are noexcept and are called from noexcept
// teardown paths (window_proc, fatal, the unregister helpers, release_request),
// so nothing may escape them: building the prefixed line copies the caller's
// text into a fresh buffer and therefore allocates, and an allocation failure
// here would call std::terminate before the caller's own try/catch could see it.
// The prefix is kept on the normal path (the trace format is unchanged); when
// the copy fails only the fixed literal marker is emitted, which allocates
// nothing. A string that cannot be built cannot report its text, so the marker
// deliberately carries no values.
void log(std::wstring_view text) noexcept {
    constexpr wchar_t fallback[] = L"Unsleep: <log line unavailable>\n";
    try {
        OutputDebugStringW(std::wstring(L"Unsleep: ").append(text).append(L"\n").c_str());
    } catch (...) {
        OutputDebugStringW(fallback);
    }
}

void log(std::string_view text) noexcept {
    constexpr char fallback[] = "Unsleep: <log line unavailable>\n";
    try {
        OutputDebugStringA(std::string("Unsleep: ").append(text).append("\n").c_str());
    } catch (...) {
        OutputDebugStringA(fallback);
    }
}

void log_utf8(std::wstring_view text) {
    if (text.empty()) return;
    const int size = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0,
                                         nullptr, nullptr);
    if (size <= 0) return;
    std::string utf8(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), utf8.data(), size, nullptr,
                        nullptr);
    log(std::string_view(utf8));
}

std::filesystem::path executable_path() {
    std::wstring buffer(32768, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (length == 0 || length >= buffer.size()) return {};
    buffer.resize(length);
    return std::filesystem::path(buffer);
}

const char* load_status_name(ThresholdLoadStatus status) noexcept {
    switch (status) {
    case ThresholdLoadStatus::loaded: return "loaded";
    case ThresholdLoadStatus::missing: return "missing";
    case ThresholdLoadStatus::invalid: return "invalid";
    case ThresholdLoadStatus::read_failed: return "read_failed";
    }
    return "unknown";
}

const char* save_status_name(ThresholdSaveStatus status) noexcept {
    switch (status) {
    case ThresholdSaveStatus::saved: return "saved";
    case ThresholdSaveStatus::invalid_value: return "invalid_value";
    case ThresholdSaveStatus::write_failed: return "write_failed";
    }
    return "unknown";
}

// Whether the taskbar is light (bright background, so icons need DARK strokes).
// The name describes the BACKGROUND; the matching icon family is the opposite
// palette. The previous revision inverted this relation and selected light
// strokes for a light background.
bool light_taskbar() noexcept {
    DWORD value = 0;
    DWORD size = sizeof(value);
    DWORD type = 0;
    return RegGetValueW(HKEY_CURRENT_USER,
                        L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                        L"SystemUsesLightTheme", RRF_RT_REG_DWORD, &type, &value, &size) == ERROR_SUCCESS &&
           type == REG_DWORD && value != 0;
}

BatterySample sample_battery() noexcept {
    BatterySample sample;
    SYSTEM_POWER_STATUS status{};
    if (!GetSystemPowerStatus(&status)) {
        sample.read_failed = true;
        return sample;
    }
    if (status.BatteryFlag == 255) {
        sample.presence = BatteryPresence::unknown;
    } else if ((status.BatteryFlag & 128) != 0) {
        sample.presence = BatteryPresence::absent;
    } else {
        sample.presence = BatteryPresence::present;
    }
    sample.ac = status.ACLineStatus == 0 ? AcState::unplugged
               : status.ACLineStatus == 1 ? AcState::plugged
                                          : AcState::unknown;
    if (status.BatteryLifePercent != 255) sample.percent = static_cast<int>(status.BatteryLifePercent);
    return sample;
}

const char* session_name(SessionState state) noexcept {
    switch (state) {
    case SessionState::unknown: return "unknown";
    case SessionState::unlocked: return "unlocked";
    case SessionState::locked: return "locked";
    }
    return "unknown";
}

// Reads the current lock state once at startup. Lock state cannot be inferred
// from a missing event, so an unreadable value is reported as unknown and the
// model keeps screen keep-awake paused until an explicit unlock notification.
std::optional<bool> query_session_locked() noexcept {
    LPWSTR buffer = nullptr;
    DWORD bytes = 0;
    if (!WTSQuerySessionInformationW(WTS_CURRENT_SERVER_HANDLE, WTS_CURRENT_SESSION, WTSSessionInfoEx,
                                     &buffer, &bytes) ||
        !buffer) {
        return std::nullopt;
    }
    std::optional<bool> locked;
    if (bytes >= sizeof(WTSINFOEXW)) {
        const auto* info = reinterpret_cast<const WTSINFOEXW*>(buffer);
        if (info->Level == 1) {
            const LONG flags = info->Data.WTSInfoExLevel1.SessionFlags;
            if (flags == static_cast<LONG>(WTS_SESSIONSTATE_LOCK)) {
                locked = true;
            } else if (flags == static_cast<LONG>(WTS_SESSIONSTATE_UNLOCK)) {
                locked = false;
            }
        }
    }
    WTSFreeMemory(buffer);
    return locked;
}

const char* reason_name(StopReason reason) noexcept {
    switch (reason) {
    case StopReason::none: return "none";
    case StopReason::manual: return "manual";
    case StopReason::expired: return "expired";
    case StopReason::low_battery: return "low_battery";
    case StopReason::battery_unknown: return "battery_unknown";
    case StopReason::request_failed: return "request_failed";
    case StopReason::sleep: return "sleep";
    }
    return "unknown";
}

const char* ac_name(AcState state) noexcept {
    switch (state) {
    case AcState::unknown: return "unknown";
    case AcState::plugged: return "plugged";
    case AcState::unplugged: return "unplugged";
    }
    return "unknown";
}

const char* presence_name(BatteryPresence presence) noexcept {
    switch (presence) {
    case BatteryPresence::unknown: return "unknown";
    case BatteryPresence::absent: return "absent";
    case BatteryPresence::present: return "present";
    }
    return "unknown";
}

// --- tooltip -------------------------------------------------------------
//
// NOTIFYICONDATAW::szTip is 128 wide characters INCLUDING the terminator, so at
// most 127 characters are stored. The text is assembled from short fields and
// every addition is measured first: the state and its reason come first
// because they answer "is it working right now", then the selected options,
// then the secondary flags. When the budget is tight the secondary flags are
// dropped first, so the state, reason and selected options are never the part
// that gets cut, and no field is ever truncated in the middle.
inline constexpr std::size_t sz_tip_capacity = 128;
inline constexpr std::size_t sz_tip_max = sz_tip_capacity - 1;

const wchar_t* short_duration_name(Duration duration) noexcept {
    switch (duration) {
    case Duration::unlimited: return L"不限时";
    case Duration::minutes_30: return L"30分";
    case Duration::hours_1: return L"1小时";
    case Duration::hours_2: return L"2小时";
    }
    return L"不限时";
}

// "时长 2小时 剩余 1 小时 59 分". Remaining time is coalesced to minutes: a
// per-second tooltip refreshed every 500 ms is noise, and the exact instant is
// not part of the product's promise.
std::wstring duration_field(const Model& model) {
    const std::wstring name = short_duration_name(model.duration());
    const auto remaining = model.remaining_time();
    if (!remaining) return std::format(L"时长 {}", name);
    const auto total = remaining->count();
    if (total >= 3600) {
        return std::format(L"时长 {} 剩余 {} 小时 {} 分", name, total / 3600, (total % 3600) / 60);
    }
    if (total >= 60) return std::format(L"时长 {} 剩余 {} 分", name, total / 60);
    return std::format(L"时长 {} 剩余 {} 秒", name, total);
}

// "屏幕常亮:开" / "屏幕常亮:锁屏暂缓" / "屏幕常亮:请求中" / "屏幕常亮:关".
// The deferred wording is used whenever the selection exists but the display
// request is not in force, so the tooltip never claims a request the system is
// not actually receiving.
std::wstring screen_field(const Model& model) {
    if (!model.running() || !model.display_selected()) return L"屏幕常亮:关";
    if (model.display_confirmed()) return L"屏幕常亮:开";
    if (model.session_state() == SessionState::unlocked) return L"屏幕常亮:请求中";
    return L"屏幕常亮:锁屏暂缓";
}

// One-line, space-free record of the suspend/resume subscription. It is part of
// the state trace so a verifier can tell a real registration (`registered`) from
// a failed one (`failed`, with the Win32 error) without trusting the tooltip.
// `release` records the unregister result: -1 not attempted, 0 failed, 1 done.
std::string suspend_registration_line(const char* state, int release, unsigned long error) {
    return std::format("suspend_registration state={} api=RegisterSuspendResumeNotification "
                       "flag=DEVICE_NOTIFY_WINDOW_HANDLE release={} error={}",
                       state, release, error);
}

// Diagnostics that must be emitted from a noexcept teardown path. Formatting a
// trace line allocates, and an allocation failure there must not terminate the
// process before the API unregister and the handle/flag cleanup have run, so the
// line is formatted defensively and a fixed literal is used when it fails. The
// literal carries no values because the values are exactly what could not be
// formatted; the line is still recognisable in the trace. Every other function
// in this block that formats from a noexcept caller does the same.
void log_suspend_registration(const char* state, int release, unsigned long error) noexcept {
    try {
        log(suspend_registration_line(state, release, error));
    } catch (...) {
        log("suspend_registration state=unavailable");
    }
}

void log_suspend_release_failure(unsigned long error) noexcept {
    try {
        log(std::format("suspend notification unregister failed ({})", error));
    } catch (...) {
        log("suspend notification unregister failed");
    }
}

void log_session_release_failure(unsigned long error) noexcept {
    try {
        log(std::format("session notification unregister failed ({})", error));
    } catch (...) {
        log("session notification unregister failed");
    }
}

// Builds the tray tooltip. `capacity` is the maximum number of wide characters
// the caller can store, excluding the terminator; it is a parameter so the real
// 127-character budget can be exercised without a tray. `suspend_subscribed`
// says whether the process really holds a suspend/resume subscription: when it
// does not, the limitation is stated while running instead of claiming sleep
// protection.
std::wstring compose_tooltip(const Model& model, std::size_t capacity = sz_tip_max,
                            bool suspend_subscribed = true) {
    std::wstring text = L"Unsleep · " + model.status_text();
    const auto try_append = [&](const std::wstring& field) {
        if (field.empty()) return;
        const std::wstring candidate = text + L" · " + field;
        if (candidate.size() <= capacity) text = candidate;
    };
    // Selected options: battery threshold always, because it stays editable
    // while stopped, then the two per-session selections.
    try_append(std::format(L"电池 {}%", model.threshold()));
    try_append(duration_field(model));
    try_append(screen_field(model));
    // Secondary flags, in the order the model reports them.
    if (!model.threshold_persisted()) try_append(L"设置未保存");
    if (model.release_unconfirmed()) try_append(L"释放未确认");
    if (model.display_release_unconfirmed()) try_append(L"屏幕释放未确认");
    // The device limit, stated as a limit: without the subscription this build
    // cannot observe a Modern Standby transition, so a running session may
    // survive sleep. It is a secondary flag in the same budget as the others;
    // the state text above never claims a successful subscription.
    if (model.running() && !suspend_subscribed) try_append(L"睡眠通知未订阅");
    // Hard guarantee: even a pathological state string cannot overflow the
    // buffer the caller copies into.
    if (text.size() > capacity) text.resize(capacity);
    return text;
}

class Application {
public:
    explicit Application(HINSTANCE instance) noexcept : instance_(instance) {}

    Application(const Application&) = delete;
    Application& operator=(const Application&) = delete;

    ~Application() {
        // A window that still exists is torn down first so WM_DESTROY runs the
        // one place that releases the subscriptions; the calls after it are
        // idempotent no-ops for a window that was already destroyed.
        if (window_ && IsWindow(window_)) DestroyWindow(window_);
        unregister_suspend_notification();
        unregister_session_notification();
        remove_tray();
        release_request();
    }

    int run() {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = &Application::window_proc;
        wc.hInstance = instance_;
        wc.lpszClassName = window_class;
        wc.hIcon = LoadIconW(instance_, MAKEINTRESOURCEW(icon_app));
        wc.hIconSm = wc.hIcon;
        if (!RegisterClassExW(&wc)) {
            log("fatal: RegisterClassEx failed");
            return 1;
        }
        window_ = CreateWindowExW(0, window_class, window_title, WS_POPUP, 0, 0, 0, 0, nullptr, nullptr,
                                  instance_, this);
        if (!window_) {
            log("fatal: CreateWindowEx failed");
            return 1;
        }
        add_tray();
        register_session_notifications();
        // The suspend/resume subscription is owned by the window that now
        // exists, so it is registered here: every later path has a handle to
        // release and the handle is never associated with a stale HWND.
        register_suspend_notification();
        load_settings();
        model_.observe(sample_battery());
        if (!model_.start()) {
            log(std::format("launch start refused reason={}", reason_name(model_.stop_reason())));
        }
        after_change();
        // register_suspend_notification() logs before the tray exists, so the
        // balloon is delivered here, once the tray can carry it.
        if (!suspend_subscribed_) PostMessageW(window_, suspend_note_message, 0, 0);
        if (!SetTimer(window_, tick_timer, tick_interval_ms, nullptr)) {
            log("fatal: SetTimer failed");
            return 1;
        }
        MSG message{};
        int result = 0;
        while ((result = GetMessageW(&message, nullptr, 0, 0)) > 0) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        if (result < 0) log("fatal: GetMessage failed");
        return failed_ || result < 0 ? 1 : 0;
    }

private:
    static LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) noexcept {
        Application* self = reinterpret_cast<Application*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            self = static_cast<Application*>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams);
            self->window_ = window;
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        if (!self) return DefWindowProcW(window, message, wparam, lparam);
        try {
            return self->handle_message(window, message, wparam, lparam);
        } catch (const std::exception& error) {
            self->fatal(error.what());
        } catch (...) {
            self->fatal("unexpected error");
        }
        return 0;
    }

    void fatal(const char* what) noexcept {
        if (failed_) return;
        failed_ = true;
        log(what);
        remove_tray();
        // DestroyWindow() below runs WM_DESTROY, which releases both
        // subscriptions; these explicit calls cover a window that is already
        // gone. Both are idempotent.
        unregister_suspend_notification();
        unregister_session_notification();
        release_request();
        if (window_ && IsWindow(window_)) DestroyWindow(window_);
        PostQuitMessage(1);
    }

    LRESULT handle_message(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
        if (taskbar_created_ != 0 && message == taskbar_created_) {
            add_tray();
            refresh();
            flush_notifications();
            return 0;
        }
        switch (message) {
        case WM_TIMER:
            if (wparam == tick_timer) tick();
            return 0;
        case WM_POWERBROADCAST:
            return power_broadcast(wparam);
        case WM_WTSSESSION_CHANGE:
            session_change(wparam);
            return 0;
        case suspend_note_message:
            // One-time feedback that the subscription is missing. The wording
            // states the limitation; it never claims sleep protection.
            if (!suspend_subscribed_) {
                balloon(L"睡眠通知未订阅：系统睡眠时可能无法自动停止");
            }
            return 0;
        case WM_COMMAND:
            command(LOWORD(wparam));
            return 0;
        case tray_message:
            tray_event(LOWORD(lparam));
            return 0;
        case WM_ENTERMENULOOP:
            // The session can stop, expire or fail while the native menu owns
            // the UI loop; refresh the marks before Windows paints it so the
            // menu never shows a state the model has already left.
            if (open_popup_) apply_menu_state(open_popup_);
            return 0;
        case WM_CLOSE:
            DestroyWindow(window);
            return 0;
        case WM_DESTROY:
            KillTimer(window, tick_timer);
            // Drop the non-owning reference so a window torn down while the
            // popup is open never leaves a dangling HMENU here; the MenuGuard
            // in show_menu() still owns and destroys the menu itself.
            open_popup_ = nullptr;
            // The window that owns both subscriptions is going away: this is the
            // one place that releases them. Each call is idempotent, so the
            // destructor and fatal() can repeat it safely.
            unregister_suspend_notification();
            unregister_session_notification();
            remove_tray();
            release_request();
            window_ = nullptr;
            PostQuitMessage(0);
            return 0;
        default:
            return DefWindowProcW(window, message, wparam, lparam);
        }
    }

    LRESULT power_broadcast(WPARAM event) {
        switch (event) {
        case PBT_APMSUSPEND:
        case PBT_APMRESUMEAUTOMATIC:
        case PBT_APMRESUMESUSPEND:
            // Conservative rule: sleep and resume both end the current session,
            // so a missed pre-sleep notification still cannot leave a request
            // behind. The model keeps the most urgent stop reason, and only a
            // fresh user start can begin again.
            log(std::format("power event={} stops the session (running_before={})",
                            static_cast<unsigned long>(event), model_.running() ? 1 : 0));
            model_.stop(StopReason::sleep);
            after_change();
            break;
        case PBT_APMPOWERSTATUSCHANGE:
            tick();
            break;
        default:
            break;
        }
        return TRUE;
    }

    // Session notifications are the lock/unlock signal. Registration failure is
    // checked, logged and reported through the state trace; without it the lock
    // state cannot be known, so screen keep-awake stays paused instead of
    // being assumed active.
    void session_change(WPARAM event) {
        if (event == WTS_SESSION_LOCK) {
            log("session locked; releasing display request and keeping system request");
            model_.observe_session(SessionState::locked);
            after_change();
        } else if (event == WTS_SESSION_UNLOCK) {
            log("session unlocked; restoring display request only if still selected");
            model_.observe_session(SessionState::unlocked);
            after_change();
        }
    }

    void tick() {
        if (!tray_added_) add_tray();
        const bool was_running = model_.running();
        // The model refreshes its monotonic clock here and stops the session
        // as soon as the deadline has passed (timer interval < 1 s).
        model_.observe(sample_battery());
        if (was_running && !model_.running() && model_.stop_reason() == StopReason::expired) {
            // Only the intent is known here; reconcile() reports the real API
            // result of the release that follows.
            log("duration expiry reached; session stopped, requesting full release");
        }
        after_change();
    }

    void after_change() {
        reconcile();
        refresh();
        // A menu can be open while a timer, battery, power or session event
        // stops the session; update its marks immediately instead of waiting
        // for the next WM_ENTERMENULOOP.
        if (open_popup_) apply_menu_state(open_popup_);
        flush_notifications();
    }

    // Registers for lock/unlock notifications and reads the initial lock state
    // before any display request can be considered. A session that is already
    // locked is therefore never assumed to be unlocked.
    void register_session_notifications() {
        const BOOL registered = WTSRegisterSessionNotification(window_, NOTIFY_FOR_THIS_SESSION);
        session_notifications_registered_ = registered != FALSE;
        if (!session_notifications_registered_) {
            log(std::format("session notification registration failed ({}); screen keep-awake stays paused",
                            GetLastError()));
            model_.observe_session(SessionState::unknown);
            return;
        }
        log("session notifications registered");
        const auto locked = query_session_locked();
        if (!locked) {
            log("initial session state unknown; treating it as locked until an unlock notification");
            model_.observe_session(SessionState::unknown);
            return;
        }
        log(*locked ? "initial session state=locked" : "initial session state=unlocked");
        model_.observe_session(*locked ? SessionState::locked : SessionState::unlocked);
        log(std::format("initial session state applied={}", session_name(model_.session_state())));
    }

    // Registers for suspend/resume notifications through
    // RegisterSuspendResumeNotification(hwnd, DEVICE_NOTIFY_WINDOW_HANDLE).
    //
    // why: WTSRegisterSessionNotification only reports lock/unlock; Windows
    // 8+ Modern Standby transitions reach a desktop app through the
    // power-notification subscription, which is the documented subscription for
    // a window-based desktop app. Without it a session that the user suspends is
    // not told about the transition and keeps its request. Note what this is
    // and is not: S0 "screen off" is not a user sleep, and this notification is
    // the sleep/resume signal only - it is never used to treat a display
    // power-off as a stop, so the documented "may allow the display to turn
    // off" behaviour is unchanged.
    //
    // The returned HPOWERNOTIFY belongs to the window: it is stored only here
    // and released exactly once by unregister_suspend_notification(). The call
    // returns NULL on failure with GetLastError set; that is diagnosed, recorded
    // in the state trace and, if a later start needs it, reported to the user as
    // a limitation rather than as a successful protection.
    void register_suspend_notification() {
        // Idempotent: one pending or live registration per process. A second
        // call (a duplicate trigger) cannot create a second subscription that
        // would then have to be released twice.
        if (suspend_registered_ || suspend_handle_) return;
        suspend_registered_ = true;
        SetLastError(ERROR_SUCCESS);
        suspend_handle_ =
            RegisterSuspendResumeNotification(window_, DEVICE_NOTIFY_WINDOW_HANDLE);
        const auto error = suspend_handle_ ? ERROR_SUCCESS : GetLastError();
        suspend_subscribed_ = suspend_handle_ != nullptr;
        suspend_registration_error_ = suspend_handle_ ? 0 : static_cast<unsigned long>(error);
        log(suspend_registration_line(suspend_subscribed_ ? "registered" : "failed", -1,
                                      suspend_registration_error_));
        if (!suspend_subscribed_) {
            log(std::format("suspend notification registration failed ({}); sleep/resume will not be "
                            "observed, the session keeps running across sleep and the tray reports it",
                            suspend_registration_error_));
        }
    }

    // Releases the subscription if this application holds one. Safe to call on
    // every teardown path and repeatedly: the handle is cleared before the call
    // returns, so a second call cannot double-free it. A failed unregister is
    // logged with its error and recorded in the trace; the handle is still
    // dropped because the process is going away and a retry loop is not wanted.
    //
    // The handle and the flag are dropped first and the diagnostic is emitted
    // last, so no allocation failure in the log formatting can interrupt the
    // cleanup or, through terminate(), skip the session cleanup that the
    // teardown sequence performs right after this call.
    void unregister_suspend_notification() noexcept {
        if (!suspend_registered_ && !suspend_handle_) return;
        const HPOWERNOTIFY handle = suspend_handle_;
        suspend_handle_ = nullptr;
        suspend_registered_ = false;
        if (!handle) return;
        SetLastError(ERROR_SUCCESS);
        const BOOL released = UnregisterSuspendResumeNotification(handle);
        const auto error = released ? ERROR_SUCCESS : GetLastError();
        log_suspend_registration("unregistered", released ? 1 : 0,
                                 released ? 0UL : static_cast<unsigned long>(error));
        if (!released) log_suspend_release_failure(static_cast<unsigned long>(error));
    }

    // Releases the session (lock/unlock) subscription. Same contract as the
    // suspend one: idempotent, clears its flag before returning, logs failure.
    // The flag is cleared before the API call and the diagnostics are
    // allocation-safe, for the same reason as above.
    void unregister_session_notification() noexcept {
        if (!session_notifications_registered_) return;
        session_notifications_registered_ = false;
        SetLastError(ERROR_SUCCESS);
        const BOOL released = WTSUnRegisterSessionNotification(window_);
        const auto error = released ? ERROR_SUCCESS : GetLastError();
        if (!released) {
            log_session_release_failure(static_cast<unsigned long>(error));
        } else {
            log("session notifications unregistered");
        }
    }

    // Loads the persisted threshold (or the 20% default when the file is
    // missing, unreadable or invalid) before the first battery check.
    void load_settings() {
        settings_file_ = settings_path_for(executable_path());
        const auto loaded = load_threshold(settings_file_);
        const auto change = model_.set_threshold(loaded.threshold);
        log(std::format("settings load status={} threshold={}", load_status_name(loaded.status),
                        loaded.threshold));
        if (change == ThresholdChange::rejected) {
            log("settings threshold rejected; keeping the current threshold");
        }
    }

    // Executes the actions queued by the core model and reports the real API
    // results back. A queued action may enqueue another one (for example a
    // failed apply is followed by a release).
    void reconcile() {
        for (;;) {
            const auto actions = model_.take_actions();
            if (actions.empty()) break;
            for (const auto action : actions) {
                if (action == Action::apply_system_request) {
                    SetLastError(ERROR_SUCCESS);
                    const BOOL accepted = SetThreadExecutionState(ES_CONTINUOUS | ES_SYSTEM_REQUIRED);
                    const auto error = accepted ? ERROR_SUCCESS : GetLastError();
                    system_request_held_ = accepted != FALSE;
                    if (accepted) {
                        log("system request accepted");
                    } else {
                        log(std::format("system request failed ({})", error));
                    }
                    model_.on_request_result(accepted != FALSE, static_cast<int>(error));
                } else if (action == Action::apply_display_request) {
                    SetLastError(ERROR_SUCCESS);
                    const BOOL accepted = SetThreadExecutionState(
                        ES_CONTINUOUS | ES_SYSTEM_REQUIRED | ES_DISPLAY_REQUIRED);
                    const auto error = accepted ? ERROR_SUCCESS : GetLastError();
                    if (accepted) {
                        system_request_held_ = true;
                        display_request_held_ = true;
                        log("display request accepted");
                    } else {
                        log(std::format("display request failed ({})", error));
                    }
                    model_.on_display_result(accepted != FALSE, static_cast<int>(error));
                } else if (action == Action::release_display_request &&
                           !model_.requires_full_release(action)) {
                    // Keep the system request while dropping the display one.
                    SetLastError(ERROR_SUCCESS);
                    const BOOL released = SetThreadExecutionState(ES_CONTINUOUS | ES_SYSTEM_REQUIRED);
                    const auto error = released ? ERROR_SUCCESS : GetLastError();
                    if (released) display_request_held_ = false;
                    if (released) {
                        log("display request released (system request retained)");
                    } else {
                        log(std::format("display request release failed ({})", error));
                    }
                    model_.on_display_release_result(released != FALSE, static_cast<int>(error));
                } else {
                    if (action == Action::release_display_request) {
                        log("stale display release after stop; releasing all requests");
                    }
                    SetLastError(ERROR_SUCCESS);
                    const BOOL released = SetThreadExecutionState(ES_CONTINUOUS);
                    const auto error = released ? ERROR_SUCCESS : GetLastError();
                    if (released) {
                        system_request_held_ = false;
                        display_request_held_ = false;
                    }
                    if (released) {
                        log("system request released");
                    } else {
                        log(std::format("system request release failed ({})", error));
                        log("release not confirmed; reporting unconfirmed state");
                    }
                    model_.on_release_result(released != FALSE);
                }
            }
        }
    }

    // Best-effort cleanup on exit; a failure is reported in the debug trace
    // and cannot be shown in the tray once the icon is gone.
    void release_request() noexcept {
        if (!system_request_held_ && !display_request_held_) return;
        if (SetThreadExecutionState(ES_CONTINUOUS)) {
            system_request_held_ = false;
            display_request_held_ = false;
            log("system request released on exit");
        } else {
            try {
                log(std::format("system request release failed on exit ({})", GetLastError()));
            } catch (...) {
                log("system request release failed on exit");
            }
        }
    }

    // Which configuration badges the icon must carry. Only selections that
    // belong to the current running session are marked; a stop clears them all,
    // so a stopped icon can never suggest that a request is still in force.
    unsigned icon_badges() const noexcept {
        if (!model_.running()) return 0;
        unsigned badges = 0;
        if (model_.display_selected()) {
            // "Accepted" requires the display request to be confirmed. A
            // selection that is merely remembered (locked or unknown session,
            // or a request still in flight) is drawn as an outline.
            badges |= model_.display_confirmed() ? badge_screen : badge_paused;
        }
        if (model_.duration() != Duration::unlimited) badges |= badge_duration;
        return badges;
    }

    // Resource id for the current background and configuration. A small lookup,
    // no runtime drawing and no cache: LoadIconW reads the already-embedded
    // icon, so switching state costs one resource lookup.
    int icon_for_state() const noexcept {
        if (!model_.running()) {
            return light_taskbar() ? icon_light_inactive : icon_dark_inactive;
        }
        const unsigned slot = badge_slot(icon_badges());
        return light_taskbar() ? light_running_icons[slot] : dark_running_icons[slot];
    }

    HICON load_state_icon() const noexcept {
        return LoadIconW(instance_, MAKEINTRESOURCEW(icon_for_state()));
    }

    void refresh() {
        const std::wstring tooltip = compose_tooltip(model_, sz_tip_max, suspend_subscribed_);
        const int icon_id = icon_for_state();
        if (tray_added_) {
            NOTIFYICONDATAW data{};
            data.cbSize = sizeof(data);
            data.hWnd = window_;
            data.uID = tray_id;
            data.uFlags = NIF_ICON | NIF_TIP;
            data.hIcon = load_state_icon();
            copy_text(data.szTip, std::size(data.szTip), tooltip);
            if (!Shell_NotifyIconW(NIM_MODIFY, &data)) log("tray update failed");
        }
        if (tooltip != last_tooltip_) {
            last_tooltip_ = tooltip;
            // The tooltip is the ordinary in-place feedback channel; this trace
            // makes its exact text inspectable without a popup, and records the
            // resource id actually loaded so the icon choice is verifiable.
            log_utf8(std::format(L"tooltip chars={} icon={} text={}", tooltip.size(), icon_id,
                                 tooltip));
        }
        // One space-free line so an external checker can compare the selected
        // resource and the badge bits with the model state exactly.
        const std::string icon_line = std::format(
            "icon id={} background={} badges={} running={} display_selected={} display_confirmed={} "
            "session={} duration={}",
            icon_id, light_taskbar() ? "light" : "dark", icon_badges(), model_.running() ? 1 : 0,
            model_.display_selected() ? 1 : 0, model_.display_confirmed() ? 1 : 0,
            session_name(model_.session_state()), duration_name(model_.duration()));
        if (icon_line != last_icon_line_) {
            last_icon_line_ = icon_line;
            log(icon_line);
        }
        const std::string state = std::format(
            "state running={} confirmed={} reason={} threshold={} release_unconfirmed={} persisted={} "
            "display_selected={} display_confirmed={} display_release_unconfirmed={} session={} "
            "duration={} remaining={} suspend_subscribed={}",
            model_.running() ? 1 : 0, model_.request_confirmed() ? 1 : 0,
            reason_name(model_.stop_reason()), model_.threshold(),
            model_.release_unconfirmed() ? 1 : 0, model_.threshold_persisted() ? 1 : 0,
            model_.display_selected() ? 1 : 0, model_.display_confirmed() ? 1 : 0,
            model_.display_release_unconfirmed() ? 1 : 0, session_name(model_.session_state()),
            duration_name(model_.duration()),
            model_.remaining_time() ? model_.remaining_time()->count() : -1,
            suspend_subscribed_ ? 1 : 0);
        if (state != last_state_line_) {
            last_state_line_ = state;
            log(state);
        }
    }

    void flush_notifications() {
        // Keep notifications queued in the model until a tray icon exists so
        // that one-time feedback is not lost while Explorer restarts.
        if (!tray_added_) return;
        for (const auto& text : model_.take_notifications()) balloon(text);
    }

    void balloon(std::wstring_view text) noexcept {
        NOTIFYICONDATAW data{};
        data.cbSize = sizeof(data);
        data.hWnd = window_;
        data.uID = tray_id;
        data.uFlags = NIF_INFO;
        copy_text(data.szInfo, std::size(data.szInfo), text);
        copy_text(data.szInfoTitle, std::size(data.szInfoTitle), L"Unsleep");
        data.dwInfoFlags = NIIF_INFO;
        data.uTimeout = 10000;
        if (!Shell_NotifyIconW(NIM_MODIFY, &data)) log("tray notification failed");
    }

    void add_tray() {
        NOTIFYICONDATAW data{};
        data.cbSize = sizeof(data);
        data.hWnd = window_;
        data.uID = tray_id;
        data.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
        data.uCallbackMessage = tray_message;
        data.hIcon = load_state_icon();
        copy_text(data.szTip, std::size(data.szTip), compose_tooltip(model_, sz_tip_max, suspend_subscribed_));
        tray_added_ = Shell_NotifyIconW(NIM_ADD, &data) != FALSE;
        if (!tray_added_) log("tray icon add failed");
    }

    void remove_tray() noexcept {
        if (!tray_added_) return;
        NOTIFYICONDATAW data{};
        data.cbSize = sizeof(data);
        data.hWnd = window_;
        data.uID = tray_id;
        Shell_NotifyIconW(NIM_DELETE, &data);
        tray_added_ = false;
    }

    // Left and right click open the same native menu; there is no
    // double-click action and no extra status window.
    void tray_event(UINT event) {
        switch (event) {
        case WM_LBUTTONUP:
        case WM_RBUTTONUP:
            show_menu();
            break;
        case WM_LBUTTONDBLCLK:
            break;
        default:
            break;
        }
    }

    // Writes the model's current state into a loaded popup menu. Kept separate
    // from show_menu() so the same code runs before the menu opens and again
    // whenever the session changes while the menu is still on screen (timer,
    // battery, power or session events can stop it while it is open).
    // The check marks always describe the model, never a request result.
    void apply_menu_state(HMENU popup) noexcept {
        if (!popup) return;
        if (CheckMenuItem(popup, toggle_command,
                          MF_BYCOMMAND | (model_.running() ? MF_CHECKED : MF_UNCHECKED)) ==
            static_cast<UINT>(-1)) {
            log("keep-awake menu state check failed");
        }
        // CheckMenuItem only owns the check mark; it does not enable or disable
        // an item, and MF_GRAYED there is meaningless. Checking and enabling are
        // therefore two separate calls: the mark shows the selection the user
        // made, EnableMenuItem decides whether it can still be chosen.
        if (CheckMenuItem(popup, display_command,
                          MF_BYCOMMAND | (model_.display_selected() ? MF_CHECKED : MF_UNCHECKED)) ==
            static_cast<UINT>(-1)) {
            log("screen keep-awake menu state check failed");
        }
        // Screen keep-awake is this-session only and disabled while stopped.
        if (EnableMenuItem(popup, display_command,
                           MF_BYCOMMAND | (model_.running() ? MF_ENABLED : MF_GRAYED)) ==
            static_cast<UINT>(-1)) {
            log("screen keep-awake menu enable state check failed");
        }
        // Locate the threshold and duration submenus by their command ids
        // rather than by a fixed index, so the menu can gain items without
        // mis-checking radios.
        HMENU battery_menu = nullptr;
        HMENU duration_menu = nullptr;
        int duration_position = -1;
        const int top_count = GetMenuItemCount(popup);
        for (int index = 0; index < top_count; ++index) {
            MENUITEMINFOW item{};
            item.cbSize = sizeof(item);
            item.fMask = MIIM_SUBMENU;
            if (!GetMenuItemInfoW(popup, static_cast<UINT>(index), TRUE, &item) || !item.hSubMenu) continue;
            if (!battery_menu &&
                GetMenuState(item.hSubMenu, threshold_commands[0], MF_BYCOMMAND) != static_cast<UINT>(-1)) {
                battery_menu = item.hSubMenu;
            }
            if (!duration_menu &&
                GetMenuState(item.hSubMenu, duration_commands[0], MF_BYCOMMAND) != static_cast<UINT>(-1)) {
                duration_menu = item.hSubMenu;
                duration_position = index;
            }
        }
        // The battery threshold stays editable while stopped.
        if (battery_menu) {
            const UINT checked_command = command_for_threshold(model_.threshold());
            if (checked_command == 0 ||
                !CheckMenuRadioItem(battery_menu, threshold_commands[0],
                                    threshold_commands[std::size(threshold_commands) - 1], checked_command,
                                    MF_BYCOMMAND)) {
                log("battery threshold radio check failed");
            }
        } else {
            log("battery protection submenu missing");
        }
        // Duration is this-session only and disabled while stopped; the
        // check mark shows the selected tier, not the remaining time.
        if (duration_menu && duration_position >= 0) {
            const UINT checked_command = command_for_duration(model_.duration());
            if (checked_command == 0 ||
                !CheckMenuRadioItem(duration_menu, duration_commands[0],
                                    duration_commands[std::size(duration_commands) - 1], checked_command,
                                    MF_BYCOMMAND)) {
                log("duration radio check failed");
            }
            if (EnableMenuItem(popup, static_cast<UINT>(duration_position),
                               MF_BYPOSITION | (model_.running() ? MF_ENABLED : MF_GRAYED)) ==
                static_cast<UINT>(-1)) {
                log("duration menu enable state check failed");
            }
        } else {
            log("duration submenu missing");
        }
        // Read the real HMENU back instead of trusting the model: every value
        // below comes from GetMenuState/GetMenuItemInfoW on the live popup, so an
        // API call that silently did nothing (the previous MF_GRAYED misuse of
        // CheckMenuItem) is visible instead of inferred from model fields.
        // One line, fixed field order, no spaces: an external checker can compare
        // it with the expected state without matching a sentence.
        const UINT toggle_native = GetMenuState(popup, toggle_command, MF_BYCOMMAND);
        const UINT display_native = GetMenuState(popup, display_command, MF_BYCOMMAND);
        MENUITEMINFOW duration_parent{};
        duration_parent.cbSize = sizeof(duration_parent);
        duration_parent.fMask = MIIM_STATE | MIIM_SUBMENU;
        const bool duration_parent_ok =
            duration_menu && duration_position >= 0 &&
            GetMenuItemInfoW(popup, static_cast<UINT>(duration_position), TRUE, &duration_parent);
        int threshold_enabled = 0;
        if (battery_menu) {
            threshold_enabled = 1;
            for (UINT id : threshold_commands) {
                if (GetMenuState(battery_menu, id, MF_BYCOMMAND) == static_cast<UINT>(-1)) threshold_enabled = 0;
            }
        }
        const auto is_disabled = [](UINT item_state) {
            return (item_state & (MF_DISABLED | MF_GRAYED)) != 0;
        };
        const auto enabled_flag = [&](UINT item_state) {
            return item_state == static_cast<UINT>(-1) ? -1 : (is_disabled(item_state) ? 0 : 1);
        };
        const int toggle_checked = (toggle_native & MF_CHECKED) != 0 ? 1 : 0;
        const int display_checked =
            (display_native != static_cast<UINT>(-1) && (display_native & MF_CHECKED) != 0) ? 1 : 0;
        const int display_enabled = enabled_flag(display_native);
        const int duration_enabled = !duration_parent_ok ? -1 : (is_disabled(duration_parent.fState) ? 0 : 1);
        try {
            log(std::format("menu_native toggle_checked={},display_checked={},display_enabled={},"
                            "duration_enabled={},threshold_enabled={}",
                            toggle_checked, display_checked, display_enabled, duration_enabled,
                            threshold_enabled));
        } catch (...) {
            log("menu_native state unavailable");
        }
        // Observable summary so external verification can compare the menu
        // marks with the model state without reading internal fields.
        try {
            log(std::format("menu state checked={} display_checked={} display_enabled={} threshold={} "
                            "duration={} duration_enabled={}",
                            model_.running() ? 1 : 0, model_.display_selected() ? 1 : 0,
                            model_.running() ? 1 : 0, model_.threshold(), duration_name(model_.duration()),
                            model_.running() ? 1 : 0));
        } catch (...) {
            log("menu state unavailable");
        }
    }

    // Owns the loaded menu handle for exactly one popup session, so the HMENU
    // is destroyed on every exit path (normal return, an exception escaping
    // the popup loop, or window teardown). The application does not own the
    // handle, so no double destroy is possible.
    class MenuGuard {
    public:
        explicit MenuGuard(HMENU menu) noexcept : menu_(menu) {}
        MenuGuard(const MenuGuard&) = delete;
        MenuGuard& operator=(const MenuGuard&) = delete;
        ~MenuGuard() {
            if (menu_) DestroyMenu(menu_);
        }
        [[nodiscard]] HMENU menu() const noexcept { return menu_; }

    private:
        HMENU menu_{};
    };

    // Opens the single native popup. State is written before the menu shows
    // and rewritten by WM_ENTERMENULOOP if the session changed meanwhile, so a
    // stop/expiry that happens while the menu is open cannot leave stale
    // check marks or an enabled duration submenu on screen.
    void show_menu() {
        MenuGuard guard(LoadMenuW(instance_, MAKEINTRESOURCEW(menu_resource)));
        if (!guard.menu()) {
            log("menu load failed");
            return;
        }
        HMENU popup = GetSubMenu(guard.menu(), 0);
        if (!popup) {
            log("menu popup missing");
            return;
        }
        // Recorded before TrackPopupMenu so WM_ENTERMENULOOP/WM_COMMAND
        // handling can reach the same popup while it is on screen.
        open_popup_ = popup;
        apply_menu_state(popup);
        POINT point{};
        GetCursorPos(&point);
        SetForegroundWindow(window_);
        const UINT selected = TrackPopupMenu(popup,
                                             TPM_LEFTALIGN | TPM_RIGHTBUTTON | TPM_RETURNCMD | TPM_NONOTIFY,
                                             point.x, point.y, 0, window_, nullptr);
        open_popup_ = nullptr;
        PostMessageW(window_, WM_NULL, 0, 0);
        // The guard destroys the menu exactly once here; a WM_CLOSE during the
        // popup only clears open_popup_ in WM_DESTROY, leaving ownership with
        // this guard so no handle is leaked or double freed.
        if (selected != 0) command(selected);
    }

    void command(UINT id) {
        if (is_duration_command(id)) {
            select_duration(duration_for_command(id));
            return;
        }
        switch (id) {
        case toggle_command:
            // Decide the user's intent before touching battery state: a stop
            // must never turn into a restart as a side effect of sampling.
            if (model_.running()) {
                // Stopping needs no fresh sample; the stop intent is fixed here.
                // The model applies an already-reached deadline inside the stop,
                // so this reports expired when due and manual otherwise.
                model_.stop(StopReason::manual);
                log(std::format("user stopped keep-awake reason={}", reason_name(model_.stop_reason())));
            } else {
                // Manual start samples immediately in this same event instead
                // of relying on the cached periodic sample (up to 1 s old).
                model_.observe(sample_battery());
                if (model_.start()) {
                    log("user started keep-awake");
                } else {
                    log(std::format("user start refused reason={}", reason_name(model_.stop_reason())));
                }
            }
            after_change();
            break;
        case threshold_commands[0]:
        case threshold_commands[1]:
        case threshold_commands[2]:
            select_threshold(threshold_for_command(id));
            break;
        case display_command:
            toggle_display();
            break;
        case exit_command:
            PostMessageW(window_, WM_CLOSE, 0, 0);
            break;
        default:
            break;
        }
    }

    // Screen keep-awake is a this-session option and never persisted. The
    // model records the user's intent; request success or failure is reported
    // separately by the adapter through the debug trace and tooltip.
    void toggle_display() {
        switch (model_.toggle_display()) {
        case DisplayToggle::enabled:
            log("user enabled screen keep-awake for this session; requesting display");
            break;
        case DisplayToggle::deferred_locked:
            log("user enabled screen keep-awake; deferred until the session is known unlocked");
            break;
        case DisplayToggle::disabled:
            log("user disabled screen keep-awake; requesting display release");
            break;
        case DisplayToggle::rejected_stopped:
            log("screen keep-awake toggle ignored while stopped");
            break;
        }
        after_change();
    }

    // The duration selection is a this-session option: it restarts the
    // countdown from the current monotonic moment, is only selectable while
    // running, and is never written to the settings file.
    void select_duration(Duration duration) {
        switch (model_.set_duration(duration)) {
        case DurationChange::updated:
            log(std::format("user selected duration={} remaining={}", duration_name(duration),
                            model_.remaining_time() ? model_.remaining_time()->count() : -1));
            break;
        case DurationChange::unchanged:
            log(std::format("duration {} already selected", duration_name(duration)));
            break;
        case DurationChange::rejected_stopped:
            log("duration change ignored while stopped");
            break;
        case DurationChange::rejected:
            log("duration change rejected");
            break;
        }
        after_change();
    }

    // A threshold selection samples the battery in this same event so a new
    // higher threshold can stop a running session immediately. Only a real
    // user change writes the settings file; selecting the current value is a
    // no-op and load/defaults never write.
    void select_threshold(int percent) {
        if (percent == 0) return;
        if (model_.threshold() == percent) {
            log(std::format("threshold {} already selected; not saved", percent));
            after_change();
            return;
        }
        // Sample in this same event; the follow-up gate uses this fresh data.
        const BatterySample sample = sample_battery();
        model_.observe(sample);
        log(std::format("threshold {} sampled ac={} presence={} percent={} read_failed={}", percent,
                        ac_name(sample.ac), presence_name(sample.presence),
                        sample.percent ? *sample.percent : -1, sample.read_failed ? 1 : 0));
        const auto change = model_.set_threshold(percent);
        if (change == ThresholdChange::rejected || change == ThresholdChange::unchanged) {
            log(std::format("threshold {} refused; keeping {}", percent, model_.threshold()));
            after_change();
            return;
        }
        const auto saved = save_threshold(settings_file_, percent);
        model_.set_threshold_persisted(saved == ThresholdSaveStatus::saved);
        log(std::format("settings save status={} threshold={}", save_status_name(saved), percent));
        if (saved != ThresholdSaveStatus::saved) {
            // Keep the in-memory selection; the tooltip reports it as unsaved.
            log("settings write failed; keeping the in-memory threshold and reporting unsaved");
        }
        if (change == ThresholdChange::stopped) {
            log(std::format("threshold change stopped the session reason={}",
                            reason_name(model_.stop_reason())));
        }
        after_change();
    }

    HINSTANCE instance_{};
    HWND window_{};
    Model model_{};
    std::filesystem::path settings_file_{};
    bool tray_added_{};
    bool system_request_held_{};
    bool display_request_held_{};
    bool session_notifications_registered_{};
    // Owned by this application: the HPOWERNOTIFY returned by
    // RegisterSuspendResumeNotification, or null. `suspend_registered_` tracks
    // that a registration was attempted so a repeated call is a no-op rather
    // than a second subscription.
    HPOWERNOTIFY suspend_handle_{};
    bool suspend_registered_{};
    unsigned long suspend_registration_error_{};
    // Whether RegisterSuspendResumeNotification really returned a handle. Passed
    // to compose_tooltip() so the tray states the limitation while running.
    bool suspend_subscribed_{};
    bool failed_{};
    std::wstring last_tooltip_;
    std::string last_icon_line_;
    std::string last_state_line_;
    // Non-owning: set only while TrackPopupMenu runs; MenuGuard frees the menu.
    HMENU open_popup_{};
    const UINT taskbar_created_{RegisterWindowMessageW(L"TaskbarCreated")};
};

} // namespace

int WINAPI wWinMain(_In_ HINSTANCE instance, _In_opt_ HINSTANCE, _In_ PWSTR, _In_ int) {
    const HANDLE mutex = CreateMutexW(nullptr, FALSE, instance_mutex);
    if (!mutex) {
        log(std::format("fatal: single-instance mutex failed ({})", GetLastError()));
        return 1;
    }
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        CloseHandle(mutex);
        log("another instance is running; exiting without changing it");
        return 0;
    }
    int code = 1;
    try {
        Application app(instance);
        code = app.run();
    } catch (const std::exception& error) {
        log(error.what());
    } catch (...) {
        log("fatal: unexpected error");
    }
    CloseHandle(mutex);
    return code;
}
