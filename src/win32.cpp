#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <wtsapi32.h>
#include <pdh.h>
#include <pdhmsg.h>
#include "unsleep/core.hpp"
#include <algorithm>
#include <array>
#include <cwctype>
#include <format>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

namespace {
using namespace unsleep;
constexpr wchar_t instance_title[]=L"UnsleepMainInstance";
constexpr UINT tray_message=WM_APP+10;
constexpr UINT_PTR tick_timer=0x4c8;
constexpr UINT tray_id=129;
constexpr UINT active_command=32773, about_command=32774, exit_command=32771, dialog_command=32808, revert_command=32812;

[[noreturn]] void win_error(const char* action) { throw std::runtime_error(std::format("{} (Windows error {})",action,GetLastError())); }
template <typename Handle, auto Close>
class unique_resource {
public:
    explicit unique_resource(Handle handle=nullptr) noexcept : handle_(handle) {}
    ~unique_resource() { reset(); }
    unique_resource(const unique_resource&)=delete;
    unique_resource& operator=(const unique_resource&)=delete;
    unique_resource(unique_resource&& other) noexcept : handle_(std::exchange(other.handle_,nullptr)) {}
    unique_resource& operator=(unique_resource&& other) noexcept { if(this!=&other){reset();handle_=std::exchange(other.handle_,nullptr);}return *this; }
    void reset(Handle next=nullptr) noexcept { if(handle_) Close(handle_);handle_=next; }
    [[nodiscard]] Handle get() const noexcept { return handle_; }
private:
    Handle handle_{};
};
using owned_menu=unique_resource<HMENU,DestroyMenu>;
using owned_font=unique_resource<HFONT,DeleteObject>;
using owned_window=unique_resource<HWND,DestroyWindow>;
using owned_key=unique_resource<HKEY,RegCloseKey>;
using owned_brush=unique_resource<HBRUSH,DeleteObject>;
template<std::size_t N> void copy_text(wchar_t (&dest)[N], std::wstring_view text) noexcept {
    const auto count=std::min(text.size(),N-1);
    std::copy_n(text.data(),count,dest); dest[count]=L'\0';
}
int minute_now() noexcept { SYSTEMTIME time{};GetLocalTime(&time);return time.wHour*60+time.wMinute; }
void center(HWND window) {
    RECT area{}, rect{};
    if(!GetWindowRect(window,&rect) || !SystemParametersInfoW(SPI_GETWORKAREA,0,&area,0)) return;
    SetWindowPos(window,nullptr,area.left+(area.right-area.left-(rect.right-rect.left))/2,area.top+(area.bottom-area.top-(rect.bottom-rect.top))/2,0,0,SWP_NOSIZE|SWP_NOZORDER|SWP_NOACTIVATE);
}
bool dark_system_theme() noexcept {
    HKEY raw{};
    if(RegOpenKeyExW(HKEY_CURRENT_USER,L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",0,KEY_QUERY_VALUE,&raw)!=ERROR_SUCCESS) return false;
    owned_key key(raw);
    DWORD value{},size=sizeof(value),type{};
    return RegQueryValueExW(key.get(),L"SystemUsesLightTheme",nullptr,&type,reinterpret_cast<BYTE*>(&value),&size)==ERROR_SUCCESS && type==REG_DWORD && size==sizeof(value) && value==0;
}
bool legacy_keyup_host() noexcept {
    OSVERSIONINFOW version{};version.dwOSVersionInfoSize=sizeof(version);
#pragma warning(push)
#pragma warning(disable:4996)
#pragma warning(suppress:28159)
    const bool valid=GetVersionExW(&version)!=FALSE;
#pragma warning(pop)
    return valid && version.dwPlatformId==VER_PLATFORM_WIN32_NT && (version.dwMajorVersion==5 || version.dwMajorVersion==6);
}
class CpuCounter {
public:
    CpuCounter() {
        if(PdhOpenQueryW(nullptr,0,&query_)!=ERROR_SUCCESS) return;
        if(PdhAddCounterW(query_,L"\\Processor(_Total)\\% processor time",0,&counter_)!=ERROR_SUCCESS) { PdhCloseQuery(query_);query_=nullptr; }
    }
    ~CpuCounter(){if(query_)PdhCloseQuery(query_);}
    CpuCounter(const CpuCounter&)=delete;CpuCounter& operator=(const CpuCounter&)=delete;
    std::optional<int> sample() noexcept {
        if(!query_ || PdhCollectQueryData(query_)!=ERROR_SUCCESS) return {};
        PDH_FMT_COUNTERVALUE value{};
        if(PdhGetFormattedCounterValue(counter_,PDH_FMT_LONG,nullptr,&value)!=ERROR_SUCCESS || (value.CStatus!=PDH_CSTATUS_VALID_DATA && value.CStatus!=PDH_CSTATUS_NEW_DATA)) return {};
        return value.longValue;
    }
private: PDH_HQUERY query_{};PDH_HCOUNTER counter_{};
};

struct CommandHelp { const wchar_t* option;const wchar_t* description; };
constexpr std::array<CommandHelp,28> help{{
    CommandHelp{L"xx",L"Seconds between keys"}, {L"-startoff",L"Start inactive"},
    {L"-exitafter:X",L"Exit after X minutes"},{L"-activefor:X",L"Go inactive after X minutes"},
    {L"-inactivefor:X",L"Go active after X minutes"},{L"-appexit",L"Close current instance of app"},
    {L"-appon",L"Make current instance active"},{L"-appoff",L"Make current instance of app inactive"},
    {L"-apptoggle",L"Toggle state of current instance"},{L"-useshift",L"Simulate 'shift' key, not F15"},
    {L"-allowss",L"Prevent sleep but allow screensaver"},{L"-stes",L"Tell Windows to stay awake, don't use F15"},
    {L"-noicon",L"Do not show a tray icon"},{L"-oldicons",L"Use old (colourful) icons"},
    {L"-lighticons",L"Always use light tray icons"},{L"-darkicons",L"Always use dark tray icons"},
    {L"-replace",L"Replace running app with new one"},{L"-showdlg",L"Show a dialog while running"},
    {L"-ontaskbar",L"Show -showdlg dialog on the task bar"},{L"-apptoggleshowdlg",L"Toggle state and show dialog"},
    {L"-key:X",L"Simulate pressing virtual key X"},{L"-keypress",L"Perform a full keypress, not just key up"},
    {L"-cpu:X",L"Stay active until CPU falls under X"},{L"-notwhenlocked",L"Go inactive when the workstation is locked"},
    {L"-watchwindow:X",L"Only run when a window X is found"},{L"-activeperiods",L"Only run during periods (eg 0900-1729,1830-1900)"},
    {L"-onac",L"Only run when on AC power (not on battery)"},{L"-notify",L"Show a notification when going (in)active"}
}};

class Application {
    struct MenuItem {std::wstring label;bool separator{},submenu{};UINT id{};};
public:
    Application(HINSTANCE instance,Options options) : instance_(instance),model_(std::move(options)),legacy_keyup_(legacy_keyup_host()) {
        const auto dc=GetDC(nullptr);
        if(dc){menu_scale_=static_cast<double>(MulDiv(GetDeviceCaps(dc,LOGPIXELSX),100,96))/100.0;ReleaseDC(nullptr,dc);}
    }
    ~Application() {
        if(window_) { KillTimer(window_,tick_timer);WTSUnRegisterSessionNotification(window_); }
        remove_tray();
        tooltip_.reset();dialog_.reset();
        if(window_)DestroyWindow(window_);
    }
    Application(const Application&)=delete;Application& operator=(const Application&)=delete;
    int run() {
        WNDCLASSEXW wc{};wc.cbSize=sizeof(wc);wc.hInstance=instance_;wc.lpfnWndProc=window_proc;
        wc.lpszClassName=L"UnsleepMainWindow";wc.hCursor=LoadCursorW(nullptr,IDC_ARROW);
        wc.hIcon=LoadIconW(instance_,MAKEINTRESOURCEW(128));wc.style=CS_DBLCLKS;
        if(!RegisterClassExW(&wc))win_error("RegisterClassEx");
        window_=CreateWindowExW(0,wc.lpszClassName,instance_title,WS_OVERLAPPEDWINDOW,CW_USEDEFAULT,CW_USEDEFAULT,400,300,nullptr,nullptr,instance_,this);
        if(!window_)win_error("CreateWindowEx");
        dialog_.reset(CreateDialogParamW(instance_,MAKEINTRESOURCEW(139),model_.options().on_taskbar?nullptr:window_,dialog_proc,reinterpret_cast<LPARAM>(this)));
        if(!dialog_.get())win_error("Create status dialog");
        tooltip_.reset(CreateDialogParamW(instance_,MAKEINTRESOURCEW(146),window_,tooltip_proc,reinterpret_cast<LPARAM>(this)));
        if(!tooltip_.get())win_error("Create tooltip");
        center(dialog_.get());
        model_.refresh_schedule(minute_now());
        WTSRegisterSessionNotification(window_,NOTIFY_FOR_THIS_SESSION);
        add_tray();refresh_notifications();
        if(model_.options().show_dialog)show_dialog(true);
        if(!SetTimer(window_,tick_timer,1000,nullptr))win_error("SetTimer");
        MSG message{};int result{};
        while((result=GetMessageW(&message,nullptr,0,0))>0) {
            if(dialog_.get() && IsDialogMessageW(dialog_.get(),&message)) continue;
            TranslateMessage(&message);DispatchMessageW(&message);
        }
        if(result<0)win_error("GetMessage");
        return failed_?1:static_cast<int>(message.wParam);
    }
private:
    static Application* get(HWND window) noexcept { return reinterpret_cast<Application*>(GetWindowLongPtrW(window,GWLP_USERDATA)); }
    static LRESULT CALLBACK window_proc(HWND window,UINT message,WPARAM wparam,LPARAM lparam) noexcept {
        Application* self=get(window);
        if(message==WM_NCCREATE) { self=static_cast<Application*>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams);self->window_=window;SetWindowLongPtrW(window,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(self)); }
        if(!self)return DefWindowProcW(window,message,wparam,lparam);
        try{return self->dispatch(window,message,wparam,lparam);}catch(const std::exception& error){self->fail(error.what());return 0;}catch(...){self->fail("Unexpected application error");return 0;}
    }
    static INT_PTR CALLBACK dialog_proc(HWND window,UINT message,WPARAM wparam,LPARAM lparam) noexcept {
        if(message==WM_INITDIALOG){auto* self=reinterpret_cast<Application*>(lparam);SetWindowLongPtrW(window,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(self));SendMessageW(window,WM_SETICON,ICON_SMALL,reinterpret_cast<LPARAM>(LoadIconW(self->instance_,MAKEINTRESOURCEW(128))));return TRUE;}
        auto* self=get(window);if(!self)return FALSE;
        if(message==WM_CLOSE || (message==WM_COMMAND && (LOWORD(wparam)==IDCANCEL || LOWORD(wparam)==IDOK))){self->show_dialog(false);return TRUE;}
        return FALSE;
    }
    static INT_PTR CALLBACK about_proc(HWND window,UINT message,WPARAM wparam,LPARAM lparam) noexcept {
        try {
            if(message==WM_INITDIALOG){
                auto* self=reinterpret_cast<Application*>(lparam);
                const auto list=GetDlgItem(window,1006);
                ListView_SetExtendedListViewStyle(list,LVS_EX_FULLROWSELECT);
                const int dpi=static_cast<int>(GetDpiForWindow(window));
                LVCOLUMNW column{};column.mask=LVCF_WIDTH|LVCF_TEXT;column.pszText=const_cast<wchar_t*>(L"x");column.cx=MulDiv(120,dpi,96);
                ListView_InsertColumn(list,0,&column);column.cx=MulDiv(240,dpi,96);ListView_InsertColumn(list,1,&column);
                for(std::size_t i=0;i<help.size();++i){LVITEMW item{};item.mask=LVIF_TEXT;item.iItem=static_cast<int>(i);item.pszText=const_cast<wchar_t*>(help[i].option);const int row=ListView_InsertItem(list,&item);ListView_SetItemText(list,row,1,const_cast<wchar_t*>(help[i].description));}
                SetDlgItemTextW(window,1004,L"Unsleep\nWindows desktop keep-awake utility");
                const auto link=GetDlgItem(window,1003);SetWindowLongPtrW(link,GWL_STYLE,GetWindowLongPtrW(link,GWL_STYLE)|SS_NOTIFY);
                SendMessageW(window,WM_SETICON,ICON_SMALL,reinterpret_cast<LPARAM>(LoadIconW(self->instance_,MAKEINTRESOURCEW(128))));center(window);return TRUE;
            }
            if(message==WM_COMMAND && LOWORD(wparam)==1003 && HIWORD(wparam)==STN_CLICKED){ShellExecuteW(window,L"open",L"https://github.com/Coh1e/unsleep",nullptr,nullptr,SW_SHOWNORMAL);return TRUE;}
            if(message==WM_CLOSE || (message==WM_COMMAND && (LOWORD(wparam)==IDOK || LOWORD(wparam)==IDCANCEL))){EndDialog(window,IDOK);return TRUE;}
        } catch(...) { EndDialog(window,IDCANCEL); }
        return FALSE;
    }
    static INT_PTR CALLBACK tooltip_proc(HWND window,UINT message,WPARAM,LPARAM lparam) noexcept {
        if(message==WM_INITDIALOG){SetWindowLongPtrW(window,GWLP_USERDATA,static_cast<LONG_PTR>(lparam));return TRUE;}
        auto* self=get(window);if(!self)return FALSE;
        if(message==WM_PAINT){
            PAINTSTRUCT paint{};const auto dc=BeginPaint(window,&paint);if(!dc)return TRUE;
            RECT rect{};GetClientRect(window,&rect);FillRect(dc,&rect,static_cast<HBRUSH>(GetStockObject(WHITE_BRUSH)));FrameRect(dc,&rect,static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
            SetBkMode(dc,TRANSPARENT);SetTextColor(dc,RGB(0,0,0));InflateRect(&rect,-10,-5);
            owned_font heading(CreateFontW(-16,0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,DEFAULT_QUALITY,DEFAULT_PITCH,L"Calibri"));
            const auto previous=SelectObject(dc,heading.get()?heading.get():GetStockObject(DEFAULT_GUI_FONT));
            RECT title=rect;DrawTextW(dc,L" Unsleep",-1,&title,DT_SINGLELINE|DT_TOP);SelectObject(dc,previous);
            owned_font body(CreateFontW(-13,0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,DEFAULT_QUALITY,DEFAULT_PITCH,L"Calibri"));
            const auto old=SelectObject(dc,body.get()?body.get():GetStockObject(DEFAULT_GUI_FONT));rect.top+=24;rect.left+=5;SetTextColor(dc,RGB(80,80,80));
            DrawTextW(dc,self->status_.c_str(),static_cast<int>(self->status_.size()),&rect,DT_WORDBREAK|DT_NOPREFIX);SelectObject(dc,old);EndPaint(window,&paint);return TRUE;
        }
        return FALSE;
    }
    void fail(const char* error) noexcept { if(failed_)return;failed_=true;if(window_)KillTimer(window_,tick_timer);MessageBoxA(window_,error,"Unsleep error",MB_OK|MB_ICONERROR);PostQuitMessage(1); }
    LRESULT dispatch(HWND window,UINT message,WPARAM wparam,LPARAM lparam) {
        if(taskbar_created_ && message==taskbar_created_){add_tray();refresh();return 0;}
        switch(message){
        case WM_TIMER:if(wparam==tick_timer)tick();return 0;
        case WM_COMMAND:command(LOWORD(wparam));return 0;
        case WM_WTSSESSION_CHANGE:if(wparam==WTS_SESSION_LOCK)model_.set_locked(true);else if(wparam==WTS_SESSION_UNLOCK)model_.set_locked(false);return 0;
        case WM_SETTINGCHANGE:refresh();return 0;
        case WM_MEASUREITEM:return measure_menu(reinterpret_cast<MEASUREITEMSTRUCT*>(lparam));
        case WM_DRAWITEM:return draw_menu(reinterpret_cast<DRAWITEMSTRUCT*>(lparam));
        case WM_CLOSE:DestroyWindow(window);return 0;
        case WM_DESTROY:remove_tray();KillTimer(window,tick_timer);WTSUnRegisterSessionNotification(window);window_=nullptr;PostQuitMessage(0);return 0;
        case tray_message:tray_event(LOWORD(lparam));return 0;
        default:return DefWindowProcW(window,message,wparam,lparam);
        }
    }
    bool window_matches() const {
        auto needle=model_.options().watch_window;
        if(needle.empty())return true;
        for(HWND window=GetTopWindow(nullptr);window;window=GetWindow(window,GW_HWNDNEXT)){
            std::array<wchar_t,260> buffer{};GetWindowTextW(window,buffer.data(),static_cast<int>(buffer.size()));
            std::wstring title(buffer.data());std::ranges::transform(title,title.begin(),[](wchar_t ch){return static_cast<wchar_t>(std::towlower(ch));});
            if(title.find(needle)!=title.npos)return true;
        }
        return false;
    }
    void tick() {
        SYSTEM_POWER_STATUS power{};const bool ac=GetSystemPowerStatus(&power) && power.ACLineStatus==1;
        Inputs inputs{minute_now(),(GetAsyncKeyState(VK_SHIFT)&0x8000)!=0,ac,window_matches(),{}};
        if(model_.cpu_threshold()!=-1 && model_.active())inputs.cpu_percent=cpu_.sample();
        const auto effects=model_.tick(inputs);
        if(effects.keep_awake==KeepAwake::system){if(!SetThreadExecutionState(ES_CONTINUOUS|ES_SYSTEM_REQUIRED))OutputDebugStringW(L"Unsleep: execution-state request was not accepted");}
        else if(effects.keep_awake==KeepAwake::display){if(!SetThreadExecutionState(ES_CONTINUOUS|ES_DISPLAY_REQUIRED))OutputDebugStringW(L"Unsleep: execution-state request was not accepted");}
        else if(effects.keep_awake==KeepAwake::key){
            std::array<INPUT,2> input{};for(auto& event:input){event.type=INPUT_KEYBOARD;event.ki.wVk=effects.key;}
            input[1].ki.dwFlags=KEYEVENTF_KEYUP;
            const bool up_only=effects.key_up_only && legacy_keyup_;
            const UINT count=up_only?1U:2U;INPUT* first=up_only?&input[1]:input.data();
            const UINT sent=SendInput(count,first,sizeof(INPUT));
            if(sent!=count){if(sent==1 && count==2){INPUT release=input[1];SendInput(1,&release,sizeof(INPUT));}OutputDebugStringW(L"Unsleep: input injection was incomplete");}
        }
        for(const auto& text:effects.notifications)balloon(text);
        refresh();if(effects.exit)PostMessageW(window_,WM_CLOSE,0,0);
    }
    void show_dialog(bool show) noexcept {if(dialog_.get()){ShowWindow(dialog_.get(),show?SW_SHOW:SW_HIDE);dialog_visible_=show;}}
    void command(UINT id) {
        constexpr std::array<UINT,10> on_ids{32795,32796,32800,32778,32779,32782,32820,32783,32814,32784};
        constexpr std::array<UINT,10> off_ids{32804,32805,32806,32780,32781,32785,32819,32786,32818,32787};
        constexpr std::array<int,10> minutes{15,30,45,60,120,240,360,480,600,1440};
        for(std::size_t i=0;i<minutes.size();++i){if(id==on_ids[i]){model_.timed_active(minutes[i]);refresh_notifications();return;}if(id==off_ids[i]){model_.timed_inactive(minutes[i]);refresh_notifications();return;}}
        switch(id){
        case active_command:case 1033:model_.toggle();break;
        case 1125:model_.set_active(true);break;
        case 1032:model_.set_active(false);break;
        case 1034:model_.timed_active(30);break;
        case 1135:model_.toggle();if(dialog_.get())ShowWindow(dialog_.get(),SW_SHOW);center(dialog_.get());break;
        case revert_command:model_.revert(minute_now());break;
        case dialog_command:show_dialog(!dialog_visible_);break;
        case about_command:if(DialogBoxParamW(instance_,MAKEINTRESOURCEW(138),window_,about_proc,reinterpret_cast<LPARAM>(this))==-1)win_error("About dialog");break;
        case exit_command:PostMessageW(window_,WM_CLOSE,0,0);return;
        default:return;
        }
        refresh_notifications();
    }
    void refresh_notifications(){for(const auto& text:model_.take_notifications())balloon(text);refresh();}
    void add_tray() {
        if(model_.options().no_icon)return;
        tray_={};tray_.cbSize=sizeof(tray_);tray_.hWnd=window_;tray_.uID=tray_id;tray_.uFlags=NIF_MESSAGE|NIF_ICON|NIF_TIP;tray_.uCallbackMessage=tray_message;copy_text(tray_.szTip,L"Unsleep");tray_.hIcon=LoadIconW(instance_,MAKEINTRESOURCEW(128));
        tray_added_=Shell_NotifyIconW(NIM_ADD,&tray_)!=FALSE;
        if(!tray_added_)win_error("Shell_NotifyIcon add");
        tray_.uVersion=NOTIFYICON_VERSION_4;Shell_NotifyIconW(NIM_SETVERSION,&tray_);
    }
    void remove_tray() noexcept {if(tray_added_){Shell_NotifyIconW(NIM_DELETE,&tray_);tray_added_=false;}}
    void refresh() {
        status_=model_.status(minute_now());
        SetDlgItemTextW(dialog_.get(),1005,model_.active()?L"Unsleep is active":L"Unsleep is inactive");
        if(tooltip_.get())InvalidateRect(tooltip_.get(),nullptr,TRUE);
        if(tray_added_){
            const auto style=model_.options().icons;
            const bool light=style==Icons::light || (style==Icons::automatic && dark_system_theme());
            const WORD icon=style==Icons::old?(model_.active()?143:142):light?(model_.active()?140:130):(model_.active()?145:144);
            tray_.hIcon=LoadIconW(instance_,MAKEINTRESOURCEW(icon));tray_.uFlags=NIF_ICON;Shell_NotifyIconW(NIM_MODIFY,&tray_);
        }
    }
    void balloon(std::wstring_view text) noexcept {
        if(!tray_added_ || !model_.options().notify)return;
        auto data=tray_;data.uFlags=NIF_INFO;copy_text(data.szInfo,text);copy_text(data.szInfoTitle,L"Unsleep");data.dwInfoFlags=NIIF_NONE;data.uTimeout=10000;Shell_NotifyIconW(NIM_MODIFY,&data);
    }
    void tray_event(UINT event) {
        if(event==WM_LBUTTONDBLCLK) {command(active_command);return;}
        if(event==NIN_POPUPCLOSE){ShowWindow(tooltip_.get(),SW_HIDE);return;}
        if(event==NIN_POPUPOPEN){
            POINT cursor{};GetCursorPos(&cursor);const int dpi=static_cast<int>(GetDpiForWindow(window_));const int width=MulDiv(250,dpi,96),height=MulDiv(100,dpi,96);
            const int screen_width=GetSystemMetrics(SM_CXSCREEN),screen_height=GetSystemMetrics(SM_CYSCREEN);
            const int x=cursor.x+(cursor.x+width>screen_width?-width:0),y=cursor.y+(cursor.y>=screen_height/2?-height:1);
            SetWindowPos(tooltip_.get(),HWND_TOPMOST,x,y,width,height,SWP_NOACTIVATE|SWP_SHOWWINDOW);return;
        }
        if(event!=WM_RBUTTONUP)return;
        owned_menu menu(LoadMenuW(instance_,MAKEINTRESOURCEW(129)));if(!menu.get())win_error("LoadMenu");
        const auto popup=GetSubMenu(menu.get(),0);if(!popup)win_error("GetSubMenu");
        SetMenuDefaultItem(popup,active_command,FALSE);
        CheckMenuItem(popup,active_command,MF_BYCOMMAND|(model_.active()?MF_CHECKED:MF_UNCHECKED));
        CheckMenuItem(popup,dialog_command,MF_BYCOMMAND|(dialog_visible_?MF_CHECKED:MF_UNCHECKED));
        if(!model_.can_revert())RemoveMenu(popup,revert_command,MF_BYCOMMAND);
        menu_items_.clear();prepare_menu(popup);
        POINT point{};GetCursorPos(&point);SetForegroundWindow(window_);
        TrackPopupMenu(popup,TPM_LEFTALIGN,point.x,point.y,0,window_,nullptr);PostMessageW(window_,WM_NULL,0,0);
        menu.reset();menu_items_.clear();
    }
    void prepare_menu(HMENU menu) {
        for(int index=0;index<GetMenuItemCount(menu);++index){
            std::array<wchar_t,260> label{};
            MENUITEMINFOW info{};info.cbSize=sizeof(info);info.fMask=MIIM_FTYPE|MIIM_ID|MIIM_SUBMENU|MIIM_STRING;
            info.dwTypeData=label.data();info.cch=static_cast<UINT>(label.size());
            if(!GetMenuItemInfoW(menu,static_cast<UINT>(index),TRUE,&info))win_error("GetMenuItemInfo");
            auto item=std::make_unique<MenuItem>();item->label=label.data();item->separator=(info.fType&MFT_SEPARATOR)!=0;item->submenu=info.hSubMenu!=nullptr;item->id=info.wID;
            MENUITEMINFOW owner{};owner.cbSize=sizeof(owner);owner.fMask=MIIM_FTYPE|MIIM_DATA;owner.fType=info.fType|MFT_OWNERDRAW;owner.dwItemData=reinterpret_cast<ULONG_PTR>(item.get());
            if(!SetMenuItemInfoW(menu,static_cast<UINT>(index),TRUE,&owner))win_error("SetMenuItemInfo");
            menu_items_.push_back(std::move(item));
        }
    }
    const MenuItem* menu_item(ULONG_PTR pointer) const noexcept {
        for(const auto& item:menu_items_)if(reinterpret_cast<ULONG_PTR>(item.get())==pointer)return item.get();
        return nullptr;
    }
    LRESULT measure_menu(MEASUREITEMSTRUCT* measure) noexcept {
        if(!measure || measure->CtlType!=ODT_MENU)return FALSE;
        const auto* item=menu_item(measure->itemData);if(!item)return FALSE;
        if(item->separator){measure->itemWidth=static_cast<UINT>(menu_pixels(12));measure->itemHeight=static_cast<UINT>(menu_pixels(12));return TRUE;}
        const auto dc=GetDC(window_);if(!dc)return FALSE;
        owned_font font(CreateFontW(menu_pixels(-14),0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,DEFAULT_QUALITY,DEFAULT_PITCH,L"Calibri"));
        const auto old=SelectObject(dc,font.get()?font.get():GetStockObject(DEFAULT_GUI_FONT));
        SIZE extent{};GetTextExtentPoint32W(dc,item->label.c_str(),static_cast<int>(item->label.size()),&extent);
        SelectObject(dc,old);ReleaseDC(window_,dc);
        measure->itemWidth=static_cast<UINT>(extent.cx+menu_pixels(32)+menu_pixels(24)+menu_pixels(8)+menu_pixels(24));
        measure->itemHeight=static_cast<UINT>((std::max<LONG>(extent.cy+2,menu_pixels(30))+1)&~1L);return TRUE;
    }
    LRESULT draw_menu(DRAWITEMSTRUCT* draw) noexcept {
        if(!draw || draw->CtlType!=ODT_MENU)return FALSE;
        const auto* item=menu_item(draw->itemData);if(!item)return FALSE;
        const bool dark=dark_system_theme(),selected=(draw->itemState&ODS_SELECTED)!=0,disabled=(draw->itemState&(ODS_DISABLED|ODS_GRAYED))!=0;
        const bool destructive=item->id==exit_command;
        const COLORREF background=selected?(destructive?0x484ab9:dark?0x414141:0xe6e6e6):dark?0:0xffffff;
        const COLORREF foreground=disabled?(dark?0x616161:0xc8c8c8):(selected&&destructive)?(dark?0:0xffffff):dark?0xffffff:0;
        owned_brush brush(CreateSolidBrush(background));FillRect(draw->hDC,&draw->rcItem,brush.get()?brush.get():static_cast<HBRUSH>(GetStockObject(WHITE_BRUSH)));
        if(item->separator){RECT line=draw->rcItem;line.top=(line.top+line.bottom)/2;line.bottom=line.top;line.left+=menu_pixels(8);line.right-=menu_pixels(8);DrawEdge(draw->hDC,&line,EDGE_ETCHED,BF_TOP);return TRUE;}
        const int saved=SaveDC(draw->hDC);SetBkMode(draw->hDC,TRANSPARENT);SetTextColor(draw->hDC,foreground);
        owned_font font(CreateFontW(menu_pixels(-14),0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,DEFAULT_QUALITY,DEFAULT_PITCH,L"Calibri"));
        const auto previous=SelectObject(draw->hDC,font.get()?font.get():GetStockObject(DEFAULT_GUI_FONT));
        RECT text=draw->rcItem;text.left+=menu_pixels(32)+menu_pixels(8);text.right-=menu_pixels(24);
        UINT flags=DT_SINGLELINE|DT_VCENTER|DT_LEFT;if(draw->itemState&ODS_NOACCEL)flags|=DT_HIDEPREFIX;
        DrawTextW(draw->hDC,item->label.c_str(),static_cast<int>(item->label.size()),&text,flags);
        const auto symbol_height=static_cast<int>(static_cast<float>(menu_pixels(-14))*1.5F);
        if(draw->itemState&ODS_CHECKED){owned_font symbol(CreateFontW(symbol_height,0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,DEFAULT_QUALITY,DEFAULT_PITCH,L"Calibri"));const auto old=SelectObject(draw->hDC,symbol.get()?symbol.get():font.get());RECT check=draw->rcItem;check.right=check.left+menu_pixels(32);DrawTextW(draw->hDC,L"✔",1,&check,DT_SINGLELINE|DT_CENTER|DT_VCENTER);SelectObject(draw->hDC,old);}
        if(item->submenu){owned_font symbol(CreateFontW(symbol_height,0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,DEFAULT_QUALITY,DEFAULT_PITCH,L"Segoe UI Symbol"));const auto old=SelectObject(draw->hDC,symbol.get()?symbol.get():font.get());RECT arrow=draw->rcItem;arrow.left=arrow.right-menu_pixels(24);DrawTextW(draw->hDC,L"❯",1,&arrow,DT_SINGLELINE|DT_CENTER|DT_VCENTER);SelectObject(draw->hDC,old);}
        SelectObject(draw->hDC,previous);if(saved)RestoreDC(draw->hDC,saved);return TRUE;
    }
    HINSTANCE instance_{};Model model_;CpuCounter cpu_;
    int menu_pixels(int value) const noexcept {return static_cast<int>(static_cast<double>(value)*menu_scale_);}
    double menu_scale_{1.0};
    HWND window_{};owned_window dialog_,tooltip_;
    NOTIFYICONDATAW tray_{};
    std::wstring status_;
    std::vector<std::unique_ptr<MenuItem>> menu_items_;
    UINT taskbar_created_{RegisterWindowMessageW(L"TaskbarCreated")};
    bool legacy_keyup_{},tray_added_{},dialog_visible_{},failed_{};
};

int run_remote(const Options& options) {
    const HWND existing=FindWindowW(nullptr,instance_title);
    if(existing){
        if(options.remote==Remote::exit)SendMessageW(existing,WM_CLOSE,0,0);
        else {UINT id{};switch(options.remote){case Remote::on:id=1125;break;case Remote::off:id=1032;break;case Remote::toggle_show:id=1135;break;case Remote::toggle:id=1033;break;case Remote::active_thirty:id=1034;break;default:break;}if(id)SendMessageW(existing,WM_COMMAND,id,0);}
    }
    return 0;
}
}
int WINAPI wWinMain(_In_ HINSTANCE instance,_In_opt_ HINSTANCE,_In_ PWSTR command_line,_In_ int) {
    try {
        const auto options=unsleep::parse_options(command_line?command_line:L"");
        if(options.remote!=unsleep::Remote::none)return run_remote(options);
        if(options.replace){if(const auto old=FindWindowW(nullptr,instance_title))SendMessageW(old,WM_CLOSE,0,0);}
        INITCOMMONCONTROLSEX controls{sizeof(controls),ICC_LISTVIEW_CLASSES};if(!InitCommonControlsEx(&controls))win_error("InitCommonControlsEx");
        Application app(instance,options);return app.run();
    }catch(const std::exception& error){MessageBoxA(nullptr,error.what(),"Unsleep error",MB_OK|MB_ICONERROR);return 1;}
    catch(...){MessageBoxW(nullptr,L"Unexpected error",L"Unsleep error",MB_OK|MB_ICONERROR);return 1;}
}
