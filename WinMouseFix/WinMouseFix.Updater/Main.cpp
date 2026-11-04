#include "../Shared/Update/UpdatePackage.h"
#include "../WinMouseFix.Host/JsonSupport.h"
#include <Windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <thread>
#include <atomic>
#include <stdexcept>
#include <vector>

namespace {
namespace fs = std::filesystem;
using namespace wmf::update;
constexpr UINT ProgressMessage = WM_APP+1,DoneMessage = WM_APP+2;
HWND window{},label{},bar{};
std::string failure; std::atomic<bool> successful{};
bool quietTest{};
struct Handle { HANDLE value{}; ~Handle() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); } };
void Require(bool ok,const char* text) { if (!ok) throw std::runtime_error(text); }
bool SameImage(HANDLE process,const fs::path& expected) {
    wchar_t path[32768]{}; DWORD length = 32768; return QueryFullProcessImageNameW(process,0,path,&length) && _wcsicmp(fs::weakly_canonical(path).c_str(),fs::weakly_canonical(expected).c_str()) == 0;
}
HANDLE OpenParent(DWORD pid,const fs::path& image) {
    if (!pid) return nullptr; HANDLE process = OpenProcess(SYNCHRONIZE|PROCESS_QUERY_LIMITED_INFORMATION,FALSE,pid);
    if (!process) { Require(GetLastError() == ERROR_INVALID_PARAMETER,"cannot-open-process"); return nullptr; }
    if (!SameImage(process,image)) { CloseHandle(process); throw std::runtime_error("process-image-mismatch"); } return process;
}
void Wait(HANDLE process) { if (process) Require(WaitForSingleObject(process,30000) == WAIT_OBJECT_0,"process-exit-timeout"); }
PROCESS_INFORMATION Launch(const fs::path& root,std::wstring arguments,HANDLE job=nullptr) {
    auto exe = root/L"WinMouseFix.Host.exe"; auto command = L"\""+exe.wstring()+L"\" "+arguments; STARTUPINFOW info{sizeof(info)}; PROCESS_INFORMATION process{};
    Require(CreateProcessW(exe.c_str(),command.data(),nullptr,nullptr,FALSE,job ? CREATE_SUSPENDED : 0,nullptr,root.c_str(),&info,&process),"restart-failed");
    if (job && (!AssignProcessToJobObject(job,process.hProcess) || ResumeThread(process.hThread)==static_cast<DWORD>(-1))) { TerminateProcess(process.hProcess,1); WaitForSingleObject(process.hProcess,5000); CloseHandle(process.hThread); CloseHandle(process.hProcess); throw std::runtime_error("trial-job-failed"); }
    CloseHandle(process.hThread); process.hThread = nullptr; return process;
}
void StopNew(const fs::path& tx,PROCESS_INFORMATION& child,HANDLE job) {
    // Ask both processes to close first; terminate only the verified update trial if it hangs.
    try { WriteText(tx/L"abort", "abort"); } catch (...) {} // Still stop the owned trial if the disk is full.
    if (child.hProcess) WaitForSingleObject(child.hProcess,8000);
    // This job contains only the update trial Host and its children, including a
    // Settings process that has not yet reached its IPC handshake.
    if (job) {
        Require(TerminateJobObject(job,1),"trial-shutdown-failed");
        JOBOBJECT_BASIC_ACCOUNTING_INFORMATION accounting{}; const auto deadline=GetTickCount64()+8000;
        do { Require(QueryInformationJobObject(job,JobObjectBasicAccountingInformation,&accounting,sizeof(accounting),nullptr),"trial-shutdown-failed"); if (!accounting.ActiveProcesses) break; Require(GetTickCount64()<deadline,"trial-shutdown-failed"); Sleep(20); } while (true);
    }
    if (child.hProcess && WaitForSingleObject(child.hProcess,8000) != WAIT_OBJECT_0) { TerminateProcess(child.hProcess,1); WaitForSingleObject(child.hProcess,8000); }
    if (fs::exists(tx/L"settings.pid")) {
        DWORD pid = static_cast<DWORD>(std::stoul(ReadText(tx/L"settings.pid",32))); auto root = tx.parent_path(); auto exe = root/L"Settings/WinMouseFix.Settings.exe";
        HANDLE process = OpenProcess(SYNCHRONIZE|PROCESS_QUERY_LIMITED_INFORMATION|PROCESS_TERMINATE,FALSE,pid);
        if (process) { if (SameImage(process,exe) && WaitForSingleObject(process,5000) != WAIT_OBJECT_0) { TerminateProcess(process,1); WaitForSingleObject(process,5000); } CloseHandle(process); }
    }
}
void Run(const fs::path& tx,DWORD hostPid,DWORD settingsPid,bool recover) {
    CheckTransactionPath(tx); const auto root = tx.parent_path(); CheckTree(root);
    Handle exclusive{CreateFileW((root/L".wmf-update.lock").c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,OPEN_ALWAYS,FILE_ATTRIBUTE_HIDDEN,nullptr)}; Require(exclusive.value != INVALID_HANDLE_VALUE,"update-already-running");
    Handle host{OpenParent(hostPid,root/L"WinMouseFix.Host.exe")};
    Handle settings{OpenParent(settingsPid,root/L"Settings/WinMouseFix.Settings.exe")};
    if (!recover) {
        auto manifest = ReadManifest(tx/L"payload"); VerifyPackage(tx/L"payload",manifest.version);
        Require(fs::exists(root/L"manifest.json") && !TransactionPending(root),"invalid-installation");
    } else Require(TransactionPending(root) && fs::equivalent(PendingTransaction(root),tx),"invalid-recovery");
    WriteText(tx/L"handoff.ready","ready"); Wait(host.value); Wait(settings.value);
    Require(!fs::exists(tx/L"handoff.cancel"),"handoff-cancelled");
    if (recover) { PostMessageW(window,ProgressMessage,3,0); RollbackTransaction(tx); auto child = Launch(root,L"--settings"); CloseHandle(child.hProcess); successful = true; return; }
    bool prepared{}; PROCESS_INFORMATION child{}; Handle trialJob;
    try {
        PostMessageW(window,ProgressMessage,1,0);
        // Host writes the selected data root before handing off, after saving defaults/configuration.
        const fs::path data{winrt::to_hstring(ReadText(tx/L"data-directory.txt",32768)).c_str()};
        PrepareTransaction(tx,data); prepared = true;
        ApplyTransaction(tx,[](auto done,auto total) { PostMessageW(window,ProgressMessage,1,total ? static_cast<LPARAM>(done*100/total) : 0); });
        PostMessageW(window,ProgressMessage,2,100);
        trialJob.value = CreateJobObjectW(nullptr,nullptr); Require(trialJob.value != nullptr,"trial-job-failed");
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{}; limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE; Require(SetInformationJobObject(trialJob.value,JobObjectExtendedLimitInformation,&limits,sizeof(limits)),"trial-job-failed");
        child = Launch(root,L"--update-verify \""+tx.wstring()+L"\"",trialJob.value);
        const auto deadline = GetTickCount64()+45000;
        while (!fs::exists(tx/L"health.ready")) { Require(WaitForSingleObject(child.hProcess,100) == WAIT_TIMEOUT,"new-version-exited"); Require(GetTickCount64() < deadline,"new-version-timeout"); }
        Require(ReadText(tx/L"health.ready",128) == ReadManifest(root).version,"health-version-mismatch");
        Require(WaitForSingleObject(child.hProcess,0)==WAIT_TIMEOUT,"new-version-exited");
        CommitTransaction(tx);
        limits.BasicLimitInformation.LimitFlags = 0; Require(SetInformationJobObject(trialJob.value,JobObjectExtendedLimitInformation,&limits,sizeof(limits)),"trial-job-failed");
        try { WriteText(tx/L"commit.ok","ok"); } catch (...) {} // The committed journal is also the Host's durable signal.
        successful = true;
        CloseHandle(child.hProcess); child.hProcess = nullptr;
        // Executing worker.exe remains until a later launch can clean the completed transaction.
        std::error_code ignored; fs::remove_all(tx/L"backup",ignored); fs::remove_all(tx/L"payload",ignored); fs::remove_all(tx/L"configuration",ignored); fs::remove(tx/L"package.zip",ignored);
    } catch (...) {
        const auto original = std::current_exception();
        if (child.hProcess) { StopNew(tx,child,trialJob.value); CloseHandle(child.hProcess); }
        if (prepared) { PostMessageW(window,ProgressMessage,3,0); RollbackTransaction(tx); }
        auto old = Launch(root,L"--settings"); CloseHandle(old.hProcess); std::rethrow_exception(original);
    }
}
LRESULT CALLBACK Procedure(HWND hwnd,UINT message,WPARAM a,LPARAM b) {
    if (message == WM_CLOSE) return 0;
    if (message == ProgressMessage) { SetWindowTextW(label,a == 1 ? L"正在安装更新，请稍候…" : a == 2 ? L"正在启动新版本…" : L"正在恢复上一版本…"); SendMessageW(bar,PBM_SETPOS,b,0); return 0; }
    if (message == DoneMessage) { DestroyWindow(hwnd); return 0; }
    if (message == WM_DESTROY) { PostQuitMessage(0); return 0; }
    return DefWindowProcW(hwnd,message,a,b);
}
}
int WINAPI wWinMain(HINSTANCE instance,HINSTANCE,PWSTR,int) {
    try {
        int count{}; auto raw = CommandLineToArgvW(GetCommandLineW(),&count); Require(raw != nullptr,"invalid-arguments"); std::vector<std::wstring> args; for (int i=1;i<count;++i) args.emplace_back(raw[i]); LocalFree(raw);
        fs::path tx; DWORD host{},settings{}; bool recover{};
        for (std::size_t i=0;i<args.size();++i) { if ((args[i] == L"--transaction" || args[i] == L"--recover") && i+1<args.size()) { recover = args[i] == L"--recover"; tx = args[++i]; } else if (args[i] == L"--host-pid" && i+1<args.size()) host = std::stoul(args[++i]); else if (args[i] == L"--settings-pid" && i+1<args.size()) settings = std::stoul(args[++i]); else if (args[i] == L"--test-report") quietTest = true; else throw std::runtime_error("invalid-arguments"); }
        CheckTransactionPath(tx);
        if (quietTest) Require(fs::exists(tx.parent_path()/L".wmf-test-updater"),"test-mode-requires-isolated-fixture");
        wchar_t own[32768]{}; Require(GetModuleFileNameW(nullptr,own,32768) > 0,"invalid-executable"); Require(fs::equivalent(fs::path(own).parent_path(),tx),"invalid-worker-location");
        INITCOMMONCONTROLSEX controls{sizeof(controls),ICC_PROGRESS_CLASS}; InitCommonControlsEx(&controls);
        WNDCLASSW cls{}; cls.hInstance=instance; cls.lpfnWndProc=Procedure; cls.lpszClassName=L"WinMouseFix.Updater"; cls.hCursor=LoadCursorW(nullptr,IDC_ARROW); cls.hbrBackground=reinterpret_cast<HBRUSH>(COLOR_WINDOW+1); RegisterClassW(&cls);
        auto icon = static_cast<HICON>(LoadImageW(instance,MAKEINTRESOURCEW(107),IMAGE_ICON,0,0,LR_DEFAULTSIZE)); cls.hIcon=icon;
        window=CreateWindowExW(WS_EX_DLGMODALFRAME,cls.lpszClassName,L"Win Mouse Fix 更新",WS_CAPTION|WS_SYSMENU,(GetSystemMetrics(SM_CXSCREEN)-460)/2,(GetSystemMetrics(SM_CYSCREEN)-160)/2,460,160,nullptr,nullptr,instance,nullptr);
        Require(window != nullptr,"cannot-create-window"); if (icon) SendMessageW(window,WM_SETICON,ICON_BIG,reinterpret_cast<LPARAM>(icon));
        label=CreateWindowW(L"STATIC",L"正在准备更新…",WS_CHILD|WS_VISIBLE,24,24,400,28,window,nullptr,instance,nullptr);
        SendMessageW(label,WM_SETFONT,reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT)),TRUE);
        bar=CreateWindowW(PROGRESS_CLASSW,nullptr,WS_CHILD|WS_VISIBLE,24,68,400,20,window,nullptr,instance,nullptr); if (!quietTest) ShowWindow(window,SW_SHOWNORMAL);
        std::jthread worker([&] { try { Run(tx,host,settings,recover); } catch (const std::exception& e) { failure=e.what(); try { WriteText(tx/L"handoff.error",failure); } catch (...) {} } catch (...) { failure="unexpected-update-error"; } PostMessageW(window,DoneMessage,0,0); });
        MSG message{}; while (GetMessageW(&message,nullptr,0,0)>0) { TranslateMessage(&message); DispatchMessageW(&message); } worker.join(); if (icon) DestroyIcon(icon);
        if (!successful) { auto text=L"更新未完成。原版本和配置已尽可能保留；请重试或从发布页手动更新。\n\n"+std::wstring(winrt::to_hstring(failure)); if (!quietTest) MessageBoxW(nullptr,text.c_str(),L"Win Mouse Fix",MB_OK|MB_ICONWARNING); return 1; } return 0;
    } catch (const std::exception& e) { MessageBoxW(nullptr,winrt::to_hstring(e.what()).c_str(),L"Win Mouse Fix 更新失败",MB_OK|MB_ICONERROR); return 1; }
}
