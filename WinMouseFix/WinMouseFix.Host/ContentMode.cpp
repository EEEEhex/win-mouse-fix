#include "ContentMode.h"
#include "NativeModules.h"
#include "InputOwner.h"
#include "ContentControls.h"
#include <shellapi.h>
#include <wtsapi32.h>
#include <cmath>
#include <memory>
#include <stdexcept>

namespace wmf::host {
namespace {
double Number(const std::wstring& text) {
    std::size_t length{}; const double number = std::stod(text,&length);
    if (length != text.size() || !std::isfinite(number)) throw std::runtime_error("Invalid content-drag numeric argument");
    return number;
}
std::uint32_t SizeMm(const std::wstring& text) {
    const double value = Number(text);
    if (value < 40 || value > 1000) throw std::runtime_error("Device size must be 40..1000 mm");
    return static_cast<std::uint32_t>(std::llround(value*100));
}
constexpr wchar_t ContentClass[] = L"WinMouseFix.ContentDrag.v2";
constexpr int ToggleId = 101,ExitId = 102;
struct ContentPanel {
    ContentReport& report;
    ContentOptions options;
    std::unique_ptr<ContentRuntime> runtime;
    ContentControls controls;
    HWND status{},toggle{};
    bool closing{};
    void Refresh() {
        static constexpr const wchar_t* states[]{L"正在准备",L"已启用",L"已暂停",L"发生错误，已暂停",L"正在退出；若侧键仍按住，请松开",L"已停止"};
        if (!runtime) return;
        auto text = std::wstring(states[static_cast<unsigned>(runtime->State())]);
        const auto failure = runtime->Failure();
        if (!failure) text += L"\r\n记录的错误："+std::to_wstring(static_cast<unsigned>(failure.code))+L"，Win32="+
            std::to_wstring(failure.nativeError)+L"，cleanup="+std::to_wstring(failure.cleanupError);
        text += L"\r\n\r\n结果目录：\r\n"+report.Directory().wstring();
        SetWindowTextW(status,text.c_str());
        SetWindowTextW(toggle,runtime->Requested() ? L"暂停" : L"启用");
    }
    void Toggle() { if (runtime && !closing) runtime->Enable(!runtime->Requested()); Refresh(); }
};
LRESULT CALLBACK ContentWindow(HWND w,UINT m,WPARAM a,LPARAM b) {
    auto* panel = reinterpret_cast<ContentPanel*>(GetWindowLongPtrW(w,GWLP_USERDATA));
    if (m == WM_NCCREATE) {
        panel = static_cast<ContentPanel*>(reinterpret_cast<CREATESTRUCTW*>(b)->lpCreateParams);
        SetWindowLongPtrW(w,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(panel));
    }
    if (panel) {
        switch (m) {
        case WM_CREATE: {
            const auto font = reinterpret_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
            const auto instructions = panel->options.captureOnly ?
                L"采集联动检查：绑定侧键会被捕获，画面不滚动、单击不后退。\r\n按住侧键移动后释放，检查报告中的 rawMoves / blockedMoves。" :
                L"后退侧键：单击后退，按住并移动可二维拖动内容。\r\n在已激活的浏览器或文档内容上使用。";
            const auto label = CreateWindowW(L"STATIC",instructions,WS_CHILD|WS_VISIBLE,18,16,630,52,w,nullptr,nullptr,nullptr);
            SendMessageW(label,WM_SETFONT,reinterpret_cast<WPARAM>(font),TRUE);
            const auto hint = CreateWindowW(L"STATIC",L"使用按钮启用 / 暂停。关闭本窗口退出。",WS_CHILD|WS_VISIBLE,18,76,630,24,w,nullptr,nullptr,nullptr);
            SendMessageW(hint,WM_SETFONT,reinterpret_cast<WPARAM>(font),TRUE);
            panel->status = CreateWindowExW(WS_EX_CLIENTEDGE,L"EDIT",L"正在准备",WS_CHILD|WS_VISIBLE|ES_MULTILINE|ES_READONLY|ES_AUTOVSCROLL,18,112,630,165,w,nullptr,nullptr,nullptr);
            SendMessageW(panel->status,WM_SETFONT,reinterpret_cast<WPARAM>(font),TRUE);
            panel->toggle = CreateWindowW(L"BUTTON",L"暂停",WS_CHILD|WS_VISIBLE|WS_TABSTOP,18,290,120,32,w,reinterpret_cast<HMENU>(static_cast<INT_PTR>(ToggleId)),nullptr,nullptr);
            const auto exit = CreateWindowW(L"BUTTON",L"退出",WS_CHILD|WS_VISIBLE|WS_TABSTOP,150,290,120,32,w,reinterpret_cast<HMENU>(static_cast<INT_PTR>(ExitId)),nullptr,nullptr);
            SendMessageW(panel->toggle,WM_SETFONT,reinterpret_cast<WPARAM>(font),TRUE); SendMessageW(exit,WM_SETFONT,reinterpret_cast<WPARAM>(font),TRUE);
            return 0;
        }
        case WM_COMMAND:
            if (LOWORD(a) == ToggleId) panel->Toggle();
            if (LOWORD(a) == ExitId) PostMessageW(w,WM_CLOSE,0,0);
            return 0;
        case WM_WTSSESSION_CHANGE:
            if (a == WTS_SESSION_LOCK && panel->runtime) panel->runtime->Enable(false);
            return 0;
        case WM_TIMER:
            panel->Refresh();
            if (panel->closing && panel->runtime && panel->runtime->Finished()) DestroyWindow(w);
            return 0;
        case WM_CLOSE:
            if (!panel->runtime) { DestroyWindow(w); return 0; }
            panel->closing = true; EnableWindow(panel->toggle,FALSE); panel->runtime->Stop(); panel->Refresh();
            if (panel->runtime->Finished()) DestroyWindow(w);
            return 0;
        case WM_DESTROY:
            KillTimer(w,1); panel->controls.Release(); PostQuitMessage(0); return 0;
        }
    }
    return DefWindowProcW(w,m,a,b);
}
int RunContent(HINSTANCE instance,const ContentLaunch& launch) {
    InputOwner owner; const auto owned = owner.Acquire();
    if (!owned) throw std::runtime_error("Another input owner is active, or its lock is unavailable. Close the diagnostic/content-drag instance first.");
    NativeModules modules; std::wstring moduleError;
    if (!modules.Load(launch.modules,moduleError)) throw std::runtime_error(Utf8(moduleError));
    DragHandle validation{};
    const auto valid = WmfDragCreate(&launch.options.drag,&validation);
    if (!valid) throw std::runtime_error("Invalid drag configuration");
    WmfDragDestroy(&validation);
    GestureFrame origin; MappedFrame mapped;
    if (!WmfPlatformMap(&launch.options.device,&origin,&mapped)) throw std::runtime_error("Invalid synthetic device dimensions");
    ContentReport report(launch.output,launch.options);
    ContentPanel panel{report,launch.options};
    WNDCLASSEXW cls{sizeof(cls)}; cls.hInstance = instance; cls.lpszClassName = ContentClass;
    cls.lpfnWndProc = ContentWindow; cls.hCursor = LoadCursorW(nullptr,IDC_ARROW); cls.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW+1);
    if (!RegisterClassExW(&cls)) throw std::runtime_error("Cannot register content-drag window");
    const auto window = CreateWindowExW(0,ContentClass,L"WinMouseFix — Content Drag",WS_OVERLAPPEDWINDOW,CW_USEDEFAULT,CW_USEDEFAULT,690,390,nullptr,nullptr,instance,&panel);
    if (!window) throw std::runtime_error("Cannot create content-drag window");
    const auto controls = panel.controls.Acquire(window);
    if (!controls) { DestroyWindow(window); throw std::runtime_error("Cannot register session-lock notification; input capture was not enabled. Win32="+std::to_string(controls.nativeError)); }
    try { panel.runtime = std::make_unique<ContentRuntime>(report,launch.options); }
    catch (...) { DestroyWindow(window); throw; }
    if (!SetTimer(window,1,100,nullptr)) { panel.runtime->Stop(); panel.runtime.reset(); DestroyWindow(window); throw std::runtime_error("Cannot create content status timer"); }
    ShowWindow(window,SW_SHOW);
    MSG message{}; int status{};
    while ((status = GetMessageW(&message,nullptr,0,0)) > 0) {
        if (!IsDialogMessageW(window,&message)) { TranslateMessage(&message); DispatchMessageW(&message); }
    }
    panel.runtime.reset(); if (IsWindow(window)) DestroyWindow(window); report.Finish(); return status < 0 ? 1 : 0;
}
}
ContentLaunch ParseContentArguments(const std::vector<std::wstring>& arguments) {
    ContentLaunch launch;
    wchar_t executable[32768]{}; GetModuleFileNameW(nullptr,executable,static_cast<DWORD>(std::size(executable)));
    launch.modules = std::filesystem::path(executable).parent_path();
    launch.output = DefaultOutputRoot().parent_path()/L"content-drag-validation";
    LARGE_INTEGER frequency{}; QueryPerformanceFrequency(&frequency); launch.options.drag.qpcFrequency = static_cast<std::uint64_t>(frequency.QuadPart);
    bool content{};
    for (std::size_t i = 0; i < arguments.size(); ++i) {
        const auto& a = arguments[i];
        if (a == L"--content-drag") { content = true; continue; }
        if (a == L"--capture-only") { launch.options.captureOnly = true; continue; }
        if (i+1 >= arguments.size()) throw std::runtime_error("Missing content-drag option value");
        const auto& value = arguments[++i];
        if (a == L"--output") launch.output = value;
        else if (a == L"--module-dir") launch.modules = value;
        else if (a == L"--threshold-counts") launch.options.drag.thresholdCounts = Number(value);
        else if (a == L"--mm-per-count") launch.options.drag.mmPerCountX = launch.options.drag.mmPerCountY = Number(value);
        else if (a == L"--x-mm-per-count") launch.options.drag.mmPerCountX = Number(value);
        else if (a == L"--y-mm-per-count") launch.options.drag.mmPerCountY = Number(value);
        else if (a == L"--device-width-mm") launch.options.device.widthHimetric = SizeMm(value);
        else if (a == L"--device-height-mm") launch.options.device.heightHimetric = SizeMm(value);
        else throw std::runtime_error("Unknown or incompatible content-drag argument: "+Utf8(a));
    }
    if (!content) throw std::runtime_error("--content-drag is required");
    return launch;
}
bool TryRunContentMode(HINSTANCE instance,int& result) {
    int count{}; auto args = CommandLineToArgvW(GetCommandLineW(),&count);
    if (!args) throw std::runtime_error("Cannot read content-drag command line");
    std::vector<std::wstring> arguments;
    bool selected{};
    for (int i = 1; i < count; ++i) { arguments.emplace_back(args[i]); if (arguments.back() == L"--content-drag") selected = true; }
    LocalFree(args);
    if (!selected) return false;
    const auto launch = ParseContentArguments(arguments);
    result = RunContent(instance,launch); return true;
}
}
