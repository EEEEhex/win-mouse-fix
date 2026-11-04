#include "ProductEngine.h"
#include <algorithm>

namespace wmf::host {
namespace {
void Preserve(Result& first,Result cleanup) noexcept {
    if (cleanup) return;
    if (first) first = cleanup;
    else if (!first.cleanupError) first.cleanupError = cleanup.nativeError ? cleanup.nativeError : cleanup.cleanupError ? cleanup.cleanupError : ERROR_GEN_FAILURE;
}
}
bool ProductTargetValid(const InputEvent& input,TargetPolicy policy,bool anchor) noexcept {
    if (policy == TargetPolicy::Application) {
        const auto target = reinterpret_cast<HWND>(static_cast<std::uintptr_t>(input.target));
        if (!target || !IsWindow(target) || GetForegroundWindow() != target) return false;
    } else {
        const auto desktop = OpenInputDesktop(0,FALSE,DESKTOP_READOBJECTS);
        if (!desktop) return false;
        wchar_t inputName[256]{},ownName[256]{}; DWORD bytes{};
        const bool valid = GetUserObjectInformationW(desktop,UOI_NAME,inputName,sizeof(inputName),&bytes) &&
            GetUserObjectInformationW(GetThreadDesktop(GetCurrentThreadId()),UOI_NAME,ownName,sizeof(ownName),&bytes) && wcscmp(inputName,ownName) == 0;
        CloseDesktop(desktop); if (!valid) return false;
    }
    POINT cursor{};
    return !anchor || (GetCursorPos(&cursor) && cursor.x == input.anchorX && cursor.y == input.anchorY);
}
ProductEngine::ProductEngine(BindingConfig config,ProductFunctions api) noexcept : config_(config),api_(api) {
    device_.widthHimetric = device_.heightHimetric = 50000;
}
std::uint64_t ProductEngine::Now() const noexcept {
    if (api_.clock) return api_.clock(api_.context);
    LARGE_INTEGER qpc{}; QueryPerformanceCounter(&qpc); return static_cast<std::uint64_t>(qpc.QuadPart);
}
void ProductEngine::Trace(ProductTrace t) noexcept {
    if (wheelHasLedger_ && t.input.kind == InputKind::Wheel && t.input.button == Button::None) {
        t.wheelLedger = 1; t.wheelAcceptedX = wheelLedger_.acceptedX; t.wheelAcceptedY = wheelLedger_.acceptedY;
        t.wheelDiscardedX = wheelLedger_.discardedX; t.wheelDiscardedY = wheelLedger_.discardedY;
        t.wheelEmittedX = wheelEmittedX_; t.wheelEmittedY = wheelEmittedY_;
    }
    t.processStarted = processStarted_; if (!t.returned) t.returned = Now();
    if (api_.trace) api_.trace(api_.context,t);
}
bool ProductEngine::Target(const InputEvent& input,TargetPolicy policy,bool anchor) const noexcept {
    return api_.targetValid ? api_.targetValid(api_.context,input,policy,anchor) : ProductTargetValid(input,policy,anchor);
}
void ProductEngine::Complete(bool canceled) noexcept {
    if (pressed_ && api_.complete) api_.complete(api_.context,current_.session,canceled);
    pressed_ = false;
}
Result ProductEngine::Initialize() noexcept {
    if (Ready()) return {};
    auto result = bindings_ ? Result{} : api_.createBindings(&config_,&bindings_);
    Capabilities caps;
    if (result) result = api_.capabilities(&caps);
    if (result && (caps.apiBits & (RequiredApis|ActionApi)) != (RequiredApis|ActionApi)) result = {Status::Unsupported,ERROR_PROC_NOT_FOUND};
    if (result) result = api_.createPlatform(&platform_);
    if (result) result = api_.openPlatform(platform_,&device_);
    ProductTrace t; t.kind = ProductTraceKind::Initialization; t.result = result; Trace(t);
    if (!result) { const auto cleanup = Close(); Preserve(result,cleanup); }
    return result;
}
Result ProductEngine::Configure(const BindingConfig& config) noexcept {
    if (pressed_ || contact_) return {Status::Busy};
    BindingHandle replacement{}; auto result = api_.createBindings(&config,&replacement); if (!result) return result;
    result = api_.destroyBindings(&bindings_);
    if (!result) { api_.destroyBindings(&replacement); return result; }
    bindings_ = replacement; config_ = config; return {};
}
std::uint64_t ProductEngine::Deadline() const noexcept {
    std::uint64_t value{}; if (bindings_) api_.deadline(bindings_,&value);
    if (wheelDeadline_) value = value ? (std::min)(value,wheelDeadline_) : wheelDeadline_;
    return value;
}
Result ProductEngine::CancelActual() noexcept {
    Result result;
    if (contact_ && platform_) {
        auto cancel = lastSubmitted_; cancel.phase = Phase::Cancel; ++cancel.sequence;
        MappedFrame mapped; result = api_.submit(platform_,&cancel,&mapped);
        ProductTrace t; t.kind = ProductTraceKind::Cleanup; t.input = current_; t.frame = cancel; t.result = result; t.submitted = 1; Trace(t);
        // A native submission failure retires DeviceSession, including its own
        // partial-submission cleanup. Do not invent another live contact.
        if (result || result.code == Status::NativeFailure) contact_ = false;
    }
    if (platform_ && inertia_) {
        const auto stop = api_.stopInertia(platform_,1); Preserve(result,stop); inertia_ = false;
    }
    return result;
}
Result ProductEngine::Abort(Result first,CancelReason reason) noexcept {
    if (reason != CancelReason::UnsupportedInput) wheelRetiredUntil_ = 0;
    if (wheel_) { wheelLedger_.discardedX = wheelLedger_.acceptedX-wheelEmittedX_; wheelLedger_.discardedY = wheelLedger_.acceptedY-wheelEmittedY_; }
    WmfWheelDestroy(&wheel_); wheelDeadline_ = 0;
    if (bindings_) {
        InputEvent cancel = current_; cancel.kind = InputKind::Cancel; cancel.source = InputSource::Control; cancel.flags = 0; cancel.reason = reason;
        cancel.dx = cancel.dy = cancel.wheelX = cancel.wheelY = 0;
        cancel.firstQpc = cancel.lastQpc = (std::max)(Now(),current_.lastQpc);
        BindingDecision ignored; Preserve(first,api_.feedBindings(bindings_,&cancel,&ignored));
    }
    Preserve(first,CancelActual()); Complete(true);
    ProductTrace t; t.kind = ProductTraceKind::Cancellation; t.input = current_; t.input.reason = reason; t.result = first; t.action = action_; Trace(t);
    policy_ = TargetPolicy::Application; action_ = Action::None; return first;
}
Result ProductEngine::Cancel(CancelReason reason) noexcept { processStarted_ = Now(); return Abort({},reason); }
Result ProductEngine::Decide(const InputEvent& event,const BindingDecision& decision) noexcept {
    if (decision.effects & DecisionCanceled) return Abort({},decision.actionInput.reason != CancelReason::None ? decision.actionInput.reason : event.reason);
    if (decision.action == Action::NativeZoom && contact_ && !Target(event,policy_,false)) return Abort({},CancelReason::TargetChanged);
    if (decision.effects & DecisionStopInertia) {
        if (inertia_) {
            const auto stopped = api_.stopInertia(platform_,0);
            ProductTrace t; t.kind = ProductTraceKind::StopInertia; t.input = event; t.result = stopped; Trace(t);
            if (!stopped) return Abort(stopped,CancelReason::NativeError);
            inertia_ = false;
        }
    }
    if (decision.effects & DecisionExecute) {
        if (Target(decision.actionInput,TargetPolicy::Application,false)) {
            ActionRequest request; request.binding = decision.binding; request.repeatCount = decision.repeatCount;
            const auto sent = api_.execute(&request);
            ProductTrace t; t.kind = ProductTraceKind::Action; t.action = decision.action; t.input = decision.actionInput; t.result = sent; Trace(t);
            if (!sent) return Abort(sent,CancelReason::NativeError);
        }
    }
    // Validate the entire decision before submitting even its Begin. An
    // oversized first movement must not create a phantom native gesture.
    for (std::uint32_t i = 0; i < decision.frameCount; ++i) {
        MappedFrame mapped; const auto valid = api_.map(&device_,&decision.frames[i],&mapped);
        if (!valid) return Abort(valid.code == Status::OutOfRange ? Result{} : valid,CancelReason::UnsupportedInput);
    }
    if (decision.frameCount && decision.frames[0].phase == Phase::Begin) {
        // The press selects a target, not a fixed cursor position. Side-button
        // gestures use raw motion while the physical pointer remains free.
        if (!Target(event,TargetPolicy::Application,false)) return Abort({},CancelReason::TargetChanged);
        if (api_.policy) {
            const auto accepted = api_.policy(api_.context,event.session,decision.targetPolicy);
            if (!accepted) return Abort({},CancelReason::StaleInput);
        }
        policy_ = decision.targetPolicy; action_ = decision.action;
    }
    for (std::uint32_t i = 0; i < decision.frameCount; ++i) {
        if (!Target(event,policy_,false)) return Abort({},CancelReason::TargetChanged);
        ProductTrace t; t.kind = ProductTraceKind::Frame; t.action = decision.action; t.direction = decision.direction; t.input = event; t.frame = decision.frames[i];
        if (decision.action == Action::NativeZoom && t.frame.phase == Phase::Update)
            t.stationary = t.frame.contacts[1].xMm == lastSubmitted_.contacts[1].xMm;
        if (decision.action == Action::NativeZoom && t.frame.phase == Phase::End) {
            // Release at the last submitted position, without a final snap to
            // a target that has not yet been output or motion after release.
            t.frame = lastSubmitted_; t.frame.phase = Phase::End; ++t.frame.sequence;
            t.frame.logicalSeconds = (std::max)(t.frame.logicalSeconds,decision.frames[i].logicalSeconds);
        }
        t.boundary = event.kind == InputKind::Up || decision.frames[0].phase == Phase::Begin; t.submitStarted = Now(); t.submitted = 1;
        MappedFrame mapped; t.result = api_.submit(platform_,&t.frame,&mapped); t.returned = Now(); Trace(t);
        if (!t.result) {
            if (t.result.code == Status::NativeFailure) contact_ = false;
            return Abort(t.result,CancelReason::NativeError);
        }
        lastSubmitted_ = t.frame; contact_ = t.frame.phase != Phase::End && t.frame.phase != Phase::Cancel;
        if (t.frame.phase == Phase::End) inertia_ = decision.action != Action::NativeZoom;
    }
    if (decision.effects & DecisionFinished) Complete(false);
    return {};
}
Result ProductEngine::Process(const InputEvent& event) noexcept {
    processStarted_ = Now(); if (!Ready()) return {Status::InvalidState};
    const auto valid = WmfValidateInput(&event); if (!valid) return Abort(valid,CancelReason::UnsupportedInput);
    if (event.source != InputSource::Physical) return {};
    if (event.kind == InputKind::Wheel && event.button == Button::None && !event.flags) return Wheel(event);
    if (event.kind == InputKind::Down && wheel_) { const auto stopped = Abort({},CancelReason::OtherInput); if (!stopped) return stopped; }
    if (event.flags & RecordedInput) {
        if (event.kind == InputKind::Down) { const auto result = Cancel(CancelReason::Pause); if (!result) return result; current_ = event; pressed_ = true; }
        if (event.kind == InputKind::Up || event.kind == InputKind::Cancel) Complete(event.kind == InputKind::Cancel);
        return {};
    }
    if (event.kind == InputKind::Down) {
        if (pressed_ || contact_) return Abort({Status::InvalidState},CancelReason::IncompleteInput);
        if (event.revision != config_.revision) return Abort({Status::InvalidState},CancelReason::StaleInput);
        current_ = event; pressed_ = true; policy_ = TargetPolicy::Application;
    } else if (event.kind == InputKind::Cancel) {
        if (event.session && event.session != current_.session) return {};
        return Abort(event.nativeError ? Result{Status::NativeFailure,event.nativeError,event.cleanupError} : Result{},event.reason);
    } else if (!pressed_ || event.session != current_.session) return {};
    current_ = event;
    BindingDecision decision; const auto result = api_.feedBindings(bindings_,&event,&decision);
    ProductTrace t; t.kind = ProductTraceKind::Input; t.input = event; t.result = result; Trace(t);
    if (!result) return Abort(result,CancelReason::IncompleteInput);
    return Decide(event,decision);
}
Result ProductEngine::Tick(std::uint64_t now) noexcept {
    if (!Ready()) return {Status::InvalidState}; processStarted_ = Now();
    if (wheel_) {
        WheelSample sample; const auto result = WmfWheelSample(wheel_,now,&sample);
        return result ? WheelFrame(sample,now) : Abort(result,CancelReason::IncompleteInput);
    }
    InputEvent tick; tick.kind = InputKind::Tick; tick.source = InputSource::Control; tick.firstQpc = tick.lastQpc = now;
    BindingDecision decision; const auto result = api_.feedBindings(bindings_,&tick,&decision);
    return result ? Decide(current_,decision) : Abort(result,CancelReason::IncompleteInput);
}
Result ProductEngine::Wheel(const InputEvent& event) noexcept {
    if (pressed_ || config_.wheelMode == WheelMode::Off) return {};
    if (wheelRetiredUntil_ && processStarted_ < wheelRetiredUntil_ && event.device == retiredWheel_.device && event.target == retiredWheel_.target &&
        event.revision == retiredWheel_.revision && event.generation == retiredWheel_.generation) {
        current_ = event; RetireWheel(processStarted_);
        ProductTrace canceled; canceled.kind = ProductTraceKind::Cancellation; canceled.input = event; canceled.input.reason = CancelReason::UnsupportedInput; Trace(canceled);
        return {};
    }
    wheelRetiredUntil_ = 0;
    const bool same = wheel_ && event.target == current_.target && event.device == current_.device &&
        event.revision == current_.revision && event.generation == current_.generation &&
        event.anchorX == current_.anchorX && event.anchorY == current_.anchorY;
    if (!same) {
        auto result = Abort({},CancelReason::OtherInput); if (!result) return result;
        if (event.revision != config_.revision || !Target(event,TargetPolicy::Application,true)) return {};
        result = WmfWheelCreate(&config_,&wheel_); if (!result) return result;
        wheelLedger_ = {}; wheelEmittedX_ = wheelEmittedY_ = 0; wheelHasLedger_ = true;
        wheelStarted_ = processStarted_;
    }
    current_ = event; action_ = Action::None; policy_ = TargetPolicy::Application;
    WheelSample sample; const auto now = (std::max)(processStarted_,event.lastQpc);
    const auto result = WmfWheelFeed(wheel_,&event,now,&sample);
    if (result) wheelLedger_ = sample;
    ProductTrace trace; trace.kind = ProductTraceKind::Input; trace.input = event; trace.result = result; Trace(trace);
    return result ? WheelFrame(sample,now,!same) : Abort(result,CancelReason::UnsupportedInput);
}
void ProductEngine::RetireWheel(std::uint64_t now) noexcept {
    retiredWheel_ = current_;
    const auto ms = config_.wheelMode == WheelMode::High ? config_.highMilliseconds : config_.regularMilliseconds;
    const auto duration = (std::max)(1ull,static_cast<std::uint64_t>(ms*static_cast<double>(config_.qpcFrequency)/1000));
    wheelRetiredUntil_ = now > UINT64_MAX-duration ? UINT64_MAX : now+duration;
}
Result ProductEngine::WheelFrame(const WheelSample& sample,std::uint64_t now,bool begin) noexcept {
    wheelLedger_ = sample;
    if (!Target(current_,TargetPolicy::Application,true)) return Abort({},CancelReason::TargetChanged);
    ContactFrame frame; frame.sequence = begin ? 0 : lastSubmitted_.sequence+1;
    frame.logicalSeconds = static_cast<double>(now-wheelStarted_)/static_cast<double>(config_.qpcFrequency);
    frame.phase = begin ? Phase::Begin : Phase::Update;
    frame.contacts[0] = {1,0,sample.xMm-5,sample.yMm}; frame.contacts[1] = {2,0,sample.xMm+5,sample.yMm};
    MappedFrame mapped; const auto valid = api_.map(&device_,&frame,&mapped);
    if (!valid) { if (valid.code == Status::OutOfRange) RetireWheel(now); return Abort(valid.code == Status::OutOfRange ? Result{} : valid,CancelReason::UnsupportedInput); }
    const auto submit = [&](const ContactFrame& next) -> Result {
        ProductTrace t; t.kind = ProductTraceKind::Frame; t.input = current_; t.frame = next;
        t.boundary = next.phase != Phase::Update; t.submitStarted = Now(); t.submitted = 1;
        t.result = api_.submit(platform_,&next,&mapped); t.returned = Now();
        if (t.result) { wheelEmittedX_ = sample.xMm; wheelEmittedY_ = sample.yMm; } Trace(t);
        if (t.result) { lastSubmitted_ = next; contact_ = next.phase != Phase::End; }
        else if (t.result.code == Status::NativeFailure) contact_ = false;
        return t.result;
    };
    auto result = submit(frame); if (!result) return Abort(result,CancelReason::NativeError);
    wheelDeadline_ = sample.nextQpc;
    if (!sample.active) {
        frame.phase = Phase::End; ++frame.sequence;
        result = submit(frame); if (!result) return Abort(result,CancelReason::NativeError);
        inertia_ = true; WmfWheelDestroy(&wheel_); wheelDeadline_ = 0;
    }
    return {};
}
Result ProductEngine::Close() noexcept {
    if (!bindings_ && !platform_) return {};
    auto result = Cancel(CancelReason::Shutdown);
    if (platform_) Preserve(result,api_.destroyPlatform(&platform_));
    if (bindings_) Preserve(result,api_.destroyBindings(&bindings_));
    contact_ = inertia_ = pressed_ = false; return result;
}
}
