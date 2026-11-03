#include "ContentRuntime.h"
#include "ContentTarget.h"
#include <stdexcept>

namespace wmf::host {
namespace {
std::uint64_t Now() noexcept { LARGE_INTEGER t{}; QueryPerformanceCounter(&t); return static_cast<std::uint64_t>(t.QuadPart); }
TargetSample ReadTarget(const MouseInput& e) noexcept {
    const auto w = reinterpret_cast<HWND>(static_cast<std::uintptr_t>(e.target));
    TargetSample actual;
    actual.windowValid = w && IsWindow(w);
    actual.foreground = reinterpret_cast<std::uintptr_t>(GetForegroundWindow());
    POINT p{}; actual.cursorKnown = GetCursorPos(&p) != FALSE; actual.x = p.x; actual.y = p.y;
    return actual;
}
}
ContentRuntime::ContentRuntime(ContentReport& report,ContentOptions options,ContentRuntimeApi api)
    : report_(report),options_(options),api_(api) {
    stop_ = CreateEventW(nullptr,TRUE,FALSE,nullptr);
    workerWake_ = CreateEventW(nullptr,FALSE,FALSE,nullptr);
    captureWake_ = CreateEventW(nullptr,FALSE,FALSE,nullptr);
    workerInitialized_ = CreateEventW(nullptr,TRUE,FALSE,nullptr);
    if (!stop_ || !workerWake_ || !captureWake_ || !workerInitialized_) {
        for (auto h : {stop_,workerWake_,captureWake_,workerInitialized_}) if (h) CloseHandle(h);
        throw std::runtime_error("Cannot create content input wait handles");
    }
    try {
        workerThread_ = std::thread(&ContentRuntime::Worker,this);
        captureThread_ = std::thread(&ContentRuntime::Capture,this);
    } catch (...) {
        Stop();
        if (workerThread_.joinable()) workerThread_.join();
        for (auto h : {stop_,workerWake_,captureWake_,workerInitialized_}) CloseHandle(h);
        throw;
    }
}
ContentRuntime::~ContentRuntime() {
    Stop();
    if (workerThread_.joinable()) workerThread_.join();
    if (captureThread_.joinable()) captureThread_.join();
    for (auto h : {stop_,workerWake_,captureWake_,workerInitialized_}) CloseHandle(h);
}
void ContentRuntime::Enable(bool enabled) noexcept {
    if (WaitForSingleObject(stop_,0) == WAIT_OBJECT_0) return;
    if (requested_.exchange(enabled) == enabled) return;
    ++requestedGeneration_;
    if (!enabled) { cancel_ = true; captureCancel_ = true; }
    SetEvent(workerWake_); SetEvent(captureWake_);
}
void ContentRuntime::Stop() noexcept {
    requested_ = false; cancel_ = true; captureCancel_ = true;
    SetEvent(stop_); SetEvent(workerWake_); SetEvent(captureWake_);
}
void ContentRuntime::Fault(Result result) noexcept {
    if (result) return;
    Status expected = Status::Ok;
    if (failureCode_.compare_exchange_strong(expected,result.code)) {
        nativeError_ = result.nativeError; cleanupError_ = result.cleanupError;
        failurePending_ = true;
    }
    requested_ = false; cancel_ = true; captureCancel_ = true;
    SetEvent(workerWake_); SetEvent(captureWake_);
}
Result ContentRuntime::Failure() const noexcept { return {failureCode_.load(),nativeError_.load(),cleanupError_.load()}; }
ContentRuntimeState ContentRuntime::State() const noexcept {
    if (Finished()) return ContentRuntimeState::Stopped;
    if (WaitForSingleObject(stop_,0) == WAIT_OBJECT_0) return ContentRuntimeState::Stopping;
    if (!requested_) return Failure() ? ContentRuntimeState::Paused : ContentRuntimeState::Faulted;
    return engineReady_ && captureReady_ && readyGeneration_ == requestedGeneration_ ? ContentRuntimeState::Enabled : ContentRuntimeState::Starting;
}
void ContentRuntime::RecordFailure() noexcept {
    if (!failurePending_.exchange(false)) return;
    ContentTrace t; t.kind = ContentTraceKind::Failure; t.result = Failure();
    t.returned = Now();
    t.input.reason = t.result.nativeError == ERROR_BUFFER_OVERFLOW ? CancelReason::Overflow :
        t.result.nativeError == ERROR_TIMEOUT ? CancelReason::StaleInput : CancelReason::NativeError;
    report_.Push(t);
}
bool ContentRuntime::Receive(void* p,const MouseInput* e) noexcept { return p && e && static_cast<ContentRuntime*>(p)->Receive(*e); }
bool ContentRuntime::Receive(const MouseInput& e) noexcept {
    if (!requested_ && e.kind != MouseInputKind::Cancel) return true;
    if (!input_.Push(e)) { Fault({Status::Busy,ERROR_BUFFER_OVERFLOW}); return false; }
    SetEvent(workerWake_); return true;
}
void ContentRuntime::PostComplete(std::uint64_t session,bool click,bool canceled) noexcept {
    if (auto handle = capture_.load()) {
        const auto command = canceled ? CaptureCommand::Cancel : click ? CaptureCommand::CompleteClick : CaptureCommand::CompleteDrag;
        const auto result = api_.postCapture(handle,command,session);
        if (!result) Fault(result);
    }
}
void ContentRuntime::Worker() noexcept {
    auto functions = api_.engine;
    functions.context = this;
    functions.clock = [](void* p) noexcept { auto& r = *static_cast<ContentRuntime*>(p); return r.api_.engine.clock ? r.api_.engine.clock(r.api_.engine.context) : Now(); };
    functions.trace = [](void* p,const ContentTrace& t) noexcept { static_cast<ContentRuntime*>(p)->report_.Push(t); };
    functions.complete = [](void* p,std::uint64_t s,bool click,bool canceled) noexcept { static_cast<ContentRuntime*>(p)->PostComplete(s,click,canceled); };
    functions.targetValid = [](void* p,const MouseInput& e) noexcept {
        auto& r = *static_cast<ContentRuntime*>(p);
        if (r.api_.engine.targetValid) return r.api_.engine.targetValid(r.api_.engine.context,e);
        const auto actual = ReadTarget(e); const auto flags = CheckTarget(e,actual);
        if (flags) {
            ContentTrace t; t.kind = ContentTraceKind::TargetGuard; t.input = e; t.input.reason = CancelReason::TargetChanged;
            t.guardFlags = flags; t.observedForeground = actual.foreground; t.observedX = actual.x; t.observedY = actual.y;
            t.returned = Now(); r.report_.Push(t);
        }
        return flags == 0;
    };
    ContentEngine engine(options_.drag,options_.device,options_.captureOnly,functions);
    try {
        SetEvent(workerInitialized_);
        HANDLE waits[]{stop_,workerWake_};
        std::uint64_t handledGeneration{};
        while (WaitForSingleObject(stop_,0) != WAIT_OBJECT_0) {
            RecordFailure();
            const auto generation = requestedGeneration_.load();
            if (cancel_.exchange(false) || !requested_ || generation != handledGeneration) {
                input_.Discard();
                const auto result = engine.Ready() ? engine.Cancel(CancelReason::Pause) : Result{};
                engineReady_ = false; SetEvent(captureWake_);
                if (!result) Fault(result);
                handledGeneration = generation;
            }
            if (requested_ && !engine.Ready()) {
                const auto result = engine.Initialize();
                if (!result) Fault(result);
                else { readyGeneration_ = handledGeneration; engineReady_ = true; SetEvent(captureWake_); }
            }
            MouseInput input;
            while (requested_ && !cancel_ && input_.Pop(input)) {
                const auto now = Now();
                if (input.kind != MouseInputKind::Cancel && now > input.firstQpc && now-input.firstQpc > options_.drag.qpcFrequency/20) {
                    const auto result = engine.Cancel(CancelReason::StaleInput);
                    Fault(result ? Result{Status::Busy,ERROR_TIMEOUT} : result); break;
                }
                const auto result = engine.Process(input);
                if (!result) { Fault(result); break; }
                if (!engine.Ready()) {
                    engineReady_ = false; SetEvent(captureWake_);
                    if (requested_) {
                        const auto reinitialized = engine.Initialize();
                        if (!reinitialized) Fault(reinitialized);
                        else { readyGeneration_ = handledGeneration; engineReady_ = true; SetEvent(captureWake_); }
                    }
                }
            }
            if (report_.Failed()) Fault({Status::NativeFailure,ERROR_WRITE_FAULT});
            if (WaitForMultipleObjects(2,waits,FALSE,100) == WAIT_FAILED) { Fault({Status::NativeFailure,GetLastError()}); break; }
        }
        const auto result = engine.Cancel(CancelReason::Shutdown);
        if (!result) Fault(result);
        engineReady_ = false;
    } catch (...) { Fault({Status::NativeFailure,ERROR_UNHANDLED_EXCEPTION}); }
    // The engine destructor also runs before capture may destroy its handle.
    engine.Close(); engineReady_ = false;
    RecordFailure();
    workerDone_ = true; SetEvent(captureWake_);
}
void ContentRuntime::Capture() noexcept {
    CaptureHandle handle{};
    try {
        MSG message{}; PeekMessageW(&message,nullptr,WM_USER,WM_USER,PM_NOREMOVE);
        HANDLE initialized[]{stop_,workerInitialized_};
        WaitForMultipleObjects(2,initialized,FALSE,INFINITE);
        HANDLE waits[]{stop_,captureWake_};
        bool lastEnabled{};
        auto updated = Now();
        for (;;) {
            const bool stopping = WaitForSingleObject(stop_,0) == WAIT_OBJECT_0;
            if (!handle && !stopping && requested_ && engineReady_) {
                CaptureConfig config; config.context = this; config.sink = &ContentRuntime::Receive;
                const auto result = api_.createCapture(&config,&handle);
                capture_ = handle;
                if (!result) { report_.SetCapture({},result); Fault(result); }
                else captureReady_ = true;
                lastEnabled = false;
            }
            if (captureCancel_.exchange(false) && handle) {
                const auto result = api_.postCapture(handle,CaptureCommand::Disable,0);
                if (!result) Fault(result);
                lastEnabled = false;
            }
            const bool enabled = !stopping && requested_ && engineReady_ && captureReady_ && readyGeneration_ == requestedGeneration_;
            if (handle && enabled != lastEnabled) {
                const auto result = api_.postCapture(handle,enabled ? CaptureCommand::Enable : CaptureCommand::Disable,0);
                if (!result) Fault(result);
                lastEnabled = enabled;
            }
            while (PeekMessageW(&message,nullptr,0,0,PM_REMOVE)) {
                if (message.message == WM_QUIT) Stop();
                else { TranslateMessage(&message); DispatchMessageW(&message); }
            }
            if (handle && Now()-updated >= options_.drag.qpcFrequency/5) {
                CaptureStats stats; const auto result = api_.readCapture(handle,&stats);
                report_.SetCapture(stats,result); updated = Now();
            }
            if (stopping && workerDone_) {
                if (!handle) break;
                CaptureStats stats; api_.readCapture(handle,&stats); report_.SetCapture(stats);
                captureReady_ = false;
                const auto result = api_.destroyCapture(&handle);
                if (result) { capture_ = nullptr; break; }
                if (result.code != Status::Busy) { report_.SetCapture(stats,result); Fault(result); }
            }
            // Do not busy-spin on a permanently signaled stop event while waiting
            // for a captured physical button release or cleanup retry.
            const auto wait = stopping ? MsgWaitForMultipleObjectsEx(0,nullptr,10,QS_ALLINPUT,MWMO_INPUTAVAILABLE) :
                MsgWaitForMultipleObjectsEx(2,waits,100,QS_ALLINPUT,MWMO_INPUTAVAILABLE);
            if (wait == WAIT_FAILED) { Fault({Status::NativeFailure,GetLastError()}); Stop(); }
        }
    } catch (...) { Fault({Status::NativeFailure,ERROR_UNHANDLED_EXCEPTION}); Stop(); }
    captureReady_ = false; captureDone_ = true;
}
}
