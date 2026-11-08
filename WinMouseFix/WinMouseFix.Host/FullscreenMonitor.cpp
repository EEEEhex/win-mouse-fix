#include "FullscreenMonitor.h"
#include <dwmapi.h>
#include <shellapi.h>
#include <cwchar>
#include <iterator>

namespace wmf::host {
namespace {
bool SameBounds(const RECT& a,const RECT& b) noexcept {
    const auto closeEnough=[](LONG x,LONG y) { const auto delta=static_cast<std::int64_t>(x)-y; return delta>=-2 && delta<=2; };
    return b.right>b.left && b.bottom>b.top && closeEnough(a.left,b.left) && closeEnough(a.top,b.top) && closeEnough(a.right,b.right) && closeEnough(a.bottom,b.bottom);
}
bool ShellWindow(HWND window) noexcept {
    if (window==GetDesktopWindow() || window==GetShellWindow()) return true;
    wchar_t name[128]{}; GetClassNameW(window,name,static_cast<int>(std::size(name)));
    for (const auto shellClass : {L"Progman",L"WorkerW",L"Shell_TrayWnd",L"Shell_SecondaryTrayWnd",L"MultitaskingViewFrame"})
        if (!wcscmp(name,shellClass)) return true;
    DWORD pid{},shellPid{}; GetWindowThreadProcessId(window,&pid); GetWindowThreadProcessId(GetShellWindow(),&shellPid);
    if (pid && pid==shellPid && wcscmp(name,L"CabinetWClass") && wcscmp(name,L"ExploreWClass")) return true;
    const auto process=OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,FALSE,pid);
    if (!process) return false;
    wchar_t image[32768]{}; DWORD length=static_cast<DWORD>(std::size(image));
    const bool read=QueryFullProcessImageNameW(process,0,image,&length)!=FALSE; CloseHandle(process);
    if (!read) return false;
    const auto separator=wcsrchr(image,L'\\'); const auto filename=separator ? separator+1 : image;
    for (const auto shellProcess : {L"ShellExperienceHost.exe",L"StartMenuExperienceHost.exe",L"SearchHost.exe",L"SearchApp.exe"})
        if (!_wcsicmp(filename,shellProcess)) return true;
    return false;
}
}
bool IsFullscreenWindow(const FullscreenWindow& window) noexcept {
    if (!window.visible || window.minimized || window.cloaked || window.shell) return false;
    // Maximized custom-titlebar windows can have client bounds almost as large
    // as the monitor when the taskbar auto-hides. They remain normal windows.
    if (window.maximized && (window.style & WS_CAPTION)==WS_CAPTION) return false;
    return SameBounds(window.client,window.monitor) ||
        (window.exclusive && !(window.style & WS_CAPTION) && SameBounds(window.frame,window.monitor));
}
FullscreenReading ReadFullscreenWindow(HWND specified) noexcept {
    const bool foreground=specified==nullptr;
    auto window=specified ? specified : GetForegroundWindow();
    if (!window) return {};
    window=GetAncestor(window,GA_ROOT);
    if (!window || !IsWindow(window)) return {};
    FullscreenWindow state;
    state.visible=IsWindowVisible(window)!=FALSE; state.minimized=IsIconic(window)!=FALSE;
    state.shell=ShellWindow(window);
    if (!state.visible || state.minimized || state.shell) return {true,false};
    DWORD cloaked{}; if (SUCCEEDED(DwmGetWindowAttribute(window,DWMWA_CLOAKED,&cloaked,sizeof(cloaked)))) state.cloaked=cloaked!=0;
    if (state.cloaked) return {true,false};
    const auto oldDpi=SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    if (!oldDpi) return {};
    struct RestoreDpi { DPI_AWARENESS_CONTEXT value; ~RestoreDpi() { SetThreadDpiAwarenessContext(value); } } restore{oldDpi};
    MONITORINFO monitor{sizeof(monitor)}; const auto display=MonitorFromWindow(window,MONITOR_DEFAULTTONULL);
    if (!display || !GetMonitorInfoW(display,&monitor) || !GetClientRect(window,&state.client)) return {};
    SetLastError(ERROR_SUCCESS);
    if (!MapWindowPoints(window,nullptr,reinterpret_cast<POINT*>(&state.client),2) && GetLastError()!=ERROR_SUCCESS) return {};
    if (FAILED(DwmGetWindowAttribute(window,DWMWA_EXTENDED_FRAME_BOUNDS,&state.frame,sizeof(state.frame))) && !GetWindowRect(window,&state.frame)) return {};
    state.monitor=monitor.rcMonitor; state.style=GetWindowLongPtrW(window,GWL_STYLE); state.maximized=IsZoomed(window)!=FALSE;
    QUERY_USER_NOTIFICATION_STATE notifications{};
    state.exclusive=SUCCEEDED(SHQueryUserNotificationState(&notifications)) && notifications==QUNS_RUNNING_D3D_FULL_SCREEN;
    if (!IsWindow(window) || (foreground && GetAncestor(GetForegroundWindow(),GA_ROOT)!=window)) return {};
    return {true,IsFullscreenWindow(state)};
}
bool FullscreenPause::Update(bool enabled,FullscreenReading reading,std::uint64_t now) noexcept {
    if (!enabled) { paused_=leaving_=false; return false; }
    if (reading.known && reading.fullscreen) { paused_=true; leaving_=false; }
    else if (paused_) {
        if (!leaving_) { leaving_=true; leavingSince_=now; }
        // Unknown samples are bounded too: a failed query cannot suspend forever.
        if (now-leavingSince_>=400) { paused_=false; leaving_=false; }
    }
    return paused_;
}
thread_local FullscreenMonitor* FullscreenMonitor::current_{};
FullscreenMonitor::~FullscreenMonitor() { Stop(); }
bool FullscreenMonitor::Start(HWND window,UINT message) noexcept {
    if (current_ && current_!=this) return false;
    Stop(); window_=window; message_=message; current_=this;
    foreground_=SetWinEventHook(EVENT_SYSTEM_FOREGROUND,EVENT_SYSTEM_FOREGROUND,nullptr,&Event,0,0,WINEVENT_OUTOFCONTEXT);
    location_=SetWinEventHook(EVENT_OBJECT_LOCATIONCHANGE,EVENT_OBJECT_LOCATIONCHANGE,nullptr,&Event,0,0,WINEVENT_OUTOFCONTEXT);
    return foreground_ && location_;
}
void FullscreenMonitor::Stop() noexcept {
    if (foreground_) UnhookWinEvent(foreground_); if (location_) UnhookWinEvent(location_);
    foreground_=location_=nullptr; window_=nullptr; pending_=false; if (current_==this) current_=nullptr;
}
void CALLBACK FullscreenMonitor::Event(HWINEVENTHOOK,DWORD event,HWND window,LONG object,LONG child,DWORD,DWORD) noexcept {
    auto* monitor=current_; if (!monitor || !monitor->window_ || monitor->pending_) return;
    if (event==EVENT_OBJECT_LOCATIONCHANGE && (object!=OBJID_WINDOW || child!=CHILDID_SELF || window!=GetAncestor(GetForegroundWindow(),GA_ROOT))) return;
    monitor->pending_=PostMessageW(monitor->window_,monitor->message_,0,0)!=FALSE;
}
}
