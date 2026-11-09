#include "ResidentMode.h"
#include "ResidentTray.h"
#include "ProductService.h"
#include "ProductSimulation.h"
#include "ProductPaths.h"
#include "ProductLog.h"
#include "NativeModules.h"
#include "InputOwner.h"
#include "ContentControls.h"
#include "JsonSupport.h"
#include "DiagnosticText.h"
#include "Report.h"
#include "CapabilityEvidence.h"
#include "UpdateService.h"
#include "ProductVersion.h"
#include "FullscreenMonitor.h"
#include <fstream>
#include <future>
#include <vector>

namespace wmf::host {
namespace {
constexpr UINT OpenSettingsMessage = WM_APP+20, ExitMessage = WM_APP+21, SwitchDataMessage = WM_APP+22, FullscreenMessage = WM_APP+23;
constexpr int SettingsId = 101,ExitId = 102,ToggleId = 103,StartupId = 104;
constexpr wchar_t ClassName[] = L"WinMouseFix.Resident.v1";
struct Options { bool settings{},startup{},smoke{},trace{}; std::filesystem::path output,updateTrial; std::wstring simulationTag; unsigned smokeHoldMs{}; DWORD restartAfter{},restartSettings{}; bool migrate{}; DataLocation migrationFrom{},migrationTo{}; };
struct Resident {
    HINSTANCE instance{}; HWND window{},settingsWindow{}; DWORD settingsPid{};
    ProductEnvironment environment; Options options;
    ProductSimulation simulation;
    FullscreenMonitor fullscreenMonitor;
    FullscreenPause fullscreen;
    std::atomic<bool> fullscreenDetected{};
    unsigned adds{},deletes{},icons{},destroyedIcons{},settingsOpened{},step{},startupNotifications{};
    bool trayPresent{},firstPaused{},rebuildPreserved{};
    std::atomic<bool> closing{};
    UINT taskbarCreated{};
    std::mutex settingsMutex;
    std::unique_ptr<ResidentTray> tray;
    std::unique_ptr<ContentControls> controls;
    std::unique_ptr<ConfigStore> store;
    std::unique_ptr<ProductRuntime> runtime;
    std::unique_ptr<StartupRegistration> startup;
    std::unique_ptr<ProductLog> log;
    std::unique_ptr<CapabilityEvidence> evidence;
    std::unique_ptr<UpdateService> updates;
    std::unique_ptr<ProductService> service;
    std::filesystem::path updateHandoff;
    HANDLE updaterProcess{};
    std::uint64_t handoffStarted{};
    bool updatePermitted{};
    std::vector<std::future<ConfigError>> pending;
    std::wstring lastTip;
    std::uint64_t started{};
    bool faultReported{};
    std::uint32_t reportedCleanup{};
    ~Resident() {
        fullscreenMonitor.Stop();
        if (updaterProcess) CloseHandle(updaterProcess);
        if (runtime) runtime->Stop();
        if (service) service->Stop();
        for (auto& result : pending) { try { if (result.valid()) result.get(); } catch (...) {} } pending.clear();
        service.reset(); updates.reset(); log.reset(); runtime.reset();
        tray.reset(); controls.reset();
        if (window && IsWindow(window)) DestroyWindow(window);
    }
    void OpenSettings() {
        if (options.smoke) { ++settingsOpened; return; }
        std::lock_guard lock(settingsMutex);
        if (settingsWindow && IsWindow(settingsWindow) && ProcessImageMatches(settingsPid,SettingsExecutable(environment.executableDirectory))) {
            ShowWindow(settingsWindow,SW_RESTORE); SetForegroundWindow(settingsWindow); return;
        }
        const auto executable = SettingsExecutable(environment.executableDirectory);
        if (!std::filesystem::exists(executable)) { MessageBoxW(window,L"找不到 Settings。请从完整发布目录启动，或先构建整个解决方案。",L"WinMouseFix",MB_OK|MB_ICONERROR); return; }
        std::wstring command = L"\""+executable.wstring()+L"\"";
        STARTUPINFOW startupInfo{sizeof(startupInfo)}; PROCESS_INFORMATION process{};
        if (!CreateProcessW(executable.c_str(),command.data(),nullptr,nullptr,FALSE,0,nullptr,executable.parent_path().c_str(),&startupInfo,&process))
            MessageBoxW(window,L"无法启动设置窗口。",L"WinMouseFix",MB_OK|MB_ICONERROR);
        else {
            settingsPid = process.dwProcessId;
            if (!options.updateTrial.empty()) wmf::update::WriteText(options.updateTrial/L"settings.pid",std::to_string(settingsPid));
            CloseHandle(process.hThread); CloseHandle(process.hProcess);
        }
    }
    void Enable(bool value) { if (service && !closing) pending.push_back(service->SetEnabled(value)); }
    void RestartForData(DataLocation target) {
        if (closing) return;
        // Fake sessions never launch a production replacement process.
        if (options.smoke) { service->DataLocationSwitchFailed({Status::InvalidState,ERROR_NOT_SUPPORTED}); return; }
        const auto saved = store->EnsureSaved();
        if (!saved) { service->DataLocationSwitchFailed(saved.result); return; }
        const auto executable = environment.executableDirectory/L"WinMouseFix.Host.exe";
        const auto wide = [](DataLocation value) { return value == DataLocation::Program ? L"program" : L"appdata"; };
        std::wstring command = L"\""+executable.wstring()+L"\" --settings --restart-after "+std::to_wstring(GetCurrentProcessId())+L" --storage-from "+wide(environment.dataLocation)+L" --storage-to "+wide(target);
        { std::lock_guard lock(settingsMutex); if (settingsPid && ProcessImageMatches(settingsPid,SettingsExecutable(environment.executableDirectory))) command += L" --restart-settings "+std::to_wstring(settingsPid); }
        STARTUPINFOW info{sizeof(info)}; PROCESS_INFORMATION process{};
        if (!CreateProcessW(executable.c_str(),command.data(),nullptr,nullptr,FALSE,0,nullptr,environment.executableDirectory.c_str(),&info,&process)) { service->DataLocationSwitchFailed({Status::NativeFailure,GetLastError()}); return; }
        CloseHandle(process.hThread); CloseHandle(process.hProcess);
        service->BeginExit(false);
    }
    void BeginUpdate(const std::filesystem::path& tx) {
        if (options.smoke || closing) { updates->InstallFailed("update-not-available"); return; }
        try {
            const auto saved = store->EnsureSaved(); if (!saved) throw std::runtime_error("configuration-save-failed");
            wmf::update::WriteText(tx/L"data-directory.txt",winrt::to_string(environment.dataDirectory.wstring()));
            const auto worker = tx/L"worker.exe"; std::filesystem::copy_file(environment.executableDirectory/L"WinMouseFix.Updater.exe",worker,std::filesystem::copy_options::overwrite_existing);
            auto command = L"\""+worker.wstring()+L"\" --transaction \""+tx.wstring()+L"\" --host-pid "+std::to_wstring(GetCurrentProcessId());
            { std::lock_guard lock(settingsMutex); if (settingsPid && ProcessImageMatches(settingsPid,SettingsExecutable(environment.executableDirectory))) command += L" --settings-pid "+std::to_wstring(settingsPid); }
            STARTUPINFOW info{sizeof(info)}; PROCESS_INFORMATION process{};
            if (!CreateProcessW(worker.c_str(),command.data(),nullptr,nullptr,FALSE,0,nullptr,tx.c_str(),&info,&process)) throw std::runtime_error("updater-start-failed");
            CloseHandle(process.hThread); updaterProcess = process.hProcess; updateHandoff = tx; handoffStarted = GetTickCount64();
        } catch (const std::exception& error) { updates->InstallFailed(error.what()); }
    }
    std::wstring Tip() const {
        if (!runtime) return L"WinMouseFix — 正在准备";
        if (runtime->Requested() && runtime->FullscreenPaused() && runtime->State()==ProductRuntimeState::Paused) return L"WinMouseFix — 全屏应用中，增强已自动暂停";
        constexpr const wchar_t* states[]{L"已暂停",L"正在启动",L"已启用",L"发生错误，已暂停",L"正在退出",L"已停止"};
        return L"WinMouseFix — "+std::wstring(states[static_cast<unsigned>(runtime->State())]);
    }
    void RefreshFullscreen() {
        if (!runtime || !store || options.smoke) return;
        const auto active=fullscreen.Update(true,ReadFullscreenWindow(),GetTickCount64());
        fullscreenDetected=active;
        const bool pause=store->Current()->config.disableInFullscreen && active;
        const bool changed=runtime->FullscreenPaused()!=pause;
        runtime->SetFullscreenPaused(pause);
        if (changed && log) log->Record("fullscreen-pause",pause ? "entered" : "released");
    }
    void RefreshTray(bool recreate = false) {
        const auto tip = Tip(); if (!recreate && tip == lastTip) return;
        const auto result = tray->Update(tip,recreate); lastTip = result ? tip : L"";
        if (!result) { if (runtime) Enable(false); ShowWindow(window,SW_SHOWNOACTIVATE); }
        SetWindowTextW(GetDlgItem(window,100),result ? tip.c_str() : L"托盘图标不可用。可在这里打开设置、暂停或退出。");
    }
    void Menu() {
        const auto menu = CreatePopupMenu();
        AppendMenuW(menu,MF_STRING,SettingsId,L"设置(&S)");
        AppendMenuW(menu,MF_STRING|(runtime && runtime->Requested() ? MF_CHECKED : 0),ToggleId,L"启用增强(&E)");
        AppendMenuW(menu,MF_STRING|(startup && startup->Status().registered ? MF_CHECKED : 0),StartupId,L"登录时启动(&L)");
        AppendMenuW(menu,MF_SEPARATOR,0,nullptr); AppendMenuW(menu,MF_STRING,ExitId,L"退出(&X)");
        POINT cursor{}; GetCursorPos(&cursor); SetForegroundWindow(window);
        const auto selected = TrackPopupMenu(menu,TPM_RETURNCMD|TPM_RIGHTBUTTON,cursor.x,cursor.y,0,window,nullptr); DestroyMenu(menu);
        if (selected) PostMessageW(window,WM_COMMAND,selected,0); PostMessageW(window,WM_NULL,0,0);
    }
    void ToggleStartup() {
        const auto current = store->Current()->config;
        pending.push_back(std::async(std::launch::async,[this,current] {
            using namespace winrt::Windows::Data::Json; EnsureJsonApartment(); JsonObject request,payload;
            request.SetNamedValue(L"protocolVersion",JsonValue::CreateNumberValue(1)); request.SetNamedValue(L"requestId",JsonValue::CreateStringValue(winrt::to_hstring("tray-"+std::to_string(GetTickCount64()))));
            request.SetNamedValue(L"hostRunId",JsonValue::CreateStringValue(winrt::to_hstring(service->RunId()))); request.SetNamedValue(L"command",JsonValue::CreateStringValue(L"set-startup"));
            request.SetNamedValue(L"expectedRevision",JsonValue::CreateNumberValue(static_cast<double>(current.input.revision))); payload.SetNamedValue(L"enabled",JsonValue::CreateBooleanValue(!startup->Status().registered)); request.SetNamedValue(L"payload",payload);
            const auto reply = ParseJsonObject(service->Handle(winrt::to_string(request.Stringify()),UINT64_MAX,GetCurrentProcessId()));
            if (!reply.GetNamedBoolean(L"ok") || reply.GetNamedNumber(L"nativeError")) return ConfigError{ConfigProblem::Storage,"startup","Startup registration failed"}; return ConfigError{};
        }));
    }
    void Tick() {
        if (!runtime || !service) return;
        RefreshFullscreen();
        service->Tick();
        if (!options.updateTrial.empty()) {
            const auto tx = options.updateTrial;
            if (std::filesystem::exists(tx/L"abort")) {
                { std::lock_guard lock(settingsMutex); if (settingsWindow && ProcessImageMatches(settingsPid,SettingsExecutable(environment.executableDirectory))) PostMessageW(settingsWindow,WM_CLOSE,0,0); }
                service->BeginExit(false);
            } else if (std::filesystem::exists(tx/L"commit.ok") || (!wmf::update::TransactionPending(environment.executableDirectory) && wmf::update::ReadText(tx/L"journal.json").find("completed") != std::string::npos)) {
                options.updateTrial.clear(); if (updatePermitted) runtime->Enable(store->Current()->config.enabled);
            }
        }
        if (!updateHandoff.empty()) {
            if (std::filesystem::exists(updateHandoff/L"handoff.ready") && WaitForSingleObject(updaterProcess,0) == WAIT_TIMEOUT) {
                { std::lock_guard lock(settingsMutex); if (settingsWindow && ProcessImageMatches(settingsPid,SettingsExecutable(environment.executableDirectory))) PostMessageW(settingsWindow,WM_CLOSE,0,0); }
                service->BeginExit(false); updateHandoff.clear();
            } else if (WaitForSingleObject(updaterProcess,0) == WAIT_OBJECT_0 || GetTickCount64()-handoffStarted > 60000) {
                wmf::update::WriteText(updateHandoff/L"handoff.cancel","cancelled");
                updates->InstallFailed("updater-handoff-failed"); updateHandoff.clear(); CloseHandle(updaterProcess); updaterProcess = nullptr;
            }
        }
        if (updates && !closing && options.updateTrial.empty()) {
            const auto configuration = store->Current();
            updates->Tick(GetTickCount64(),!configuration->recoveryRequired && configuration->config.automaticUpdates,configuration->config.updateIntervalHours);
            if (updates->TakePrompt(!fullscreenDetected)) OpenSettings();
            updates->RequestInstall(); // Download was explicitly approved as download-and-install.
            if (const auto tx = updates->TakeInstall(); !tx.empty()) BeginUpdate(tx);
        }
        const auto failure = runtime->Failure();
        if (!failure && log && (!faultReported || failure.cleanupError != reportedCleanup)) {
            log->Record(faultReported ? "runtime-cleanup-error" : "runtime-first-failure",JsonResult(failure),store->Current()->config.input.revision);
            faultReported = true; reportedCleanup = failure.cleanupError;
        }
        for (auto it = pending.begin(); it != pending.end();) {
            if (it->wait_for(std::chrono::seconds(0)) != std::future_status::ready) { ++it; continue; }
            const auto result = it->get(); if (!result && log) log->Record("configuration-error",result.message); it = pending.erase(it);
        }
        RefreshTray();
        if (options.smoke && !closing) {
            if (step == 0) {
                firstPaused = !runtime->Requested() && simulation.captures == 0 && simulation.platforms == 0;
                simulation.scripted = true; PostMessageW(window,WM_COMMAND,ToggleId,0); step = 1;
            } else if (step == 1 && simulation.completions && runtime->State() == ProductRuntimeState::Enabled) {
                const auto captures = simulation.captures.load(); trayPresent = false; RefreshTray(true); rebuildPreserved = simulation.captures == captures;
                PostMessageW(window,WM_WTSSESSION_CHANGE,WTS_SESSION_LOCK,0); OpenSettings(); step = 2;
            } else if (step == 2 && simulation.destroyed && runtime->State() == ProductRuntimeState::Paused && GetTickCount64()-started >= options.smokeHoldMs) PostMessageW(window,WM_CLOSE,0,0);
            if (GetTickCount64()-started > 10000) PostMessageW(window,WM_CLOSE,0,0);
        }
        if (service->ExitRequested()) closing = true;
        if (closing && service->MayClose()) DestroyWindow(window);
    }
    static LRESULT CALLBACK Procedure(HWND window,UINT message,WPARAM a,LPARAM b) noexcept {
        auto* state = reinterpret_cast<Resident*>(GetWindowLongPtrW(window,GWLP_USERDATA));
        if (message == WM_NCCREATE) { state = static_cast<Resident*>(reinterpret_cast<CREATESTRUCTW*>(b)->lpCreateParams); state->window = window; SetWindowLongPtrW(window,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(state)); }
        if (state) try {
            if (message == WM_CREATE) {
                CreateWindowW(L"STATIC",L"WinMouseFix",WS_CHILD|WS_VISIBLE,20,20,470,45,window,reinterpret_cast<HMENU>(100),state->instance,nullptr);
                CreateWindowW(L"BUTTON",L"设置",WS_CHILD|WS_VISIBLE|WS_TABSTOP,20,85,120,32,window,reinterpret_cast<HMENU>(static_cast<INT_PTR>(SettingsId)),state->instance,nullptr);
                CreateWindowW(L"BUTTON",L"启用 / 暂停",WS_CHILD|WS_VISIBLE|WS_TABSTOP,150,85,120,32,window,reinterpret_cast<HMENU>(static_cast<INT_PTR>(ToggleId)),state->instance,nullptr);
                CreateWindowW(L"BUTTON",L"退出",WS_CHILD|WS_VISIBLE|WS_TABSTOP,280,85,120,32,window,reinterpret_cast<HMENU>(static_cast<INT_PTR>(ExitId)),state->instance,nullptr); return 0;
            }
            if (state->taskbarCreated && message == state->taskbarCreated && state->tray) { state->RefreshTray(true); return 0; }
            if (message == ResidentTrayMessage) {
                const auto notification = LOWORD(b);
                if (notification == NIN_SELECT || notification == NIN_KEYSELECT || notification == WM_LBUTTONUP || notification == NIN_BALLOONUSERCLICK) state->OpenSettings();
                if (notification == WM_CONTEXTMENU || notification == WM_RBUTTONUP) state->Menu(); return 0;
            }
            if (message == OpenSettingsMessage) { state->OpenSettings(); return 0; }
            if (message == FullscreenMessage) { state->fullscreenMonitor.Acknowledge(); state->RefreshFullscreen(); state->RefreshTray(); return 0; }
            if (message == SwitchDataMessage) { state->RestartForData(static_cast<DataLocation>(a)); return 0; }
            if (message == ExitMessage || message == WM_CLOSE) { state->closing = true; if (state->service) state->service->BeginExit(false); else DestroyWindow(window); return 0; }
            if (message == WM_COMMAND) {
                switch (LOWORD(a)) { case SettingsId: state->OpenSettings(); break; case ExitId: PostMessageW(window,WM_CLOSE,0,0); break; case ToggleId: state->Enable(state->service && state->service->Recording() ? false : !state->runtime->Requested()); break; case StartupId: state->ToggleStartup(); break; } return 0;
            }
            if (message == WM_WTSSESSION_CHANGE && a == WTS_SESSION_LOCK) { state->Enable(false); return 0; }
            if (message == WM_TIMER) { state->Tick(); return 0; }
            if ((message == WM_DPICHANGED || message == WM_SETTINGCHANGE || message == WM_DISPLAYCHANGE) && state->tray) { state->RefreshFullscreen(); state->lastTip.clear(); state->RefreshTray(); }
            if (message == WM_DESTROY) { state->fullscreenMonitor.Stop(); KillTimer(window,1); if (state->tray) state->tray->Remove(); if (state->controls) state->controls->Release(); PostQuitMessage(0); return 0; }
        } catch (...) { if (state->runtime) state->runtime->ReportFailure({Status::NativeFailure,ERROR_UNHANDLED_EXCEPTION}); PostMessageW(window,WM_CLOSE,0,0); }
        return DefWindowProcW(window,message,a,b);
    }
};
bool ActivateExisting(const ProductEnvironment& environment) {
    if (!WaitNamedPipeW(environment.pipe.c_str(),2000)) return false;
    const auto pipe = CreateFileW(environment.pipe.c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,OPEN_EXISTING,FILE_FLAG_OVERLAPPED,nullptr); if (pipe == INVALID_HANDLE_VALUE) return false;
    const auto stop = CreateEventW(nullptr,TRUE,FALSE,nullptr); ULONG pid{}; bool valid = GetNamedPipeServerProcessId(pipe,&pid) && ProcessImageMatches(pid,environment.executableDirectory/L"WinMouseFix.Host.exe");
    try {
        using namespace winrt::Windows::Data::Json; EnsureJsonApartment(); JsonObject root,payload;
        root.SetNamedValue(L"protocolVersion",JsonValue::CreateNumberValue(1)); root.SetNamedValue(L"requestId",JsonValue::CreateStringValue(winrt::to_hstring("hello-"+std::to_string(GetCurrentProcessId())+"-"+std::to_string(GetTickCount64()))));
        root.SetNamedValue(L"hostRunId",JsonValue::CreateStringValue(L"")); root.SetNamedValue(L"command",JsonValue::CreateStringValue(L"hello")); root.SetNamedValue(L"expectedRevision",JsonValue::CreateNumberValue(0)); root.SetNamedValue(L"payload",payload);
        std::string reply;
        if (valid) valid = static_cast<bool>(WriteControlFrame(pipe,stop,winrt::to_string(root.Stringify()))) && static_cast<bool>(ReadControlFrame(pipe,stop,reply));
        if (valid) { const auto hello = ParseJsonObject(reply); valid = hello.GetNamedBoolean(L"ok"); root.SetNamedValue(L"hostRunId",hello.GetNamedValue(L"hostRunId")); root.SetNamedValue(L"command",JsonValue::CreateStringValue(L"open-settings")); root.SetNamedValue(L"requestId",JsonValue::CreateStringValue(winrt::to_hstring("open-"+std::to_string(GetCurrentProcessId())+"-"+std::to_string(GetTickCount64())))); }
        if (valid) valid = static_cast<bool>(WriteControlFrame(pipe,stop,winrt::to_string(root.Stringify()))) && static_cast<bool>(ReadControlFrame(pipe,stop,reply)) && ParseJsonObject(reply).GetNamedBoolean(L"ok");
    } catch (...) { valid = false; }
    if (stop) CloseHandle(stop); CloseHandle(pipe); return valid;
}
int RunResident(HINSTANCE instance,const Options& options) {
    auto environment = options.smoke ? ProductEnvironment::Simulation(options.output,options.simulationTag) : ProductEnvironment::Production();
    if (!options.smoke && options.updateTrial.empty() && wmf::update::TransactionPending(environment.executableDirectory)) {
        const auto tx = wmf::update::PendingTransaction(environment.executableDirectory),worker = tx/L"worker.exe";
        auto command = L"\""+worker.wstring()+L"\" --recover \""+tx.wstring()+L"\" --host-pid "+std::to_wstring(GetCurrentProcessId());
        STARTUPINFOW info{sizeof(info)}; PROCESS_INFORMATION process{};
        if (!CreateProcessW(worker.c_str(),command.data(),nullptr,nullptr,FALSE,0,nullptr,tx.c_str(),&info,&process)) throw std::runtime_error("Cannot recover an interrupted update");
        CloseHandle(process.hThread); CloseHandle(process.hProcess); return 0;
    }
    if (!options.updateTrial.empty()) {
        wmf::update::CheckTransactionPath(options.updateTrial);
        if (!wmf::update::TransactionPending(environment.executableDirectory) || !std::filesystem::equivalent(wmf::update::PendingTransaction(environment.executableDirectory),options.updateTrial) || wmf::update::ReadManifest(environment.executableDirectory).version != ProductVersion) throw std::runtime_error("Invalid update verification");
    }
    UserSecurity security(environment.sid); const auto mutex = CreateMutexW(security.Attributes(),FALSE,environment.hostMutex.c_str()); const auto mutexError = GetLastError();
    if (!mutex) throw std::runtime_error("Cannot create resident instance lock");
    struct Handle { HANDLE h; ~Handle() { CloseHandle(h); } } instanceLock{mutex};
    if (mutexError == ERROR_ALREADY_EXISTS) return options.startup || ActivateExisting(environment) ? 0 : 1;
    InputOwner owner(environment.inputOwnerSuffix); if (!owner.Acquire()) throw std::runtime_error("另一个诊断或输入实例正在运行，请先退出它。");
    if (!options.smoke && options.updateTrial.empty()) wmf::update::CleanupTransactions(environment.executableDirectory);
    if (options.migrate) {
        DataMigration moved;
        if (environment.dataLocation != options.migrationFrom) moved.result = {Status::InvalidState,ERROR_REVISION_MISMATCH};
        else moved = MigrateData(environment.dataPaths,options.migrationFrom,options.migrationTo);
        if (!moved.result) MessageBoxW(nullptr,L"配置迁移未完成，已保留原配置位置和数据。请检查目标目录的写入权限、磁盘空间或文件占用后重试。",L"Win Mouse Fix",MB_OK|MB_ICONWARNING);
        else if (moved.sourceRetained) MessageBoxW(nullptr,L"配置已切换成功。原目录被占用，旧数据仍保留在原位置。",L"Win Mouse Fix",MB_OK|MB_ICONINFORMATION);
        environment.dataLocation = ReadDataLocation(environment.dataPaths); environment.dataDirectory = environment.dataPaths.Directory(environment.dataLocation);
    }
    NativeModules modules; std::wstring moduleError; if (!modules.Load(environment.executableDirectory,moduleError)) throw std::runtime_error(Utf8(moduleError));
    Resident state; state.instance = instance; state.options = options; state.environment = environment; state.started = GetTickCount64();
    WNDCLASSEXW cls{sizeof(cls)}; cls.hInstance = instance; cls.lpszClassName = ClassName; cls.lpfnWndProc = Resident::Procedure; cls.hCursor = LoadCursorW(nullptr,IDC_ARROW); cls.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW+1);
    if (!RegisterClassExW(&cls) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) throw std::runtime_error("Cannot register resident control window");
    if (!CreateWindowExW(0,ClassName,L"WinMouseFix 后台状态",WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU,CW_USEDEFAULT,CW_USEDEFAULT,510,175,nullptr,nullptr,instance,&state)) throw std::runtime_error("Cannot create resident control window");
    state.taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    if (options.smoke) state.controls = std::make_unique<ContentControls>([](HWND,DWORD)->BOOL{return TRUE;},[](HWND)->BOOL{return TRUE;});
    else state.controls = std::make_unique<ContentControls>();
    if (!state.controls->Acquire(state.window)) throw std::runtime_error("无法注册会话锁定通知；输入未启用。");
    auto trayApi = RealTrayApi();
    if (options.smoke) {
        trayApi = {&state,
            [](void* p,DWORD command,NOTIFYICONDATAW* data)->BOOL { auto& s = *static_cast<Resident*>(p); if (command == NIM_ADD) { ++s.adds; s.trayPresent = true; } if (command == NIM_DELETE) { ++s.deletes; s.trayPresent = false; } if (command == NIM_MODIFY && (data->uFlags & NIF_INFO)) ++s.startupNotifications; return TRUE; },
            [](void* p,HINSTANCE,int)->HICON { ++static_cast<Resident*>(p)->icons; return reinterpret_cast<HICON>(1); },
            [](void* p,HICON) { ++static_cast<Resident*>(p)->destroyedIcons; },true};
    }
    state.tray = std::make_unique<ResidentTray>(environment,state.window,instance,trayApi); state.RefreshTray();
    state.store = std::make_unique<ConfigStore>(std::make_unique<NativeConfigFiles>(environment.dataDirectory)); state.store->Load();
    const auto configuration = state.store->Current();
    Capabilities nativeCaps; const auto queried = options.smoke ? state.simulation.RuntimeApi().engine.capabilities(&nativeCaps) : WmfPlatformCapabilities(&nativeCaps);
    if (!options.updateTrial.empty() && (!queried || configuration->recoveryRequired)) throw std::runtime_error("Updated Host did not pass configuration/module health checks");
    state.evidence = std::make_unique<CapabilityEvidence>(environment,nativeCaps);
    const auto localEvidence = queried && !options.smoke ? state.evidence->Flags() : 0;
    const auto captureFlags = queried ? ReleasedCaptureFlags(nativeCaps,localEvidence) : 0;
    const auto runtimeApi = options.smoke ? state.simulation.RuntimeApi() : ProductRuntimeApi{};
    state.runtime = std::make_unique<ProductRuntime>(environment,configuration->config.input,false,runtimeApi,captureFlags);
    state.startup = std::make_unique<StartupRegistration>(environment,environment.executableDirectory/L"WinMouseFix.Host.exe",options.smoke ? state.simulation.Startup() : RealStartupApi());
    state.log = std::make_unique<ProductLog>(environment.dataDirectory/L"logs",*state.runtime,configuration->config.input.qpcFrequency,options.smoke);
    ProductServiceApi api; api.simulation = options.smoke; api.validatedFlags = captureFlags; if (options.smoke) api.capabilities = runtimeApi.engine.capabilities;
    api.fullscreenDetected = [&] { return state.fullscreenDetected.load(); };
    state.updates = std::make_unique<UpdateService>(options.smoke,environment.dataDirectory/L"update-reminder.txt",options.smoke ? UpdateFetch{[](unsigned,std::stop_token) { return std::string("[]"); }} : UpdateFetch{},options.smoke ? std::filesystem::path{} : environment.executableDirectory);
    api.updateStatus = [&] { return state.updates->StatusJson(); }; api.checkUpdates = [&] { state.updates->Request(GetTickCount64()); };
    api.acknowledgeUpdate = [&](std::uint64_t id) { state.updates->Acknowledge(id); };
    api.downloadUpdate = [&](std::uint64_t id) { return state.updates->Download(id); }; api.cancelUpdate = [&] { return state.updates->CancelDownload(); };
    api.installUpdate = [&] { return state.updates->RequestInstall(); }; api.updateBusy = [&] { return state.updates->Busy(); };
    api.switchDataLocation = [&](DataLocation target) -> Result { return PostMessageW(state.window,SwitchDataMessage,static_cast<WPARAM>(target),0) ? Result{} : Result{Status::NativeFailure,GetLastError()}; };
    api.openSettings = [&] { PostMessageW(state.window,OpenSettingsMessage,0,0); }; api.requestExit = [&] { state.closing = true; };
    api.settingsWindow = [&](DWORD pid,std::uint64_t window) {
        std::lock_guard lock(state.settingsMutex); state.settingsPid = pid; state.settingsWindow = reinterpret_cast<HWND>(static_cast<std::uintptr_t>(window));
        if (!options.updateTrial.empty()) wmf::update::WriteText(options.updateTrial/L"settings.pid",std::to_string(pid));
    };
    api.settingsReady = [&](DWORD pid) {
        std::lock_guard lock(state.settingsMutex);
        if (!options.updateTrial.empty() && pid == state.settingsPid && state.settingsWindow && IsWindowVisible(state.settingsWindow)) wmf::update::WriteText(options.updateTrial/L"health.ready",ProductVersion);
    };
    api.isSettingsProcess = [&](DWORD pid) { return options.smoke || ProcessImageMatches(pid,SettingsExecutable(environment.executableDirectory)); };
    api.logError = [&] { return state.log->Error(); }; api.startTrace = [&] { state.log->DetailFor(); };
    api.exportDiagnostics = [&] { return state.log->Export(state.service->StatusJson(),SerializeConfig(state.store->Current()->config)); };
    api.observe = [&](std::string_view command,const ProtocolReply& reply) { if (command != "subscribe" && command != "get-state" && command != "get-config" && command != "hello") state.log->Record(std::string(command),reply.code,reply.revision); };
    api.shellObservations = [&](const BindingConfig& config) { return state.evidence->ShellObservations(config); };
    api.recordShell = [&](const BindingConfig& config,std::string_view payload) { state.evidence->RecordShell(config,payload); };
    state.service = std::make_unique<ProductService>(environment,*state.store,*state.runtime,*state.startup,api);
    if (!state.service->Start()) throw std::runtime_error("Cannot start local settings control pipe");
    if (!options.smoke) {
        if (!state.fullscreenMonitor.Start(state.window,FullscreenMessage)) state.log->Record("fullscreen-monitor","Window event subscription incomplete; maintenance sampling remains active");
        state.RefreshFullscreen();
    }
    state.log->Record("startup",state.service->StatusJson(),configuration->config.input.revision);
    if (options.smoke) { std::filesystem::create_directories(options.output); std::ofstream(options.output/L"ready.json") << "{\"simulation\":true,\"ready\":true}"; }
    if (!SetTimer(state.window,1,options.smoke ? 50 : 200,nullptr)) throw std::runtime_error("Cannot start resident status timer");
    state.log->Record("startup","WinMouseFix "+std::string(ProductVersion)+"; x64; SDK 10.0.28000.0; simulation="+std::to_string(options.smoke),configuration->config.input.revision);
    if (options.trace) state.log->DetailFor();
    bool limited{};
    for (unsigned i = 0; i < configuration->config.input.bindingCount; ++i)
        if (configuration->config.input.bindings[i].action == Action::NativeZoom && !(captureFlags & NativeZoomValidated)) limited = true;
    if (limited) state.log->Record("validation-expired","Unverified pinch actions are inactive; other bindings remain available");
    state.updatePermitted = !configuration->recoveryRequired && !state.lastTip.empty();
    if (!options.smoke && state.updatePermitted && options.updateTrial.empty()) state.runtime->Enable(configuration->config.enabled);
    if (!configuration->recoveryRequired && !state.lastTip.empty() && options.updateTrial.empty()) {
        const auto notice = state.tray->NotifyStartup(configuration->config.startupNotification);
        if (!notice) state.log->Record("startup-notification-error",JsonResult(notice),configuration->config.input.revision);
    }
    if (options.settings || !options.updateTrial.empty()) state.OpenSettings();
    MSG message{}; int result{}; while ((result = GetMessageW(&message,nullptr,0,0)) > 0) { if (!IsDialogMessageW(state.window,&message)) { TranslateMessage(&message); DispatchMessageW(&message); } }
    state.service->Stop(); state.tray->Remove();
    if (state.runtime->RequiresProcessExit() && !options.smoke) {
        const auto failure = state.runtime->Failure();
        state.log->Record("fatal-cleanup","Native capture teardown failed permanently; exiting without unloading callback modules");
        state.log.reset(); // Flush the bounded error log before OS resource teardown.
        ExitProcess(failure.nativeError ? failure.nativeError : ERROR_GEN_FAILURE);
    }
    if (options.smoke) {
        const auto expectedNotices = configuration->config.startupNotification && !configuration->recoveryRequired ? 1u : 0u;
        std::filesystem::create_directories(options.output); const bool ok = state.firstPaused && state.rebuildPreserved && state.adds == 2 && state.icons == state.destroyedIcons && state.settingsOpened >= 1 && state.runtime->Finished() && static_cast<bool>(state.runtime->Failure()) && state.startupNotifications == expectedNotices;
        std::ofstream out(options.output/L"report.json"); out << "{\"simulation\":true,\"ok\":" << (ok ? "true" : "false") << ",\"firstPaused\":" << (state.firstPaused ? "true" : "false")
            << ",\"trayRecreatedWithoutEngineRestart\":" << (state.rebuildPreserved ? "true" : "false") << ",\"iconHandlesBalanced\":" << (state.icons == state.destroyedIcons ? "true" : "false")
            << ",\"fakeFrames\":" << state.simulation.frames << ",\"captures\":" << state.simulation.captures << ",\"settingsActivations\":" << state.settingsOpened << ",\"startupNotificationRequests\":" << state.startupNotifications << ",\"realCapture\":false,\"desktopInjection\":false,\"realStartupRegistry\":false}";
        return ok ? 0 : 1;
    }
    return result < 0 ? 1 : 0;
}
}
bool TryRunResidentMode(HINSTANCE instance,int& result) {
    int count{}; auto raw = CommandLineToArgvW(GetCommandLineW(),&count); if (!raw) throw std::runtime_error("Cannot read command line");
    std::vector<std::wstring> arguments; for (int i = 1; i < count; ++i) arguments.emplace_back(raw[i]); LocalFree(raw);
    for (const auto& argument : arguments) if (argument == L"--diagnostic" || argument == L"--probe" || argument == L"--probe-device" || argument == L"--smoke" || argument == L"--simulation") return false;
    Options options;
    for (std::size_t i = 0; i < arguments.size(); ++i) {
        const auto& argument = arguments[i];
        if (argument == L"--settings") options.settings = true; else if (argument == L"--startup") options.startup = true;
        else if (argument == L"--update-verify" && i+1 < arguments.size()) options.updateTrial = std::filesystem::absolute(arguments[++i]);
        else if (argument == L"--resident-smoke") options.smoke = true; else if (argument == L"--trace") options.trace = true;
        else if (argument == L"--resident") {} else if (argument == L"--output" && i+1 < arguments.size()) options.output = std::filesystem::absolute(arguments[++i]);
        else if (argument == L"--restart-after" && i+1 < arguments.size()) { options.restartAfter = static_cast<DWORD>(std::stoul(arguments[++i])); if (!options.restartAfter || options.restartAfter == GetCurrentProcessId()) throw std::runtime_error("Invalid restart parent"); }
        else if (argument == L"--restart-settings" && i+1 < arguments.size()) options.restartSettings = static_cast<DWORD>(std::stoul(arguments[++i]));
        else if ((argument == L"--storage-from" || argument == L"--storage-to") && i+1 < arguments.size()) {
            const auto mode = arguments[++i]; if (mode != L"appdata" && mode != L"program") throw std::runtime_error("Invalid storage location");
            const auto location = mode == L"program" ? DataLocation::Program : DataLocation::AppData;
            if (argument == L"--storage-from") options.migrationFrom = location; else { options.migrationTo = location; options.migrate = true; }
        }
        else if (argument == L"--simulation-scope" && i+1 < arguments.size()) options.simulationTag = arguments[++i];
        else if (argument == L"--smoke-hold-ms" && i+1 < arguments.size()) { options.smokeHoldMs = static_cast<unsigned>(std::stoul(arguments[++i])); if (options.smokeHoldMs > 8000) throw std::runtime_error("Smoke hold must be <= 8000 ms"); }
        else throw std::runtime_error("Unknown resident argument");
    }
    if (options.smoke && options.output.empty()) throw std::runtime_error("Resident smoke requires an explicit isolated output directory");
    if (!options.updateTrial.empty() && (options.smoke || options.migrate || options.restartAfter)) throw std::runtime_error("Invalid update verification arguments");
    if (!options.smoke && !options.output.empty()) throw std::runtime_error("--output is only supported by isolated resident smoke");
    if (!options.smoke && (!options.simulationTag.empty() || options.smokeHoldMs)) throw std::runtime_error("Shared simulation arguments require --resident-smoke");
    if (options.migrate && (!options.restartAfter || options.smoke || options.migrationFrom == options.migrationTo)) throw std::runtime_error("Storage relocation requires an explicit production restart");
    if (options.restartSettings && !options.migrate) throw std::runtime_error("Settings handoff requires a storage relocation");
    if (options.restartAfter) {
        const auto environment = ProductEnvironment::Production(false);
        HANDLE parent = OpenProcess(SYNCHRONIZE|PROCESS_QUERY_LIMITED_INFORMATION,FALSE,options.restartAfter);
        if (parent) {
            const bool matches = ProcessImageMatches(options.restartAfter,environment.executableDirectory/L"WinMouseFix.Host.exe");
            const auto waited = matches ? WaitForSingleObject(parent,30000) : WAIT_FAILED; CloseHandle(parent);
            if (!matches || waited != WAIT_OBJECT_0) throw std::runtime_error("Previous Host did not exit safely; relocation was not started");
        } else if (GetLastError() != ERROR_INVALID_PARAMETER) throw std::runtime_error("Cannot verify the previous Host");
        if (options.restartSettings) {
            HANDLE settings = OpenProcess(SYNCHRONIZE|PROCESS_QUERY_LIMITED_INFORMATION,FALSE,options.restartSettings);
            if (settings) {
                const bool matches = ProcessImageMatches(options.restartSettings,SettingsExecutable(environment.executableDirectory));
                const auto waited = matches ? WaitForSingleObject(settings,30000) : WAIT_FAILED; CloseHandle(settings);
                if (!matches || waited != WAIT_OBJECT_0) throw std::runtime_error("Settings did not finish saving; relocation was not started");
            } else if (GetLastError() != ERROR_INVALID_PARAMETER) throw std::runtime_error("Cannot verify Settings shutdown");
        }
    }
    result = RunResident(instance,options); return true;
}
}

