#include "../WinMouseFix.Host/FullscreenMonitor.h"
#include <iostream>
#include <vector>
namespace {
using namespace wmf::host;
int checks{},failures{};
#define VERIFY(...) do { ++checks; if (!(__VA_ARGS__)) { ++failures; std::cerr << __FILE__ << ':' << __LINE__ << ": " << #__VA_ARGS__ << '\n'; } } while(false)
void Classification() {
    FullscreenWindow window; window.visible=true; window.client=window.frame=window.monitor={0,0,2560,1440};
    VERIFY(IsFullscreenWindow(window));
    window.monitor=window.client=window.frame={-3840,-2160,0,0}; VERIFY(IsFullscreenWindow(window));
    window.client.left+=2; VERIFY(IsFullscreenWindow(window)); window.client.left+=1; VERIFY(!IsFullscreenWindow(window));
    window.client=window.monitor; window.client.bottom-=48; VERIFY(!IsFullscreenWindow(window)); // Work area is not the monitor.
    window.client=window.monitor; window.style=WS_OVERLAPPEDWINDOW; window.maximized=true; VERIFY(!IsFullscreenWindow(window));
    window.style=WS_POPUP; VERIFY(IsFullscreenWindow(window)); // Some borderless games use a maximized state.
    for (unsigned excluded=0;excluded<4;++excluded) {
        auto copy=window; if (excluded==0) copy.visible=false; if (excluded==1) copy.minimized=true;
        if (excluded==2) copy.cloaked=true; if (excluded==3) copy.shell=true;
        VERIFY(!IsFullscreenWindow(copy));
    }
    window.maximized=false; window.client.bottom-=10; window.exclusive=true; VERIFY(IsFullscreenWindow(window));
    window.frame.right-=100; VERIFY(!IsFullscreenWindow(window)); // A system-wide D3D signal cannot suspend an unrelated window.
    window.monitor={}; VERIFY(!IsFullscreenWindow(window));
}
void Stability() {
    FullscreenPause state;
    VERIFY(!state.Update(false,{true,true},1));
    VERIFY(state.Update(true,{true,true},2));
    VERIFY(state.Update(true,{true,false},10));
    VERIFY(state.Update(true,{true,false},409));
    VERIFY(!state.Update(true,{true,false},410));
    VERIFY(state.Update(true,{true,true},420));
    VERIFY(state.Update(true,{true,false},430));
    VERIFY(state.Update(true,{true,true},500)); // Fullscreen returns before exit settles.
    VERIFY(state.Update(true,{true,false},510));
    VERIFY(state.Update(true,{true,false},909));
    VERIFY(!state.Update(true,{true,false},910));
    VERIFY(state.Update(true,{true,true},920));
    VERIFY(state.Update(true,{},930));
    VERIFY(!state.Update(true,{},1330)); // Queries failing repeatedly never cause permanent suspension.
    VERIFY(state.Update(true,{true,true},1340));
    VERIFY(!state.Update(false,{true,true},1341)); // Disabling the preference takes effect immediately.
    VERIFY(!state.Update(true,{true,false},1342));
}
void NativeWindows() {
    const auto oldDpi=SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2); VERIFY(oldDpi!=nullptr);
    WNDCLASSW cls{}; cls.lpfnWndProc=DefWindowProcW; cls.hInstance=GetModuleHandleW(nullptr); cls.lpszClassName=L"WinMouseFix.FullscreenFixture";
    VERIFY(RegisterClassW(&cls)!=0 || GetLastError()==ERROR_CLASS_ALREADY_EXISTS);
    std::vector<RECT> displays;
    VERIFY(EnumDisplayMonitors(nullptr,nullptr,[](HMONITOR display,HDC,LPRECT,LPARAM p)->BOOL {
        MONITORINFO info{sizeof(info)}; if (GetMonitorInfoW(display,&info)) reinterpret_cast<std::vector<RECT>*>(p)->push_back(info.rcMonitor); return TRUE;
    },reinterpret_cast<LPARAM>(&displays)));
    VERIFY(!displays.empty());
    for (const auto& bounds : displays) {
        const auto window=CreateWindowExW(WS_EX_NOACTIVATE|WS_EX_TOOLWINDOW,cls.lpszClassName,L"Isolated fullscreen detection fixture",WS_POPUP,
            bounds.left,bounds.top,bounds.right-bounds.left,bounds.bottom-bounds.top,nullptr,nullptr,cls.hInstance,nullptr);
        VERIFY(window!=nullptr); if (!window) continue;
        auto reading=ReadFullscreenWindow(window); VERIFY(reading.known && !reading.fullscreen);
        // Keep fixtures at the bottom of the Z order; do not take focus or send input.
        VERIFY(SetWindowPos(window,HWND_BOTTOM,bounds.left,bounds.top,bounds.right-bounds.left,bounds.bottom-bounds.top,SWP_NOACTIVATE|SWP_SHOWWINDOW));
        reading=ReadFullscreenWindow(window); VERIFY(reading.known && reading.fullscreen);
        VERIFY(SetWindowPos(window,HWND_BOTTOM,bounds.left+50,bounds.top+50,640,480,SWP_NOACTIVATE));
        reading=ReadFullscreenWindow(window); VERIFY(reading.known && !reading.fullscreen);
        SetWindowLongPtrW(window,GWL_STYLE,WS_OVERLAPPEDWINDOW|WS_VISIBLE);
        VERIFY(SetWindowPos(window,HWND_BOTTOM,bounds.left,bounds.top,bounds.right-bounds.left,bounds.bottom-bounds.top,SWP_NOACTIVATE|SWP_FRAMECHANGED));
        reading=ReadFullscreenWindow(window); VERIFY(reading.known && !reading.fullscreen);
        VERIFY(DestroyWindow(window)); VERIFY(!ReadFullscreenWindow(window).known);
    }
    const auto receiver=CreateWindowExW(0,cls.lpszClassName,L"Monitor wakeup fixture",0,0,0,0,0,HWND_MESSAGE,nullptr,cls.hInstance,nullptr);
    VERIFY(receiver!=nullptr);
    { FullscreenMonitor monitor; VERIFY(monitor.Start(receiver,WM_APP+80)); monitor.Acknowledge(); monitor.Stop(); monitor.Stop(); }
    DestroyWindow(receiver); UnregisterClassW(cls.lpszClassName,cls.hInstance);
    if (oldDpi) SetThreadDpiAwarenessContext(oldDpi);
}
}
int RunFullscreenTests(int& total) { Classification(); Stability(); NativeWindows(); total+=checks; return failures; }
