#include "ProductProbeMode.h"
#include "ProductSimulation.h"
#include "ProductLog.h"
#include "CapabilityEvidence.h"
#include "ContentControls.h"
#include "NativeModules.h"
#include "InputOwner.h"
#include "DiagnosticText.h"
#include "Report.h"
#include "JsonSupport.h"
#include "ProductConfig.h"
#include <shellapi.h>
#include <fstream>
namespace wmf::host {
namespace {
constexpr wchar_t ProbeClass[] = L"WinMouseFix.ProductValidation.v1";
struct Panel {
    bool zoom{},smoke{},closing{},started{},saved{};
    HWND window{},status{},expected{},visual{},notes{},save{},recordFailure{};
    ProductEnvironment environment;
    std::filesystem::path output;
    ProductSimulation simulation;
    std::unique_ptr<ProductRuntime> runtime;
    std::unique_ptr<ProductLog> log;
    std::unique_ptr<CapabilityEvidence> evidence;
    std::atomic<std::uint64_t> accepted{},frames{};
    ContentControls controls;
    std::uint64_t epoch{};
    std::wstring notice;
    std::string recordingError;
    BindingConfig experiment;
    TOUCHPAD_PARAMETERS_V1 touchpad{};
    bool settingsRead{};
    DWORD settingsError{};
    ~Panel() { if (runtime) runtime->Stop(); log.reset(); runtime.reset(); if (window && IsWindow(window)) DestroyWindow(window); }
    void Start() {
        if (started || closing) return;
        started = true; epoch = GetTickCount64(); log->DetailFor(); runtime->Enable(true);
    }
    void Stop() { if (started) runtime->Stop(); }
    void Save(bool passed = true) {
        if (smoke || !started || !runtime->Finished() || runtime->RequiresProcessExit() || saved) return;
        if (passed && !runtime->Failure()) return;
        if (runtime->LostTraces() || log->Error()) throw std::runtime_error("Diagnostic evidence is incomplete; repeat the validation");
        BOOL valid{}; const auto count = GetDlgItemInt(window,105,&valid,FALSE);
        wchar_t text[2049]{}; GetWindowTextW(notes,text,static_cast<int>(std::size(text)));
        const auto stats = runtime->Stats();
        if (passed) evidence->Confirm(zoom,valid ? count : 0,stats.rawWheels,stats.blockedWheels,accepted,frames,SendMessageW(visual,BM_GETCHECK,0,0) == BST_CHECKED,Utf8(text));
        else evidence->RecordFailure(zoom,stats.rawWheels,stats.blockedWheels,accepted,frames,Utf8(text),runtime->Failure());
        saved = passed;
        recordingError.clear();
        notice = passed ? L"已记录本机证据。退出此窗口后重新启动后台，才会开放对应能力。" : L"已记录未通过；没有开放缩放/增强。可以直接关闭窗口。";
    }
    void Tick() {
        if (!runtime) return;
        const auto stats = runtime->Stats();
        std::wstring text = !started ? L"尚未接管输入。请先聚焦目标应用，再按 Ctrl+Alt+F5。" : runtime->Finished() ? L"本次已停止，可填写观察并确认结果。" : runtime->Requested() ? L"实验中：Ctrl+Alt+F6 停止；也可选择停止并关闭。" : L"正在停止；请松开已按住的侧键。";
        text += L"\r\nRaw 滚轮包="+std::to_wstring(stats.rawWheels)+L"；截留 Hook="+std::to_wstring(stats.blockedWheels)+L"；接受="+std::to_wstring(accepted.load())+L"；自身/其他注入 Hook="+std::to_wstring(stats.injectedWheelHooks)+L"；原生帧="+std::to_wstring(frames.load());
        const auto result = runtime->Failure(); if (!result) text += L"\r\n故障="+std::to_wstring(static_cast<unsigned>(result.code))+L"，Win32="+std::to_wstring(result.nativeError)+L"，清理="+std::to_wstring(result.cleanupError);
        text += L"\r\n"+notice+L"\r\n输出："+output.wstring(); SetWindowTextW(status,text.c_str());
        EnableWindow(save,!smoke && started && runtime->Finished() && static_cast<bool>(result) && !saved);
        EnableWindow(recordFailure,!smoke && started && runtime->Finished() && !saved);
        if (smoke) {
            if (!started) { simulation.scripted = true; Start(); }
            if (simulation.completions) Stop();
            if (GetTickCount64()-epoch > 8000) { closing = true; runtime->Stop(); }
            if (runtime->Finished()) closing = true;
        }
        // A real validation has a short, explicit capture lease. A missed Stop
        // hotkey can never leave an experimental global wheel owner running.
        if (started && GetTickCount64()-epoch >= 60000) Stop();
        if (closing && runtime->Finished()) DestroyWindow(window);
    }
};
LRESULT CALLBACK Procedure(HWND window,UINT message,WPARAM a,LPARAM b) noexcept {
    auto* p = reinterpret_cast<Panel*>(GetWindowLongPtrW(window,GWLP_USERDATA));
    if (message == WM_NCCREATE) { p = static_cast<Panel*>(reinterpret_cast<CREATESTRUCTW*>(b)->lpCreateParams); p->window = window; SetWindowLongPtrW(window,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(p)); }
    if (p) try {
        if (message == WM_CREATE) {
            CreateWindowW(L"STATIC",p->zoom ? L"原生缩放验证：按住后退侧键，向上 5 格、向下 5 格，释放后 F6 停止。\r\n没有缩放请填写应用名并选择“记录未通过”，不要勾选有效。" : L"来源验证：在可滚动内容区向同一方向滚动 10 格，间隔约 0.5 秒。\r\n停止滚轮后观察尾段、等待 2 秒，再按 Ctrl+Alt+F6。",WS_CHILD|WS_VISIBLE,20,18,650,55,window,nullptr,nullptr,nullptr);
            p->status = CreateWindowW(L"STATIC",L"准备中",WS_CHILD|WS_VISIBLE,20,80,650,130,window,nullptr,nullptr,nullptr);
            CreateWindowW(L"STATIC",L"本次明确输入的物理包数（至少 10）：",WS_CHILD|WS_VISIBLE,20,225,320,24,window,nullptr,nullptr,nullptr);
            p->expected = CreateWindowExW(WS_EX_CLIENTEDGE,L"EDIT",L"10",WS_CHILD|WS_VISIBLE|WS_TABSTOP|ES_NUMBER,350,220,95,30,window,reinterpret_cast<HMENU>(105),nullptr,nullptr);
            p->visual = CreateWindowW(L"BUTTON",L"已看到目标正确滚动/缩放与反向；没有双重滚动或递归",WS_CHILD|WS_VISIBLE|WS_TABSTOP|BS_AUTOCHECKBOX,20,260,650,32,window,nullptr,nullptr,nullptr);
            CreateWindowW(L"STATIC",L"目标应用、版本及观察（必须填写）：",WS_CHILD|WS_VISIBLE,20,305,650,24,window,nullptr,nullptr,nullptr);
            p->notes = CreateWindowExW(WS_EX_CLIENTEDGE,L"EDIT",L"",WS_CHILD|WS_VISIBLE|WS_TABSTOP|ES_MULTILINE|ES_AUTOVSCROLL,20,333,650,75,window,nullptr,nullptr,nullptr);
            p->save = CreateWindowW(L"BUTTON",L"确认有效并记录通过",WS_CHILD|WS_VISIBLE|WS_TABSTOP,20,430,210,36,window,reinterpret_cast<HMENU>(106),nullptr,nullptr); EnableWindow(p->save,FALSE);
            p->recordFailure = CreateWindowW(L"BUTTON",L"记录未通过 / 无响应",WS_CHILD|WS_VISIBLE|WS_TABSTOP,240,430,220,36,window,reinterpret_cast<HMENU>(108),nullptr,nullptr); EnableWindow(p->recordFailure,FALSE);
            CreateWindowW(L"BUTTON",L"停止并关闭",WS_CHILD|WS_VISIBLE|WS_TABSTOP,470,430,170,36,window,reinterpret_cast<HMENU>(107),nullptr,nullptr);
            return 0;
        }
        if (message == WM_HOTKEY) { if (a == 51) p->Start(); else p->Stop(); return 0; }
        if (message == WM_WTSSESSION_CHANGE && a == WTS_SESSION_LOCK) { p->Stop(); return 0; }
        if (message == WM_COMMAND) { if (LOWORD(a) == 106) p->Save(); if (LOWORD(a) == 108) p->Save(false); if (LOWORD(a) == 107) PostMessageW(window,WM_CLOSE,0,0); return 0; }
        if (message == WM_TIMER) { p->Tick(); return 0; }
        if (message == WM_CLOSE) { p->closing = true; p->runtime->Stop(); return 0; }
        if (message == WM_DESTROY) { KillTimer(window,1); UnregisterHotKey(window,51); UnregisterHotKey(window,52); p->controls.Release(); PostQuitMessage(0); return 0; }
    } catch (const std::exception& error) { p->recordingError = error.what(); p->notice = std::wstring(L"未记录通过：")+winrt::to_hstring(error.what()).c_str(); }
    catch (...) { if (p && p->runtime) p->runtime->Stop(); }
    return DefWindowProcW(window,message,a,b);
}
}
bool TryRunProductProbe(HINSTANCE instance,int& result) {
    int count{}; auto args = CommandLineToArgvW(GetCommandLineW(),&count); if (!args) throw std::runtime_error("Cannot read probe arguments");
    std::vector<std::wstring> arguments; for (int i = 1; i < count; ++i) arguments.emplace_back(args[i]); LocalFree(args);
    bool selected{}; for (const auto& a : arguments) if (a == L"--validate-wheel-source" || a == L"--validate-native-zoom" || a == L"--product-probe-smoke") selected = true;
    if (!selected) return false;
    NativeModules modules; Panel p;
    for (std::size_t i = 0; i < arguments.size(); ++i) {
        const auto& a = arguments[i]; if (a == L"--validate-native-zoom") p.zoom = true;
        else if (a == L"--product-probe-smoke") p.smoke = true;
        else if (a == L"--validate-wheel-source") {}
        else if (a == L"--output" && i+1 < arguments.size()) p.output = std::filesystem::absolute(arguments[++i]);
        else throw std::runtime_error("Unknown validation argument");
    }
    if (p.smoke && p.output.empty()) throw std::runtime_error("Probe smoke needs an explicit isolated directory");
    p.environment = p.smoke ? ProductEnvironment::Simulation(p.output) : ProductEnvironment::Production();
    if (p.output.empty()) p.output = p.environment.dataDirectory/L"validation"/(std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(GetTickCount64()));
    InputOwner owner(p.environment.inputOwnerSuffix); if (!owner.Acquire()) throw std::runtime_error("请先从托盘退出后台及其他诊断窗口，再开始独立验证。");
    std::wstring error; if (!modules.Load(p.environment.executableDirectory,error)) throw std::runtime_error(Utf8(error));
    Capabilities caps; const auto capabilityResult = p.smoke ? p.simulation.RuntimeApi().engine.capabilities(&caps) : WmfPlatformCapabilities(&caps);
    if (!capabilityResult) throw std::runtime_error("Native capability query failed");
    p.evidence = std::make_unique<CapabilityEvidence>(p.environment,caps);
    p.touchpad.versionNumber = TOUCHPAD_PARAMETERS_VERSION_1;
    if (p.smoke) { p.settingsRead = true; p.touchpad.panEnabled = p.touchpad.zoomEnabled = TRUE; }
    else { p.settingsRead = SystemParametersInfoW(SPI_GETTOUCHPADPARAMETERS,sizeof(p.touchpad),&p.touchpad,0) != FALSE; if (!p.settingsRead) p.settingsError = GetLastError(); }
    // Pinch validates its paired source and target independently. A successful
    // pinch observation never grants ordinary wheel interception permission.
    auto config = DefaultBindingConfig(); LARGE_INTEGER frequency{}; QueryPerformanceFrequency(&frequency); config.qpcFrequency = frequency.QuadPart;
    config.wheelMode = p.zoom ? WheelMode::Off : WheelMode::Regular;
    if (p.zoom) { config.bindingCount = 2; config.bindings[1] = {2,Button::Back,Trigger::Wheel,0,Action::NativeZoom}; }
    p.experiment = config;
    auto api = p.smoke ? p.simulation.RuntimeApi() : ProductRuntimeApi{};
    api.engine.context = &p; api.engine.trace = [](void* context,const ProductTrace& trace) noexcept {
        auto& panel = *static_cast<Panel*>(context); if (trace.kind == ProductTraceKind::Input && trace.input.kind == InputKind::Wheel && trace.result) panel.accepted += trace.input.samples;
        if (trace.kind == ProductTraceKind::Frame && trace.result) ++panel.frames;
    };
    p.runtime = std::make_unique<ProductRuntime>(p.environment,config,false,api,ExplicitValidationProbe);
    p.log = std::make_unique<ProductLog>(p.output/L"logs",*p.runtime,config.qpcFrequency,p.smoke);
    WNDCLASSEXW cls{sizeof(cls)}; cls.hInstance = instance; cls.lpfnWndProc = Procedure; cls.lpszClassName = ProbeClass; cls.hCursor = LoadCursorW(nullptr,IDC_ARROW); cls.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW+1);
    if (!RegisterClassExW(&cls) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) throw std::runtime_error("Cannot register validation window");
    if (!CreateWindowExW(0,ProbeClass,p.zoom ? L"WinMouseFix 原生缩放验证" : L"WinMouseFix 滚轮来源验证",WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU,CW_USEDEFAULT,CW_USEDEFAULT,710,530,nullptr,nullptr,instance,&p)) throw std::runtime_error("Cannot create validation window");
    if (!p.smoke) {
        if (!p.controls.Acquire(p.window) || !RegisterHotKey(p.window,51,MOD_CONTROL|MOD_ALT|MOD_NOREPEAT,VK_F5) || !RegisterHotKey(p.window,52,MOD_CONTROL|MOD_ALT|MOD_NOREPEAT,VK_F6)) throw std::runtime_error("Cannot register validation hotkeys");
        ShowWindow(p.window,SW_SHOWNORMAL);
    }
    if (!SetTimer(p.window,1,100,nullptr)) throw std::runtime_error("Cannot create validation lease timer"); MSG message{};
    while (GetMessageW(&message,nullptr,0,0) > 0) { if (!IsDialogMessageW(p.window,&message)) { TranslateMessage(&message); DispatchMessageW(&message); } }
    if (p.runtime->RequiresProcessExit() && !p.smoke) { p.log.reset(); ExitProcess(ERROR_GEN_FAILURE); }
    const auto stats = p.runtime->Stats(); std::filesystem::create_directories(p.output);
    const bool ok = p.smoke && p.runtime->Finished() && p.simulation.completions && static_cast<bool>(p.runtime->Failure());
    std::ofstream(p.output/L"report.json") << "{\"simulation\":" << (p.smoke ? "true" : "false") << ",\"smokePassed\":" << (ok ? "true" : "false")
        << ",\"validated\":" << (p.saved ? "true" : "false") << ",\"rawWheels\":" << stats.rawWheels << ",\"blockedWheels\":" << stats.blockedWheels << ",\"accepted\":" << p.accepted << ",\"frames\":" << p.frames
        << ",\"recordingError\":" << Json(p.recordingError)
        << ",\"identity\":" << Json(p.evidence->Identity()) << ",\"lostTraceRecords\":" << p.runtime->LostTraces() << ",\"logError\":" << p.log->Error()
        << ",\"result\":" << JsonResult(p.runtime->Failure()) << ",\"zoom\":" << (p.zoom ? "true" : "false")
        << ",\"touchpadSettings\":{\"queryOk\":" << (p.settingsRead ? "true" : "false") << ",\"nativeError\":" << p.settingsError
        << ",\"panEnabled\":" << (p.settingsRead ? p.touchpad.panEnabled ? "true" : "false" : "null")
        << ",\"zoomEnabled\":" << (p.settingsRead ? p.touchpad.zoomEnabled ? "true" : "false" : "null") << '}'
        << ",\"configuration\":" << SerializeConfig(ProductConfig{p.experiment,false,false}) << '}';
    result = p.smoke && !ok ? 1 : 0; return true;
}
}
