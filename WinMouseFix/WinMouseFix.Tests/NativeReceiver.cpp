#include <SDKDDKVer.h>
#ifndef NTDDI_VERSION
#define NTDDI_VERSION NTDDI_WIN11_GE
#endif
#include <Windows.h>
#include <shellapi.h>
#include <interactioncontext.h>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>
#include <cmath>
#include <algorithm>

namespace {
using ProcessFramesFn = HRESULT(WINAPI*)(HINTERACTIONCONTEXT,UINT32,UINT32,const POINTER_TYPE_INFO*);
constexpr std::size_t HistoryCapacity = 4096, RecordCapacity = 65536;
std::int64_t Now() { LARGE_INTEGER t{}; QueryPerformanceCounter(&t); return t.QuadPart; }
struct RawRecord { std::int64_t received{}; POINTER_INFO info{}; };
struct InteractionRecord { std::int64_t received{}; unsigned stage{},flags{}; float dx{},dy{},x{},y{},deltaScale{1},scale{1}; };
struct Receiver {
    decltype(&RegisterTouchpadCapableWindow) registerWindow{};
    decltype(&GetPointerFrameTouchpadInfoHistory) history{};
    ProcessFramesFn processFrames{};
    HINTERACTIONCONTEXT context{};
    HRESULT contextResult{E_PENDING}, processResult{E_PENDING};
    HWND window{};
    bool registered{}, processing{}, closed{}, writeFailed{}, documentedOrdinal{}, simulation{};
    DWORD registrationError{}, unregistrationError{}, receiveError{}, pointerInfoError{};
    unsigned callbacks{}, active{}, lost{}, wheelMessages{}, capture{1}, pointerMessages{}, pointerInfoFailures{}, otherPointerMessages{};
    std::vector<POINTER_TOUCH_INFO> touches{HistoryCapacity};
    std::vector<POINTER_TYPE_INFO> types{HistoryCapacity};
    std::vector<RawRecord> raw;
    std::vector<InteractionRecord> interactions;
    std::filesystem::path directory;
    std::wstring notice;
    float cumulativeX{}, cumulativeY{},cumulativeScale{1},minimumScale{1},maximumScale{1};
    Receiver(const std::filesystem::path& root) {
        directory = std::filesystem::absolute(root) / (std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(Now()));
        std::filesystem::create_directories(directory); raw.reserve(RecordCapacity); interactions.reserve(RecordCapacity);
        const auto user = GetModuleHandleW(L"user32.dll");
        const auto input = GetModuleHandleW(L"ninput.dll");
#pragma warning(push)
#pragma warning(disable: 4191)
        registerWindow = reinterpret_cast<decltype(registerWindow)>(GetProcAddress(user,"RegisterTouchpadCapableWindow"));
        history = reinterpret_cast<decltype(history)>(GetProcAddress(user,"GetPointerFrameTouchpadInfoHistory"));
        // Public Microsoft signature; this SDK does not yet declare the extended function.
        processFrames = reinterpret_cast<ProcessFramesFn>(GetProcAddress(input,"ProcessPointerFramesInteractionContext2"));
        // Publicly documented ordinal and signature, not a guessed private entry:
        // https://learn.microsoft.com/windows/win32/input-precisiontouchpad/processpointerframesinteractioncontext2
        if (!processFrames) {
            processFrames = reinterpret_cast<ProcessFramesFn>(GetProcAddress(input,MAKEINTRESOURCEA(2507)));
            documentedOrdinal = processFrames != nullptr;
        }
#pragma warning(pop)
        contextResult = InitializeContext();
    }
    ~Receiver() { CloseContext(); }
    HRESULT InitializeContext() {
        if (!processFrames) return HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND);
        auto hr = CreateInteractionContext(&context);
        if (FAILED(hr)) return hr;
        hr = RegisterOutputCallbackInteractionContext(context,Output,this);
        if (SUCCEEDED(hr)) hr = SetPropertyInteractionContext(context,INTERACTION_CONTEXT_PROPERTY_MEASUREMENT_UNITS,0);
        if (SUCCEEDED(hr)) hr = SetPropertyInteractionContext(context,INTERACTION_CONTEXT_PROPERTY_FILTER_POINTERS,FALSE);
        INTERACTION_CONTEXT_CONFIGURATION config{INTERACTION_ID_MANIPULATION,
            static_cast<INTERACTION_CONFIGURATION_FLAGS>(INTERACTION_CONFIGURATION_FLAG_MANIPULATION |
            INTERACTION_CONFIGURATION_FLAG_MANIPULATION_TRANSLATION_X | INTERACTION_CONFIGURATION_FLAG_MANIPULATION_TRANSLATION_Y |
            INTERACTION_CONFIGURATION_FLAG_MANIPULATION_SCALING)};
        if (SUCCEEDED(hr)) hr = SetInteractionConfigurationInteractionContext(context,1,&config);
        return hr;
    }
    static void CALLBACK Output(void* state,const INTERACTION_CONTEXT_OUTPUT* output) {
        auto& self = *static_cast<Receiver*>(state);
        if (output->interactionId != INTERACTION_ID_MANIPULATION) return;
        const auto& m = output->arguments.manipulation;
        if (!std::isfinite(m.delta.translationX) || !std::isfinite(m.delta.translationY) ||
            !std::isfinite(m.cumulative.translationX) || !std::isfinite(m.cumulative.translationY) ||
            !std::isfinite(m.delta.scale) || !std::isfinite(m.cumulative.scale) || m.delta.scale <= 0 || m.cumulative.scale <= 0) { ++self.lost; return; }
        if (self.interactions.size() < RecordCapacity)
            self.interactions.push_back({Now(),self.processing ? 0u : 1u,static_cast<unsigned>(output->interactionFlags),
                m.delta.translationX,m.delta.translationY,m.cumulative.translationX,m.cumulative.translationY,m.delta.scale,m.cumulative.scale});
        else ++self.lost;
        self.cumulativeX = m.cumulative.translationX; self.cumulativeY = m.cumulative.translationY;
        self.cumulativeScale = m.cumulative.scale;
        self.minimumScale = (std::min)(self.minimumScale,m.cumulative.scale); self.maximumScale = (std::max)(self.maximumScale,m.cumulative.scale);
    }
    void CloseContext() {
        if (context) { DestroyInteractionContext(context); context = nullptr; }
    }
    void Attach(HWND h) {
        window = h;
        registered = registerWindow && history && registerWindow(h,TRUE);
        if (!registered) registrationError = registerWindow && history ? GetLastError() : ERROR_PROC_NOT_FOUND;
        CreateWindowW(L"BUTTON",L"清空记录",WS_VISIBLE|WS_CHILD|WS_TABSTOP,20,18,140,36,h,reinterpret_cast<HMENU>(1),nullptr,nullptr);
        CreateWindowW(L"BUTTON",L"保存记录",WS_VISIBLE|WS_CHILD|WS_TABSTOP,180,18,140,36,h,reinterpret_cast<HMENU>(2),nullptr,nullptr);
        SetTimer(h,1,250,nullptr);
    }
    bool Receive(WPARAM w) {
        return ReadPointer(GET_POINTERID_WPARAM(w));
    }
    bool ReadPointer(UINT32 id) {
        ++pointerMessages;
        POINTER_INFO info{};
        if (!GetPointerInfo(id,&info)) { ++pointerInfoFailures; pointerInfoError = GetLastError(); return false; }
        if (info.pointerType != PT_TOUCHPAD) { ++otherPointerMessages; return false; }
        ++callbacks;
        UINT32 frames{}, pointers{};
        if (!history(id,&frames,&pointers,nullptr)) { receiveError = GetLastError(); SkipPointerFrameMessages(id); return true; }
        if (!pointers || !frames || frames > HistoryCapacity/pointers) { ++lost; SkipPointerFrameMessages(id); return true; }
        if (!history(id,&frames,&pointers,touches.data())) { receiveError = GetLastError(); SkipPointerFrameMessages(id); return true; }
        const auto received = Now();
        active = 0;
        for (UINT32 i = 0; i < pointers; ++i) if (touches[i].pointerInfo.pointerFlags & POINTER_FLAG_INCONTACT) ++active;
        for (UINT32 i = 0; i < frames*pointers; ++i) {
            if (raw.size() < RecordCapacity) raw.push_back({received,touches[i].pointerInfo}); else ++lost;
            types[i] = {}; types[i].type = PT_TOUCHPAD; types[i].touchInfo = touches[i];
        }
        if (SUCCEEDED(contextResult) && context) {
            processing = true; processResult = processFrames(context,frames,pointers,types.data()); processing = false;
        }
        if (!SkipPointerFrameMessages(id)) receiveError = GetLastError();
        return true;
    }
    void Clear() {
        if (active) { notice = L"请先结束接触，再清空记录。"; return; }
        CloseContext(); Save();
        if (writeFailed) return;
        ++capture; raw.clear(); interactions.clear(); callbacks = lost = wheelMessages = pointerMessages = pointerInfoFailures = otherPointerMessages = 0;
        receiveError = pointerInfoError = 0; cumulativeX = cumulativeY = 0; cumulativeScale = minimumScale = maximumScale = 1; processResult = E_PENDING;
        contextResult = InitializeContext(); notice = L"已清空。请在 Host 准备独立 1 mm 实验，再激活本窗口。";
    }
    void Save() {
        try {
            const auto captureDirectory = directory/(L"capture-"+std::to_wstring(capture));
            std::filesystem::create_directories(captureDirectory);
            std::ofstream csv(captureDirectory/L"contacts.csv",std::ios::binary|std::ios::trunc);
            csv.exceptions(std::ios::failbit|std::ios::badbit);
            csv << "received_qpc,frame_id,pointer_id,flags,source_performance_count,source_dwtime,x_himetric,y_himetric\n";
            for (const auto& r : raw) csv << r.received << ',' << r.info.frameId << ',' << r.info.pointerId << ',' << r.info.pointerFlags << ',' <<
                r.info.PerformanceCount << ',' << r.info.dwTime << ',' << r.info.ptHimetricLocation.x << ',' << r.info.ptHimetricLocation.y << '\n';
            csv.close();
            std::ofstream ic(captureDirectory/L"interactions.csv",std::ios::binary|std::ios::trunc);
            ic.exceptions(std::ios::failbit|std::ios::badbit);
            ic << "received_qpc,stage,flags,delta_x_himetric,delta_y_himetric,cumulative_x_himetric,cumulative_y_himetric,delta_scale,cumulative_scale\n" << std::setprecision(9);
            for (const auto& r : interactions) ic << r.received << ',' << r.stage << ',' << r.flags << ',' << r.dx << ',' << r.dy << ',' << r.x << ',' << r.y << ',' << r.deltaScale << ',' << r.scale << '\n';
            ic.close();
            std::ofstream json(captureDirectory/L"receiver.json",std::ios::binary|std::ios::trunc);
            json.exceptions(std::ios::failbit|std::ios::badbit);
            LARGE_INTEGER frequency{}; QueryPerformanceFrequency(&frequency);
            json << "{\"schemaVersion\":1,\"kind\":\"native-receiver\",\"desktopContactInjection\":false,\"sdk\":\"" << WMF_SDK_VERSION <<
                "\",\"registerApi\":" << (registerWindow ? "true" : "false") << ",\"historyApi\":" << (history ? "true" : "false") <<
                ",\"processExtendedApi\":" << (processFrames ? "true" : "false") << ",\"registrationAttempted\":" << (window ? "true" : "false") <<
                ",\"processResolution\":\"" << (documentedOrdinal ? "documented-ordinal-2507" : processFrames ? "name" : "unavailable") << "\"" <<
                ",\"registered\":" << (registered ? "true" : "false") << ",\"registrationError\":" << registrationError <<
                ",\"unregistrationError\":" << unregistrationError <<
                ",\"contextHresult\":" << static_cast<std::uint32_t>(contextResult) << ",\"processHresult\":" << static_cast<std::uint32_t>(processResult) <<
                ",\"callbacks\":" << callbacks << ",\"contactRecords\":" << raw.size() << ",\"interactionOutputs\":" << interactions.size() <<
                ",\"simulation\":" << (simulation ? "true" : "false") << ",\"pointerMessages\":" << pointerMessages <<
                ",\"pointerMessageScope\":\"WM_POINTERDOWN/UPDATE/UP data messages\"" <<
                ",\"pointerInfoFailures\":" << pointerInfoFailures << ",\"pointerInfoError\":" << pointerInfoError <<
                ",\"otherPointerMessages\":" << otherPointerMessages <<
                ",\"capture\":" << capture <<
                ",\"activeContacts\":" << active << ",\"receiveError\":" << receiveError << ",\"recordsLost\":" << lost <<
                ",\"wheelMessages\":" << wheelMessages << ",\"cumulativeXHimetric\":" << cumulativeX << ",\"cumulativeYHimetric\":" << cumulativeY << ",\"cumulativeScale\":" << cumulativeScale << ",\"minimumScale\":" << minimumScale << ",\"maximumScale\":" << maximumScale <<
                ",\"qpcFrequency\":" << frequency.QuadPart << ",\"receiverClosed\":" << (closed ? "true" : "false") <<
                ",\"verdict\":\"inconclusive\",\"scope\":\"Window-delivered touchpad frames and local Interaction Context; not raw HID, not browser behavior, not screen latency\"}\n";
            json.close(); notice = L"记录已保存："+captureDirectory.wstring();
        } catch (...) { writeFailed = true; notice = L"保存失败。请保留窗口并检查输出目录。"; }
    }
    void Paint() {
        PAINTSTRUCT ps{}; const auto dc = BeginPaint(window,&ps);
        RECT rect{}; GetClientRect(window,&rect); rect.left = 20; rect.top = 72; rect.right -= 20;
        std::wostringstream text;
        text << L"原生触控板接收诊断（本窗口不注入接触，也不模拟滚动）\n\n"
             << L"请在 Host 准备实验，再激活本窗口，将光标放在此区域。\n"
             << L"窗口接收：" << (registered ? L"已注册" : L"不可用") << L"   Win32=" << registrationError << L"\n"
             << L"识别器初始化 HRESULT=" << static_cast<std::uint32_t>(contextResult) << L"   最近处理：" <<
                (processResult == E_PENDING ? L"尚未处理输入" : std::to_wstring(static_cast<std::uint32_t>(processResult))) << L"\n\n"
             << L"接收回调：" << callbacks << L"    触点记录：" << raw.size() << L"    活动触点：" << active << L"\n"
             << L"指针数据消息：" << pointerMessages << L"    信息查询失败：" << pointerInfoFailures << L" / 错误=" << pointerInfoError << L"    非触控板：" << otherPointerMessages << L"\n"
             << L"识别器输出：" << interactions.size() << L"    识别器累计 X/Y：" << cumulativeX/100.0f << L" / " << cumulativeY/100.0f << L" mm\n"
             << L"识别器缩放比：" << cumulativeScale << L"（1 为尚无缩放，展开应增大，捏合应减小）\n"
             << L"本轮缩放范围：" << minimumScale << L" — " << maximumScale << L"（反向后回到 1 不会清除范围记录）\n"
             << L"接收错误：" << receiveError << L"    丢失记录：" << lost << L"    wheel 消息：" << wheelMessages << L"\n\n"
             << L"这里显示系统投递到本窗口的触控板数据，不是底层 HID 报文。\n"
             << L"识别器使用本窗口自己的设置，结果不能直接替代 Edge 的行为结论。\n"
             << L"实验结束后点击保存记录；清空时自动保存上一轮。当前 capture=" << capture << L"\n\n" << notice << L"\n\n输出目录：" << directory.wstring();
        SetBkMode(dc,TRANSPARENT); const auto value = text.str(); DrawTextW(dc,value.c_str(),-1,&rect,DT_LEFT|DT_WORDBREAK); EndPaint(window,&ps);
    }
};
LRESULT CALLBACK Procedure(HWND h,UINT message,WPARAM w,LPARAM l) {
    auto app = reinterpret_cast<Receiver*>(GetWindowLongPtrW(h,GWLP_USERDATA));
    if (message == WM_NCCREATE) { app = static_cast<Receiver*>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams); SetWindowLongPtrW(h,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(app)); }
    try {
        switch (message) {
        case WM_CREATE: app->Attach(h); return 0;
        case WM_POINTERDOWN: case WM_POINTERUPDATE: case WM_POINTERUP: if (app && app->Receive(w)) return 0; break;
        case WM_MOUSEWHEEL: case WM_MOUSEHWHEEL: if (app) ++app->wheelMessages; break;
        case WM_TIMER: InvalidateRect(h,nullptr,TRUE); return 0;
        case WM_PAINT: if (app) app->Paint(); return 0;
        case WM_COMMAND: if (LOWORD(w) == 1) app->Clear(); else if (LOWORD(w) == 2) app->Save(); InvalidateRect(h,nullptr,TRUE); return 0;
        case WM_CLOSE:
            if (app->registered && !app->registerWindow(h,FALSE)) app->unregistrationError = GetLastError();
            app->CloseContext(); app->closed = true; app->Save(); DestroyWindow(h); return 0;
        case WM_DESTROY: KillTimer(h,1); PostQuitMessage(0); return 0;
        }
    } catch (...) { if (app) { app->notice = L"接收诊断发生异常；结果不能作为通过证据。"; ++app->lost; } if (message == WM_CREATE) return -1; }
    return DefWindowProcW(h,message,w,l);
}
std::filesystem::path DefaultRoot() {
    wchar_t executable[32768]{}; GetModuleFileNameW(nullptr,executable,32768);
    for (auto start : {std::filesystem::current_path(),std::filesystem::path(executable).parent_path()})
        for (auto p=start; !p.empty() && p != p.parent_path(); p=p.parent_path())
            if (std::filesystem::exists(p/L"openspec")) return p/L"artifacts/native-gesture-validation/receiver";
    return std::filesystem::current_path()/L"artifacts/native-gesture-validation/receiver";
}
}
int APIENTRY wWinMain(HINSTANCE instance,HINSTANCE,LPWSTR,int) {
    bool probe{}, smoke{};
    try {
        auto root = DefaultRoot(); int count{}; auto args = CommandLineToArgvW(GetCommandLineW(),&count);
        if (!args) return 1;
        bool invalid{};
        for (int i=1;i<count;++i) {
            if (std::wstring_view(args[i]) == L"--probe") probe = true;
            else if (std::wstring_view(args[i]) == L"--smoke") smoke = true;
            else if (std::wstring_view(args[i]) == L"--output" && i+1<count) root = args[++i];
            else invalid = true;
        }
        LocalFree(args); if (invalid) return 1;
        Receiver app(root);
        if (probe) { app.Save(); return app.writeFailed ? 4 : app.registerWindow && app.history ? 0 : 2; }
        WNDCLASSW cls{}; cls.hInstance = instance; cls.lpfnWndProc = Procedure; cls.lpszClassName = L"WinMouseFix.NativeReceiver";
        cls.hCursor = LoadCursorW(nullptr,IDC_ARROW); cls.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW+1);
        if (!RegisterClassW(&cls)) return 1;
        const auto h = CreateWindowW(cls.lpszClassName,L"WinMouseFix Native Receiver",WS_OVERLAPPEDWINDOW,CW_USEDEFAULT,CW_USEDEFAULT,1080,680,nullptr,nullptr,instance,&app);
        if (!h) return 1;
        if (smoke) {
            app.simulation = true;
            // Exercise the query path directly; Windows filters forged pointer messages.
            app.ReadPointer(0xffffffffu);
            PostMessageW(h,WM_CLOSE,0,0);
        } else ShowWindow(h,SW_SHOW);
        MSG message{}; int result{};
        while ((result=GetMessageW(&message,nullptr,0,0))>0) { TranslateMessage(&message); DispatchMessageW(&message); }
        return result<0 ? 1 : app.writeFailed ? 4 : 0;
    } catch (...) { if (!probe) MessageBoxW(nullptr,L"无法启动原生接收诊断。",L"WinMouseFix",MB_OK|MB_ICONERROR); return 1; }
}
