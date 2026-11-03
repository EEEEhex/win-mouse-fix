#include <Windows.h>
#include <shellapi.h>
#include <wtsapi32.h>
#include "NativeApi.h"
#include "NativeModules.h"
#include "InputRuntime.h"
#include "DiagnosticText.h"
#include "DiagnosticHotkeys.h"
#include "ContentMode.h"
#include "ResidentMode.h"
#include "ProductProbeMode.h"
#include "InputOwner.h"
#include <cmath>
#include <algorithm>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using namespace wmf;
using namespace wmf::host;
constexpr UINT StartPreparedControlMessage = WM_APP + 1;
enum Id : int {
    Scenario = 100, Fingers, Mode, MotionProfile, Hz, Duration, ExtentX, ExtentY, Width, Height,
    Preparation, Confirm, Arm, Suspend, InertiaStop, RuntimeLabel, SummaryLabel,
    ObservationItem, ObservationResult, Observer, Evidence, Notes, ExperimentId, SaveObservation
};
struct Options {
    bool diagnostic{}, probe{}, probeDevice{}, smoke{}, simulation{};
    std::filesystem::path output{DefaultOutputRoot()}, modules;
};
std::wstring Text(HWND control) {
    const int size = GetWindowTextLengthW(control);
    std::wstring text(static_cast<std::size_t>(size) + 1, L'\0');
    GetWindowTextW(control, text.data(), size + 1);
    text.resize(static_cast<std::size_t>(size));
    return text;
}
std::wstring Wide(std::string_view text) {
    if (text.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring out(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), n);
    return out;
}
Options Parse() {
    Options o;
    wchar_t executable[32768]{};
    GetModuleFileNameW(nullptr, executable, static_cast<DWORD>(std::size(executable)));
    o.modules = std::filesystem::path(executable).parent_path();
    int count{};
    auto args = CommandLineToArgvW(GetCommandLineW(), &count);
    if (!args) throw std::runtime_error("Cannot read command line");
    std::vector<std::wstring> values;
    for (int i = 1; i < count; ++i) values.emplace_back(args[i]);
    LocalFree(args);
    for (std::size_t i = 0; i < values.size(); ++i) {
        const auto& a = values[i];
        if (a == L"--diagnostic") o.diagnostic = true;
        else if (a == L"--probe") o.probe = true;
        else if (a == L"--probe-device") { o.probe = true; o.probeDevice = true; }
        else if (a == L"--smoke") { o.probe = true; o.smoke = true; }
        else if (a == L"--simulation") o.simulation = true;
        else if ((a == L"--output" || a == L"--module-dir") && i + 1 < values.size()) {
            if (a == L"--output") o.output = values[++i]; else o.modules = values[++i];
        } else throw std::runtime_error("Unknown or incomplete argument: " + Utf8(a));
    }
    if (o.simulation && !o.probe) throw std::runtime_error("--simulation is only allowed for headless verification");
    return o;
}
double Number(HWND window, Id id) {
    const auto text = Text(GetDlgItem(window, id));
    std::size_t parsed{};
    const double value = std::stod(text, &parsed);
    if (parsed != text.size() || !std::isfinite(value)) throw std::runtime_error("Invalid numeric parameter");
    return value;
}
struct Panel {
    Options options;
    ProbeInfo probe;
    ReportWriter& report;
    std::unique_ptr<InputRuntime> runtime;
    HFONT font{};
    int scale{96};
    bool hotkeys{}, armed{};
    std::wstring hotkeyError, lastError;
    DiagnosticHotkeys registeredHotkeys;
    std::vector<Observation> baseline{BaselineObservations()};
    HWND window{};
    int scrollX{}, scrollY{};

    int Px(int value) const { return MulDiv(value, scale, 96); }
    HWND Add(const wchar_t* cls, const wchar_t* text, DWORD style, int x, int y, int w, int h, int id = 0) {
        auto control = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, Px(x), Px(y), Px(w), Px(h),
            window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), GetModuleHandleW(nullptr), nullptr);
        if (!control) throw std::runtime_error("Cannot create diagnostic control");
        SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
        return control;
    }
    void Combo(Id id, const std::vector<std::wstring>& entries, int x, int y, int w, int selected = 0) {
        auto combo = Add(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, x, y, w, 250, id);
        for (const auto& entry : entries) SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(entry.c_str()));
        SendMessageW(combo, CB_SETCURSEL, selected, 0);
    }
    void Edit(Id id, const wchar_t* value, int x, int y, int w) {
        Add(L"EDIT", value, WS_BORDER | ES_AUTOHSCROLL | WS_TABSTOP, x, y, w, 25, id);
    }
    void Label(const wchar_t* value, int x, int y, int w) { Add(L"STATIC", value, 0, x, y, w, 22); }
    int Selected(Id id) const { return static_cast<int>(SendDlgItemMessageW(window, id, CB_GETCURSEL, 0, 0)); }
    void Set(Id id, const std::wstring& value) { SetDlgItemTextW(window, id, value.c_str()); }
    void Defaults() {
        const int c = Selected(Scenario);
        SendDlgItemMessageW(window, Fingers, CB_SETCURSEL, c < 3 ? 0 : c < 5 ? 1 : 2, 0);
        SendDlgItemMessageW(window, MotionProfile, CB_SETCURSEL, c == 2 ? 4 : 0, 0);
        Set(Duration, c == 2 ? L"0.25" : L"2.0");
        Set(ExtentX, c == 1 || c == 5 ? L"20" : c == 6 ? L"-20" : L"0");
        Set(ExtentY, c == 4 ? L"20" : c == 1 || c >= 5 ? L"0" : L"-20");
        armed = false;
    }
    void Initialize(HWND h) {
        window = h;
        scale = static_cast<int>(GetDpiForWindow(h));
        RECT desired{0,0,Px(1080),Px(660)};
        AdjustWindowRectExForDpi(&desired,static_cast<DWORD>(GetWindowLongPtrW(h,GWL_STYLE)),FALSE,0,static_cast<UINT>(scale));
        MONITORINFO monitor{}; monitor.cbSize = sizeof(monitor);
        int width = desired.right-desired.left, height = desired.bottom-desired.top;
        if (GetMonitorInfoW(MonitorFromWindow(h,MONITOR_DEFAULTTONEAREST),&monitor)) {
            width = (std::min)(width,static_cast<int>(monitor.rcWork.right-monitor.rcWork.left));
            height = (std::min)(height,static_cast<int>(monitor.rcWork.bottom-monitor.rcWork.top));
        }
        SetWindowPos(h,nullptr,0,0,width,height,SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        font = CreateFontW(-MulDiv(10, scale, 72), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
        Label(L"原生触控板诊断：仅显式启动实验时提交接触。侧键产品功能尚未接入。", 18, 12, 1040);
        Label(L"场景", 18, 48, 45);
        std::vector<std::wstring> cases;
        for (const auto name : CaseNames) cases.push_back(Wide(name));
        Combo(Scenario, cases, 68, 43, 180);
        Label(L"指数量", 265, 48, 55); Combo(Fingers, {L"2",L"3",L"4"}, 325, 43, 65);
        Label(L"模式", 410, 48, 45); Combo(Mode, {L"固定轨迹",L"交互推进"}, 460, 43, 140);
        Label(L"轨迹", 620, 48, 45);
        Combo(MotionProfile, {L"线性",L"保持后返回",L"细小阶梯",L"慢停释放",L"快速释放",L"二维",L"起始后微移（诊断）"}, 670, 43, 170);
        Label(L"频率 Hz", 860, 48, 70); Combo(Hz, {L"60",L"120",L"240"}, 935, 43, 80, 1);
        Label(L"时长 s",18,89,60); Edit(Duration,L"2.0",80,84,75);
        Label(L"X 位移 mm",175,89,90); Edit(ExtentX,L"0",270,84,75);
        Label(L"Y 位移 mm",365,89,90); Edit(ExtentY,L"-20",460,84,75);
        Label(L"设备宽 mm",555,89,95); Edit(Width,L"100",655,84,75);
        Label(L"设备高 mm",750,89,95); Edit(Height,L"60",850,84,75);
        Label(L"准备记录：应用与版本、可滚动区域；Shell 场景填写实际手势映射、动画及桌面条件。",18,126,1040);
        Edit(Preparation,L"",18,153,1030);
        Add(L"BUTTON", L"已确认目标可用；Shell 映射/动画符合场景；桌面切换时已有至少两个桌面。",
            BS_AUTOCHECKBOX | WS_TABSTOP,18,187,1030,25,Confirm);
        Add(L"BUTTON",L"准备实验（不提交输入）",WS_TABSTOP,18,225,225,30,Arm);
        Add(L"BUTTON",L"暂停运行时 / 释放接触",WS_TABSTOP,260,225,225,30,Suspend);
        Add(L"BUTTON",L"原生停止惯性",WS_TABSTOP,502,225,180,30,InertiaStop);
        Label(L"准备后将光标移到目标并激活目标，再按 Ctrl+Alt+F5 开始；诊断面板不抢回前台。",18,268,1040);
        Label(L"Ctrl+Alt：F6 继续  |  F7 保持接触停住  |  F8 反向  |  F9 释放  |  F10 取消",18,296,1040);
        Label(L"F11 原生停止惯性；使用暂停按钮停止运行时。反向/保持控制用于交互模式，固定模式按预设轨迹运行。",18,324,1040);
        Add(L"STATIC",L"",0,18,356,1030,42,RuntimeLabel);
        Add(L"STATIC",L"",0,18,400,1030,55,SummaryLabel);
        Label(L"人工观察：接口成功不代表动画跟手。每项结果关联一次已完成且同场景的实验。",18,462,1040);
        std::vector<std::wstring> items;
        for (const auto& o : baseline) items.push_back(Wide(std::string(CaseNames[static_cast<unsigned>(o.scenario)]) + " / " + o.item));
        Combo(ObservationItem,items,18,490,420);
        Combo(ObservationResult,{L"not-run",L"pass",L"fail",L"unsupported",L"inconclusive"},455,490,155);
        Label(L"实验 ID",628,495,70); Edit(ExperimentId,L"0",700,490,75);
        Label(L"观察人",18,535,70); Edit(Observer,L"",90,530,200);
        Label(L"证据引用",310,535,85); Edit(Evidence,L"",400,530,640);
        Label(L"说明",18,575,65); Edit(Notes,L"",90,570,950);
        Add(L"BUTTON",L"保存本项观察",WS_TABSTOP,18,610,180,30,SaveObservation);
        Label(L"请保持本次诊断进程打开直至观察填写完成；每次进程启动生成独立证据目录。",215,616,830);
        if (options.diagnostic && probe.modulesReady) {
            runtime = std::make_unique<InputRuntime>(report);
            DWORD hotkeyCode{};
            hotkeys = registeredHotkeys.Acquire(h,hotkeyCode);
            if (!hotkeys) hotkeyError = L"诊断热键注册失败，Win32=" + std::to_wstring(hotkeyCode);
        }
        report.RecordDiagnostic("startup",options.diagnostic ? "diagnostic" : "view-only; launch with --diagnostic to run experiments");
        if (runtime) report.RecordDiagnostic("hotkey-registration",hotkeys ? "Ctrl+Alt+F5..F11 registered" : Utf8(hotkeyError));
        if (!runtime || !hotkeys) EnableWindow(GetDlgItem(h,Arm),FALSE);
        if (!runtime) {
            EnableWindow(GetDlgItem(h,Suspend),FALSE); EnableWindow(GetDlgItem(h,InertiaStop),FALSE);
        }
        SetTimer(h,1,250,nullptr);
        Defaults(); Refresh(); UpdateScrolls();
        if (runtime && hotkeys && !SetPropW(h,L"WinMouseFix.PreparedStartProtocol",reinterpret_cast<HANDLE>(static_cast<INT_PTR>(1))))
            throw std::runtime_error("Cannot register prepared-start control protocol");
    }
    void UpdateScrolls() {
        RECT client{}; GetClientRect(window,&client);
        const int oldX = scrollX, oldY = scrollY;
        SCROLLINFO si{}; si.cbSize = sizeof(si); si.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
        si.nMax = Px(1080)-1; si.nPage = static_cast<UINT>((std::max)(0L,client.right)); si.nPos = scrollX;
        SetScrollInfo(window,SB_HORZ,&si,TRUE); scrollX = GetScrollPos(window,SB_HORZ);
        si.nMax = Px(660)-1; si.nPage = static_cast<UINT>((std::max)(0L,client.bottom)); si.nPos = scrollY;
        SetScrollInfo(window,SB_VERT,&si,TRUE); scrollY = GetScrollPos(window,SB_VERT);
        if (oldX != scrollX || oldY != scrollY)
            ScrollWindowEx(window,oldX-scrollX,oldY-scrollY,nullptr,nullptr,nullptr,nullptr,SW_SCROLLCHILDREN | SW_INVALIDATE | SW_ERASE);
    }
    void Scroll(int bar, int action) {
        SCROLLINFO si{}; si.cbSize = sizeof(si); si.fMask = SIF_ALL; GetScrollInfo(window,bar,&si);
        int position = si.nPos;
        switch (action) {
        case SB_LINEUP: position -= Px(24); break;
        case SB_LINEDOWN: position += Px(24); break;
        case SB_PAGEUP: position -= static_cast<int>(si.nPage); break;
        case SB_PAGEDOWN: position += static_cast<int>(si.nPage); break;
        case SB_THUMBTRACK: position = si.nTrackPos; break;
        default: return;
        }
        position = std::clamp(position,0,(std::max)(0,si.nMax-static_cast<int>(si.nPage)+1));
        const int delta = si.nPos-position;
        SetScrollPos(window,bar,position,TRUE);
        if (bar == SB_HORZ) scrollX = position; else scrollY = position;
        ScrollWindowEx(window,bar == SB_HORZ ? delta : 0,bar == SB_VERT ? delta : 0,
            nullptr,nullptr,nullptr,nullptr,SW_SCROLLCHILDREN | SW_INVALIDATE | SW_ERASE);
    }
    void Refresh() {
        std::wstring message;
        if (!probe.modulesReady) message = L"模块错误：" + Wide(probe.moduleError);
        else if (!options.diagnostic) message = L"普通启动不会注入输入。启动参数 --diagnostic 可打开实验控制。";
        else if (!hotkeys) message = hotkeyError;
        else {
            const auto state = runtime->State();
            message = L"运行状态：" + std::to_wstring(static_cast<unsigned>(state)) +
                L"（0 空闲 / 1 待执行 / 2 运动 / 3 停止中 / 4 已停止）  最后结果：" + Wide(StatusName(runtime->LastStatus()));
            if (armed) message += L"  已准备：在目标上按 Ctrl+Alt+F5";
        }
        if ((probe.caps.apiBits & RequiredApis) != RequiredApis && probe.modulesReady) message += L"  当前系统缺少必需的原生触控板 API。";
        if (report.Failed()) { message += L"  证据写入失败，不得判定通过。"; EnableWindow(GetDlgItem(window,Arm),FALSE); }
        if (!lastError.empty()) message += L"\r\n错误：" + lastError;
        Set(RuntimeLabel,message);
        Set(SummaryLabel,Wide(report.Summary()));
    }
    void Prepare() {
        report.RecordDiagnostic("prepare-requested", "Checking target preparation");
        if (!runtime) throw std::runtime_error("Diagnostic runtime unavailable; launch with --diagnostic and check modules");
        if (!hotkeys) throw std::runtime_error(Utf8(hotkeyError));
        if (report.Failed()) throw std::runtime_error("Evidence writer failed; restart with a writable output directory");
        if ((probe.caps.apiBits & RequiredApis) != RequiredApis) throw std::runtime_error("Required native touchpad APIs are unavailable on this system");
        if (runtime->State() != RuntimeState::Idle) throw std::runtime_error("An experiment is already active");
        if (Text(GetDlgItem(window,Preparation)).find_first_not_of(L" \t\r\n") == std::wstring::npos)
            throw std::runtime_error("请先填写准备记录，例如：Edge 版本、可纵向滚动的长页面、当前页面位置。");
        if (SendDlgItemMessageW(window,Confirm,BM_GETCHECK,0,0) != BST_CHECKED)
            throw std::runtime_error("请先勾选准备记录下方的“已确认目标可用”复选框。");
        if (Selected(MotionProfile) == static_cast<int>(Profile::StartThenMicro) && Selected(Scenario) > 1)
            throw std::runtime_error("起始后微移仅用于 pan-y / pan-x 的双指固定诊断。");
        armed = true; lastError.clear();
        report.RecordDiagnostic("prepared","Activate target and press Ctrl+Alt+F5");
        Refresh();
    }
    void MicroDefaults() {
        if (Selected(MotionProfile) != static_cast<int>(Profile::StartThenMicro)) return;
        SendDlgItemMessageW(window,Scenario,CB_SETCURSEL,0,0);
        SendDlgItemMessageW(window,Fingers,CB_SETCURSEL,0,0);
        SendDlgItemMessageW(window,Mode,CB_SETCURSEL,0,0);
        Set(Duration,L"8"); Set(ExtentX,L"0"); Set(ExtentY,L"-16");
        armed = false; lastError.clear();
        report.RecordDiagnostic("profile-selected","Diagnostic only: 0-1s move 15mm; 1-4s hold; 4-6s move 1mm; 6-8s hold; release. Not a startup compensation.");
        Refresh();
    }
    StartInfo Capture() {
        StartInfo info;
        info.scenario = static_cast<Case>(Selected(Scenario));
        info.trajectory.fingers = static_cast<unsigned>(Selected(Fingers)+2);
        info.trajectory.profile = static_cast<Profile>(Selected(MotionProfile));
        info.trajectory.interactive = static_cast<unsigned>(Selected(Mode));
        info.trajectory.durationSeconds = Number(window,Duration);
        info.trajectory.extentXmm = Number(window,ExtentX);
        info.trajectory.extentYmm = Number(window,ExtentY);
        const double width = Number(window,Width), height = Number(window,Height);
        if (width < 40 || width > 1000 || height < 20 || height > 1000) throw std::runtime_error("Device dimensions are out of range");
        info.device.widthHimetric = static_cast<unsigned>(std::llround(width*100));
        info.device.heightHimetric = static_cast<unsigned>(std::llround(height*100));
        constexpr unsigned hz[]{60,120,240};
        info.hz = hz[Selected(Hz)];
        const HWND foreground = GetForegroundWindow();
        DWORD pid{}; GetWindowThreadProcessId(foreground,&pid);
        if (!pid || pid == GetCurrentProcessId()) throw std::runtime_error("Activate the target window before starting the experiment");
        info.targetPid = pid; info.dpi = GetDpiForWindow(foreground);
        POINT cursor{};
        if (!GetCursorPos(&cursor)) throw std::runtime_error("Cannot capture cursor anchor");
        info.anchorX = cursor.x; info.anchorY = cursor.y;
        MONITORINFOEXW monitor{}; monitor.cbSize = sizeof(monitor);
        if (GetMonitorInfoW(MonitorFromPoint(cursor,MONITOR_DEFAULTTONEAREST),&monitor)) {
            DEVMODEW mode{}; mode.dmSize = sizeof(mode);
            if (EnumDisplaySettingsW(monitor.szDevice,ENUM_CURRENT_SETTINGS,&mode)) info.refreshHz = mode.dmDisplayFrequency;
        }
        const auto prep = Text(GetDlgItem(window,Preparation));
        if (prep.size() >= std::size(info.preparation)) throw std::runtime_error("Preparation record must be shorter than 384 characters");
        wcscpy_s(info.preparation,prep.c_str());
        info.prepared = SendDlgItemMessageW(window,Confirm,BM_GETCHECK,0,0) == BST_CHECKED;
        return info;
    }
    void Start() {
        if (!armed || !runtime) throw std::runtime_error("Prepare an experiment first");
        const auto result = runtime->Start(Capture());
        if (!result) throw std::runtime_error(StatusName(result.code));
        armed = false;
        report.RecordDiagnostic("start-accepted","Queued on input thread",runtime->LatestExperiment());
        Set(ExperimentId,std::to_wstring(runtime->LatestExperiment()));
    }
    void Hotkey(int id) {
        report.RecordDiagnostic("hotkey-received","Ctrl+Alt+F" + std::to_string(id+4));
        if (!runtime) throw std::runtime_error("Diagnostic runtime unavailable; launch with --diagnostic and check modules");
        Result result;
        switch (id) {
        case 1: Start(); break;
        case 2: result = runtime->ControlMotion(Control::Advance); break;
        case 3: result = runtime->ControlMotion(Control::HoldMotion); break;
        case 4: result = runtime->ControlMotion(Control::Reverse); break;
        case 5: result = runtime->ControlMotion(Control::Release); break;
        case 6: runtime->Cancel(); armed = false; break;
        case 7: result = runtime->StopInertia(); break;
        default: return;
        }
        if (!result) throw std::runtime_error(StatusName(result.code));
        lastError.clear();
        Refresh();
    }
    void Error(const std::exception& error, UINT message, WPARAM parameter) {
        lastError = Wide(error.what());
        const auto action = message == WM_HOTKEY ? "hotkey-" + std::to_string(parameter) :
            message == WM_COMMAND ? "command-" + std::to_string(LOWORD(parameter)) : "window-message-" + std::to_string(message);
        report.RecordDiagnostic("rejected",action + ": " + error.what());
        Refresh();
    }
    void Observe() {
        const int index = Selected(ObservationItem);
        if (index < 0 || static_cast<std::size_t>(index) >= baseline.size()) return;
        auto item = baseline[static_cast<std::size_t>(index)];
        item.finding = static_cast<Finding>(Selected(ObservationResult));
        const double id = Number(window,ExperimentId);
        if (id < 1 || id > 1000000 || std::floor(id) != id) throw std::runtime_error("A valid experiment ID is required");
        item.experiment = static_cast<unsigned>(id);
        item.observer = Utf8(Text(GetDlgItem(window,Observer)));
        item.evidence = Utf8(Text(GetDlgItem(window,Evidence)));
        item.notes = Utf8(Text(GetDlgItem(window,Notes)));
        item.utc = UtcNow();
        if (!report.Observe(static_cast<unsigned>(index),item))
            throw std::runtime_error("Observation requires an observer, evidence reference and a valid result");
    }
    void Shutdown() {
        armed = false;
        registeredHotkeys.Release();
        if (runtime) { runtime->Stop(); runtime.reset(); }
        report.Finish();
    }
    ~Panel() { if (runtime) runtime->Stop(); if (font) DeleteObject(font); }
};
LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM w, LPARAM l) {
    auto panel = reinterpret_cast<Panel*>(GetWindowLongPtrW(window,GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        panel = static_cast<Panel*>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams);
        SetWindowLongPtrW(window,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(panel));
    }
    try {
        switch (message) {
        case WM_CREATE: panel->Initialize(window); return 0;
        case WM_SIZE: if (panel && panel->window) panel->UpdateScrolls(); return 0;
        case WM_HSCROLL: if (panel) panel->Scroll(SB_HORZ,LOWORD(w)); return 0;
        case WM_VSCROLL: if (panel) panel->Scroll(SB_VERT,LOWORD(w)); return 0;
        case WM_TIMER: if (panel) panel->Refresh(); return 0;
        case WM_HOTKEY: if (panel) panel->Hotkey(static_cast<int>(w)); return 0;
        case StartPreparedControlMessage:
            if (panel) {
                panel->report.RecordDiagnostic("prepared-start-control","Explicit non-keyboard start request");
                panel->Start(); panel->lastError.clear(); panel->Refresh();
            }
            return 0;
        case WM_COMMAND:
            if (!panel) break;
            if (LOWORD(w) == Scenario && HIWORD(w) == CBN_SELCHANGE) panel->Defaults();
            else if (LOWORD(w) == MotionProfile && HIWORD(w) == CBN_SELCHANGE) panel->MicroDefaults();
            else if (LOWORD(w) == Arm) panel->Prepare();
            else if (LOWORD(w) == Suspend && panel->runtime) { panel->runtime->Suspend(); panel->armed = false; }
            else if (LOWORD(w) == InertiaStop && panel->runtime) panel->runtime->StopInertia();
            else if (LOWORD(w) == SaveObservation) panel->Observe();
            return 0;
        case WM_WTSSESSION_CHANGE:
            if (panel && w == WTS_SESSION_LOCK && panel->runtime) { panel->armed = false; panel->runtime->Suspend(); }
            return 0;
        case WM_QUERYENDSESSION: if (panel) panel->Shutdown(); return TRUE;
        case WM_CLOSE: if (panel) panel->Shutdown(); DestroyWindow(window); return 0;
        case WM_DESTROY:
            RemovePropW(window,L"WinMouseFix.PreparedStartProtocol");
            WTSUnRegisterSessionNotification(window);
            KillTimer(window,1); PostQuitMessage(0); return 0;
        }
    } catch (const std::exception& error) {
        if (panel) panel->Error(error,message,w);
        if (message == WM_CREATE) return -1;
        // Do not steal target focus with a modal error dialog during a gesture.
        return 0;
    }
    return DefWindowProcW(window,message,w,l);
}
int Run(const Options& options, HINSTANCE instance) {
    InputOwner inputOwner;
    if (options.diagnostic && !options.probe && !inputOwner.Acquire())
        throw std::runtime_error("Another diagnostic/content-drag input owner is already active");
    NativeModules modules;
    std::wstring error;
    ProbeInfo probe;
    probe.simulation = options.simulation;
    probe.modulesReady = modules.Load(options.modules,error);
    probe.moduleError = Utf8(error);
    if (probe.modulesReady) {
        // Actual calls also exercise the linked import libraries and delay-load thunks.
        if (WmfCoreVersion() != AbiVersion || WmfPlatformVersion() != AbiVersion) throw std::runtime_error("ABI mismatch");
        probe.query = WmfPlatformCapabilities(&probe.caps);
    } else probe.query = {Status::InvalidState};
    if (options.probe) {
        Result smokeResult{};
        if (probe.modulesReady && (options.probeDevice || options.smoke)) {
            std::thread worker([&] {
                PlatformHandle platform{};
                auto result = WmfPlatformCreate(&platform);
                probe.platformAttempted = true; probe.platformInstance = result;
                if (result && options.smoke) {
                    CoreHandle core{}; TrajectoryConfig config;
                    result = WmfCoreCreate(&config,&core);
                    probe.coreAttempted = true; probe.coreInstance = result;
                    if (core) WmfCoreDestroy(&core);
                }
                smokeResult = result;
                if (options.probeDevice) {
                    probe.deviceAttempted = true;
                    if (result) { DeviceConfig config; result = WmfPlatformOpen(platform,&config); }
                    probe.device = result;
                }
                if (platform) probe.cleanup = WmfPlatformDestroy(&platform);
            });
            worker.join();
            probe.instancesAttempted = true; probe.instances = smokeResult;
        }
        ReportWriter report(options.output,probe);
        report.Finish();
        if (report.Failed()) return 4;
        if (!probe.modulesReady) return 3;
        if (options.smoke) return smokeResult ? 0 : 3;
        if (!probe.query || (probe.caps.apiBits & RequiredApis) != RequiredApis) return 2;
        if (options.probeDevice && !probe.device) return 2;
        return 0;
    }
    ReportWriter report(options.output,probe);
    Panel panel{options,probe,report};
    WNDCLASSEXW cls{}; cls.cbSize = sizeof(cls); cls.hInstance = instance;
    cls.lpfnWndProc = WindowProc; cls.lpszClassName = L"WinMouseFix.NativeDiagnostic";
    cls.hCursor = LoadCursorW(nullptr,IDC_ARROW); cls.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW+1);
    if (!RegisterClassExW(&cls)) throw std::runtime_error("Cannot register diagnostic window");
    const auto dpi = GetDpiForSystem();
    auto window = CreateWindowExW(0,cls.lpszClassName,L"WinMouseFix — Native Gesture Diagnostics",
        WS_OVERLAPPEDWINDOW | WS_HSCROLL | WS_VSCROLL, CW_USEDEFAULT,CW_USEDEFAULT,MulDiv(1100,dpi,96),MulDiv(725,dpi,96),
        nullptr,nullptr,instance,&panel);
    if (!window) throw std::runtime_error("Cannot create diagnostic window");
    if (!WTSRegisterSessionNotification(window,NOTIFY_FOR_THIS_SESSION)) {
        const DWORD code = GetLastError();
        DestroyWindow(window);
        throw std::runtime_error("Cannot register session-lock cleanup, Win32=" + std::to_string(code));
    }
    ShowWindow(window,SW_SHOW);
    MSG message{};
    int result{};
    while ((result = GetMessageW(&message,nullptr,0,0)) > 0) {
        if (!IsDialogMessageW(window,&message)) { TranslateMessage(&message); DispatchMessageW(&message); }
    }
    panel.Shutdown();
    return result < 0 ? 1 : 0;
}
}
int APIENTRY wWinMain(HINSTANCE instance, HINSTANCE, LPWSTR, int) {
    bool headless = wcsstr(GetCommandLineW(),L"--resident-smoke") != nullptr || wcsstr(GetCommandLineW(),L"--product-probe-smoke") != nullptr;
    try { int contentResult{}; if (TryRunContentMode(instance,contentResult)) return contentResult;
        int probeResult{}; if (TryRunProductProbe(instance,probeResult)) return probeResult;
        int residentResult{}; if (TryRunResidentMode(instance,residentResult)) return residentResult;
        const auto options = Parse(); headless = options.probe; return Run(options,instance); }
    catch (const std::exception& error) {
        if (headless) OutputDebugStringW(Wide(error.what()).c_str());
        else MessageBoxW(nullptr,Wide(error.what()).c_str(),L"WinMouseFix startup error",MB_OK | MB_ICONERROR);
        return 1;
    }
}
