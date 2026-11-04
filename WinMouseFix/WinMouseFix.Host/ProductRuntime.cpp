#include "ProductRuntime.h"
#include <algorithm>
#include <stdexcept>
#include <vector>

namespace wmf::host {
namespace {
std::uint64_t Clock() noexcept { LARGE_INTEGER q{}; QueryPerformanceCounter(&q); return static_cast<std::uint64_t>(q.QuadPart); }
ProductRuntimeApi GuardEnvironment(const ProductEnvironment& environment,ProductRuntimeApi api) {
    if (api.simulation && !environment.simulation) throw std::invalid_argument("Fake product backend requires an isolated environment");
    api.simulation = environment.simulation; return api;
}
BindingConfig AvailableBindings(BindingConfig config,std::uint32_t flags) noexcept {
    if (flags & ExplicitValidationProbe) return config;
    if (!(flags & WheelSourceValidated)) config.wheelMode = WheelMode::Off;
    if (!(flags & NativeZoomValidated)) {
        std::uint32_t count{};
        for (std::uint32_t i=0;i<config.bindingCount;++i)
            if (config.bindings[i].action != Action::NativeZoom) config.bindings[count++] = config.bindings[i];
        for (auto i=count;i<config.bindingCount;++i) config.bindings[i] = {};
        config.bindingCount = count;
    }
    return config;
}
}
ProductRuntime::ProductRuntime(const ProductEnvironment& environment,BindingConfig config,bool enabled,ProductRuntimeApi api,std::uint32_t flags)
    : ProductRuntime(config,enabled,GuardEnvironment(environment,api),flags) {}
ProductRuntime::ProductRuntime(BindingConfig config,bool enabled,ProductRuntimeApi api,std::uint32_t flags)
    : api_(api),flags_(flags),requested_(enabled) {
    if (api_.simulation && (api_.createCapture == WmfProductCaptureCreate || api_.engine.submit == WmfPlatformSubmitContacts || api_.engine.execute == WmfExecuteAction))
        throw std::invalid_argument("Simulation requires fake capture, contact submission and discrete actions");
    if (!WmfValidateBindings(&config,nullptr)) throw std::invalid_argument("Invalid product binding configuration");
    // Availability affects execution only. ConfigStore keeps every user binding
    // so an expired optional capability never blocks unrelated gestures.
    current_ = std::make_shared<const BindingConfig>(AvailableBindings(config,flags_)); configs_.emplace(config.revision,current_);
    stop_ = CreateEventW(nullptr,TRUE,FALSE,nullptr); workerWake_ = CreateEventW(nullptr,FALSE,FALSE,nullptr); captureWake_ = CreateEventW(nullptr,FALSE,FALSE,nullptr);
    deadlineTimer_ = CreateWaitableTimerExW(nullptr,nullptr,CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,TIMER_MODIFY_STATE|SYNCHRONIZE);
    if (!stop_ || !workerWake_ || !captureWake_ || !deadlineTimer_) {
        for (auto h : {stop_,workerWake_,captureWake_,deadlineTimer_}) if (h) CloseHandle(h);
        throw std::runtime_error("Cannot create product runtime wait handles");
    }
    try { workerThread_ = std::thread(&ProductRuntime::Worker,this); captureThread_ = std::thread(&ProductRuntime::Capture,this); }
    catch (...) {
        Stop(); if (workerThread_.joinable()) workerThread_.join();
        for (auto h : {stop_,workerWake_,captureWake_,deadlineTimer_}) CloseHandle(h); throw;
    }
}
ProductRuntime::~ProductRuntime() {
    Stop(); if (workerThread_.joinable()) workerThread_.join(); if (captureThread_.joinable()) captureThread_.join();
    for (auto h : {stop_,workerWake_,captureWake_,deadlineTimer_}) CloseHandle(h);
}
std::shared_ptr<const BindingConfig> ProductRuntime::Current() { std::lock_guard lock(configsMutex_); return current_; }
std::shared_ptr<const BindingConfig> ProductRuntime::Resolve(std::uint64_t revision) {
    std::lock_guard lock(configsMutex_); const auto found = configs_.find(revision);
    return found == configs_.end() ? nullptr : found->second.lock();
}
Result ProductRuntime::Publish(const BindingConfig& config) {
    PreparedConfig prepared; const auto result = Prepare(config,prepared);
    return result ? PublishPrepared(std::move(prepared)) : result;
}
Result ProductRuntime::Prepare(const BindingConfig& config,PreparedConfig& prepared) {
    const auto valid = WmfValidateBindings(&config,nullptr); if (!valid) return valid;
    auto snapshot = std::make_shared<const BindingConfig>(AvailableBindings(config,flags_));
    {
        std::lock_guard lock(configsMutex_);
        if (config.revision <= current_->revision) return {Status::InvalidState};
        configs_[config.revision] = snapshot;
        for (auto it = configs_.begin(); it != configs_.end();) { if (it->second.expired()) it = configs_.erase(it); else ++it; }
    }
    prepared.snapshot = std::move(snapshot); return {};
}
Result ProductRuntime::PublishPrepared(PreparedConfig&& prepared) noexcept {
    if (!prepared.snapshot) return {Status::InvalidArgument};
    {
        std::lock_guard lock(configsMutex_);
        if (prepared.snapshot->revision <= current_->revision) return {Status::InvalidState};
        current_ = std::move(prepared.snapshot);
    }
    SetEvent(captureWake_); SetEvent(workerWake_); return {};
}
void ProductRuntime::Enable(bool enabled) noexcept {
    std::lock_guard lock(controlMutex_); ++controlEpoch_; SetEnabled(enabled);
}
bool ProductRuntime::ApplySavedEnabled(bool enabled,std::uint64_t expected) noexcept {
    std::lock_guard lock(controlMutex_);
    if (controlEpoch_ != expected) return false;
    SetEnabled(enabled); return true;
}
void ProductRuntime::SetEnabled(bool enabled) noexcept {
    if (WaitForSingleObject(stop_,0) == WAIT_OBJECT_0 || !Failure()) return;
    if (requested_.exchange(enabled) == enabled) return;
    ++generation_;
    if (!enabled) { recording_ = false; recordingDirty_ = true; cancel_ = true; captureCancel_ = true; }
    SetEvent(workerWake_); SetEvent(captureWake_);
}
void ProductRuntime::Record(bool enabled) noexcept {
    std::lock_guard lock(controlMutex_); ++controlEpoch_;
    if (WaitForSingleObject(stop_,0) == WAIT_OBJECT_0 || !Failure()) return;
    recording_ = enabled; recordingDirty_ = true;
    cancel_ = true; captureCancel_ = true; ++generation_; SetEvent(workerWake_); SetEvent(captureWake_);
}
void ProductRuntime::Stop() noexcept {
    std::lock_guard lock(controlMutex_); ++controlEpoch_;
    requested_ = false; recording_ = false; recordingDirty_ = true; cancel_ = true; captureCancel_ = true;
    SetEvent(stop_); SetEvent(workerWake_); SetEvent(captureWake_);
}
void ProductRuntime::Fault(Result result) noexcept {
    if (result) return;
    bool expected = false;
    if (failureClaimed_.compare_exchange_strong(expected,true)) {
        nativeError_ = result.nativeError;
        if (result.cleanupError) { std::uint32_t empty{}; cleanupError_.compare_exchange_strong(empty,result.cleanupError); }
        failureCode_.store(result.code,std::memory_order_release);
    }
    ++controlEpoch_; requested_ = false; recording_ = false; cancel_ = true; captureCancel_ = true;
    SetEvent(workerWake_); SetEvent(captureWake_);
}
void ProductRuntime::CleanupFailure(Result result) noexcept {
    if (result) return;
    if (!failureClaimed_) { Fault(result); return; }
    std::uint32_t expected{};
    cleanupError_.compare_exchange_strong(expected,result.cleanupError ? result.cleanupError : result.nativeError ? result.nativeError : ERROR_GEN_FAILURE);
}
Result ProductRuntime::Failure() const noexcept { return {failureCode_,nativeError_,cleanupError_}; }
ProductRuntimeState ProductRuntime::State() const noexcept {
    if (Finished()) return ProductRuntimeState::Stopped;
    if (WaitForSingleObject(stop_,0) == WAIT_OBJECT_0) return ProductRuntimeState::Stopping;
    if (!Failure()) return ProductRuntimeState::Faulted;
    if (!requested_) return ProductRuntimeState::Paused;
    return workerReady_ && captureReady_ && readyGeneration_ == generation_ ? ProductRuntimeState::Enabled : ProductRuntimeState::Starting;
}
CaptureStats ProductRuntime::Stats() const noexcept { std::lock_guard lock(statsMutex_); return captureStats_; }
bool ProductRuntime::Receive(void* p,const InputEvent* e) noexcept { return p && e && static_cast<ProductRuntime*>(p)->Receive(*e); }
bool ProductRuntime::Receive(const InputEvent& event) noexcept {
    if (!requested_ && !recording_ && event.kind != InputKind::Cancel) return true;
    if (!input_.Push(event)) { Fault({Status::Busy,ERROR_BUFFER_OVERFLOW}); return false; }
    accepted_ += event.samples; SetEvent(workerWake_); return true;
}
void ProductRuntime::Complete(std::uint64_t session,bool canceled) noexcept {
    if (const auto handle = capture_.load()) {
        const auto result = api_.post(handle,canceled ? CaptureCommand::Cancel : CaptureCommand::CompleteDrag,session);
        if (!result) Fault(result);
    }
}
void ProductRuntime::Worker() noexcept {
    auto functions = api_.engine; functions.context = this;
    functions.clock = [](void* p) noexcept { auto& r = *static_cast<ProductRuntime*>(p); return r.api_.engine.clock ? r.api_.engine.clock(r.api_.engine.context) : Clock(); };
    functions.trace = [](void* p,const ProductTrace& t) noexcept {
        auto& r = *static_cast<ProductRuntime*>(p); if (!r.traces_.Push(t)) ++r.lostTraces_;
        if (r.api_.engine.trace) r.api_.engine.trace(r.api_.engine.context,t);
    };
    functions.complete = [](void* p,std::uint64_t session,bool canceled) noexcept { static_cast<ProductRuntime*>(p)->Complete(session,canceled); };
    functions.policy = [](void* p,std::uint64_t session,TargetPolicy policy) noexcept -> Result {
        auto& r = *static_cast<ProductRuntime*>(p); const auto handle = r.capture_.load();
        return handle ? r.api_.policy(handle,session,policy) : Result{Status::InvalidState};
    };
    functions.targetValid = [](void* p,const InputEvent& e,TargetPolicy policy,bool anchor) noexcept {
        auto& r = *static_cast<ProductRuntime*>(p);
        return r.api_.engine.targetValid ? r.api_.engine.targetValid(r.api_.engine.context,e,policy,anchor) : ProductTargetValid(e,policy,anchor);
    };
    try {
        ProductEngine engine(*Current(),functions);
        std::uint64_t handledGeneration{};
        const HANDLE waits[]{stop_,workerWake_,deadlineTimer_};
        while (WaitForSingleObject(stop_,0) != WAIT_OBJECT_0) {
            const auto generation = generation_.load(); const bool run = requested_ || recording_;
            if (cancel_.exchange(false) || !run || handledGeneration != generation) {
                InputEvent discarded; while (input_.Pop(discarded)) processed_ += discarded.samples;
                const auto result = engine.Cancel(CancelReason::Pause); if (!result) Fault(result);
                if (!run) { const auto closed = engine.Close(); if (!closed) Fault(closed); }
                workerReady_ = false; workerPaused_ = true; activeRevision_ = 0; handledGeneration = generation; SetEvent(captureWake_);
            }
            if (run && Failure() && !engine.Ready()) {
                workerPaused_ = false; const auto snapshot = Current();
                auto result = engine.Configure(*snapshot); if (result) result = engine.Initialize();
                if (!result) Fault(result);
            }
            if (run && Failure() && engine.Ready()) {
                workerPaused_ = false; readyGeneration_ = handledGeneration;
                if (!workerReady_.exchange(true)) SetEvent(captureWake_);
                InputEvent event;
                while ((requested_ || recording_) && !cancel_ && input_.Pop(event)) {
                    if (event.generation != handledGeneration) { Complete(event.session,true); processed_ += event.samples; continue; }
                    auto result = Result{};
                    if ((event.kind == InputKind::Down || (event.kind == InputKind::Wheel && event.button == Button::None)) && event.revision != engine.Revision()) {
                        if (engine.ContactActive() && !engine.Pressed()) result = engine.Cancel(CancelReason::StaleInput);
                        const auto snapshot = Resolve(event.revision);
                        if (result) result = snapshot ? engine.Configure(*snapshot) : Result{Status::InvalidState};
                    }
                    if (result) {
                        const auto now = Clock(); const auto snapshot = Current();
                        if (event.kind != InputKind::Cancel && now > event.firstQpc && now-event.firstQpc > snapshot->qpcFrequency/20)
                            result = {Status::Busy,ERROR_TIMEOUT};
                        else result = engine.Process(event);
                    }
                    if ((event.flags & RecordedInput) && event.kind == InputKind::Down && !recorded_.Push(event)) result = {Status::Busy,ERROR_BUFFER_OVERFLOW};
                    activeRevision_ = engine.Pressed() || engine.Deadline() ? engine.Revision() : 0;
                    processed_ += event.samples; if (!result) { Fault(result); break; }
                }
                const auto deadline = engine.Deadline();
                if (deadline && Clock() >= deadline && !cancel_) {
                    const auto result = engine.Tick(Clock()); if (!result) Fault(result);
                    activeRevision_ = engine.Pressed() || engine.Deadline() ? engine.Revision() : 0;
                }
            }
            DWORD waitCount = 2;
            if (const auto deadline = engine.Deadline()) {
                const auto now = Clock(); const auto frequency = Current()->qpcFrequency;
                const auto ticks = deadline <= now ? 1 : static_cast<LONGLONG>((std::max)(1.0L,(std::min)(1e12L,static_cast<long double>(deadline-now)*10000000.0L/static_cast<long double>(frequency))));
                LARGE_INTEGER due{}; due.QuadPart = -ticks;
                if (!SetWaitableTimerEx(deadlineTimer_,&due,0,nullptr,nullptr,nullptr,0)) { Fault({Status::NativeFailure,GetLastError()}); continue; }
                waitCount = 3;
            } else CancelWaitableTimer(deadlineTimer_);
            // A one-shot high-resolution timer is armed only for real work.
            // Raw input and emergency control still wake this same wait directly.
            if (WaitForMultipleObjects(waitCount,waits,FALSE,INFINITE) == WAIT_FAILED) { Fault({Status::NativeFailure,GetLastError()}); break; }
        }
        const auto result = engine.Close(); if (!result) CleanupFailure(result);
    } catch (...) { Fault({Status::NativeFailure,ERROR_UNHANDLED_EXCEPTION}); }
    workerReady_ = false; workerPaused_ = true; workerDone_ = true; activeRevision_ = 0; SetEvent(captureWake_);
}
void ProductRuntime::Capture() noexcept {
    CaptureHandle handle{};
    try {
        MSG message{}; PeekMessageW(&message,nullptr,WM_USER,WM_USER,PM_NOREMOVE);
        const HANDLE waits[]{stop_,captureWake_};
        bool lastEnabled{},lastRecording{};
        std::uint64_t cleanupFailedSince{};
        std::shared_ptr<const BindingConfig> posted;
        // Keep every exposed revision until all queued events are processed and
        // no physical press owns it. The weak registry never owns idle history.
        std::vector<std::shared_ptr<const BindingConfig>> retained;
        for (;;) {
            const bool stopping = WaitForSingleObject(stop_,0) == WAIT_OBJECT_0;
            const bool run = !stopping && (requested_ || recording_) && Failure();
            if (!handle && run && workerReady_ && readyGeneration_ == generation_) {
                posted = Current(); retained.push_back(posted);
                ProductCaptureConfig config; config.context = this; config.sink = &ProductRuntime::Receive; config.bindings = *posted; config.flags = flags_;
                if ((flags_ & NativeZoomValidated) && !(flags_ & WheelSourceValidated)) {
                    // A pinch observation proves the paired-button source path.
                    // The Platform ABI uses a shared source bit internally;
                    // ordinary wheel interception remains explicitly disabled.
                    config.flags |= WheelSourceValidated; config.bindings.wheelMode = WheelMode::Off;
                }
                const auto result = api_.createCapture(&config,&handle); capture_ = handle;
                if (!result) Fault(result); else captureReady_ = true;
                lastEnabled = lastRecording = false; recordingDirty_ = true;
            }
            if (handle) {
                if (captureCancel_.exchange(false)) {
                    const auto result = api_.post(handle,CaptureCommand::Disable,0); if (!result) Fault(result); lastEnabled = false;
                }
                const auto snapshot = Current();
                if (posted && snapshot->revision != posted->revision) {
                    retained.push_back(snapshot); const auto result = api_.update(handle,snapshot.get());
                    if (!result) Fault(result); else posted = snapshot;
                }
                if (recordingDirty_.exchange(false) || lastRecording != recording_) {
                    lastRecording = recording_; const auto result = api_.recording(handle,lastRecording ? 1 : 0); if (!result) Fault(result);
                }
                const bool enable = !stopping && (requested_ || recording_) && Failure() && workerReady_ && readyGeneration_ == generation_;
                if (enable != lastEnabled) {
                    const auto result = api_.post(handle,enable ? CaptureCommand::Enable : CaptureCommand::Disable,enable ? generation_.load() : 0);
                    if (!result) Fault(result); lastEnabled = enable;
                }
            }
            while (PeekMessageW(&message,nullptr,0,0,PM_REMOVE)) {
                if (message.message == WM_QUIT) Stop(); else { TranslateMessage(&message); DispatchMessageW(&message); }
            }
            if (handle) {
                CaptureStats stats; const auto read = api_.stats(handle,&stats);
                if (!read) Fault(read); else { std::lock_guard lock(statsMutex_); captureStats_ = stats; }
                CaptureRevision revision; const auto readRevision = api_.revision(handle,&revision);
                if (!readRevision) Fault(readRevision);
                else if (!revision.active && processed_.load() == accepted_.load()) {
                    retained.erase(std::remove_if(retained.begin(),retained.end(),[&](const auto& value) {
                        return value->revision != revision.applied && (!posted || value->revision != posted->revision);
                    }),retained.end());
                }
                if ((!run && workerPaused_) || (stopping && workerDone_)) {
                    const auto result = api_.destroyCapture(&handle);
                    if (result) { capture_ = nullptr; captureReady_ = false; posted.reset(); retained.clear(); lastEnabled = false; }
                    else if (result.code != Status::Busy) {
                        CleanupFailure(result); Fault(result);
                        if (stopping) {
                            if (!cleanupFailedSince) cleanupFailedSince = GetTickCount64();
                            if (GetTickCount64()-cleanupFailedSince >= 1000) { abandonedCapture_ = true; break; }
                        }
                    } else cleanupFailedSince = 0; // A real owned press still needs its physical release.
                }
            }
            if (stopping && workerDone_ && !handle) break;
            const DWORD timeout = stopping ? 10 : handle ? 100 : INFINITE;
            const auto waited = stopping ? MsgWaitForMultipleObjectsEx(0,nullptr,timeout,QS_ALLINPUT,MWMO_INPUTAVAILABLE) :
                MsgWaitForMultipleObjectsEx(2,waits,timeout,QS_ALLINPUT,MWMO_INPUTAVAILABLE);
            if (waited == WAIT_FAILED) { Fault({Status::NativeFailure,GetLastError()}); Stop(); }
        }
    } catch (...) { Fault({Status::NativeFailure,ERROR_UNHANDLED_EXCEPTION}); if (handle) abandonedCapture_ = true; Stop(); }
    captureReady_ = false; captureDone_ = true;
}
}
