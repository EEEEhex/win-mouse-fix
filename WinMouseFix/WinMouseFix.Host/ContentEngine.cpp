#include "ContentEngine.h"
#include <algorithm>

namespace wmf::host {
ContentEngine::ContentEngine(DragConfig config,DeviceConfig device,bool captureOnly,ContentFunctions functions) noexcept
    : config_(config),device_(device),captureOnly_(captureOnly),api_(functions) {}
std::uint64_t ContentEngine::Now() const noexcept {
    if (api_.clock) return api_.clock(api_.context);
    LARGE_INTEGER t{}; QueryPerformanceCounter(&t); return static_cast<std::uint64_t>(t.QuadPart);
}
void ContentEngine::Record(ContentTrace t) noexcept {
    if (!t.returned) t.returned = Now();
    if (api_.trace) api_.trace(api_.context,t);
}
Result ContentEngine::Initialize() noexcept {
    if (Ready()) return {};
    auto result = api_.createDrag(&config_,&drag_);
    ContentTrace t; t.kind = ContentTraceKind::Initialization; t.result = result; Record(t);
    if (!result) return result;
    if (!captureOnly_) {
        Capabilities caps;
        result = api_.capabilities(&caps);
        if (result && (caps.apiBits & (RequiredApis | ActionApi)) != (RequiredApis | ActionApi)) result = {Status::Unsupported,ERROR_PROC_NOT_FOUND};
        if (result) result = api_.createPlatform(&platform_);
        if (result) result = api_.openPlatform(platform_,&device_);
        t.result = result; Record(t);
        if (!result) { const auto cleanup = Close(); if (!cleanup) result.cleanupError = cleanup.nativeError; return result; }
    }
    state_ = DragState::Idle; current_ = {}; released_ = contactActive_ = guardRejected_ = false; lastSubmitted_ = {}; return {};
}
void ContentEngine::Complete(bool click,bool canceled) noexcept {
    if (api_.complete && current_.session) api_.complete(api_.context,current_.session,click && !captureOnly_,canceled);
}
Result ContentEngine::Frames(const MouseInput& input,const DragOutput& output,bool validate) noexcept {
    for (std::uint32_t i = 0; i < output.frameCount; ++i) {
        ContentTrace t; t.kind = ContentTraceKind::Frame; t.input = input;
        if ((output.effects & FinishedEffect) || (output.frameCount && output.frames[0].phase == Phase::Begin)) t.input.flags |= ContentBoundary;
        t.frame = output.frames[i]; t.processStart = processingStarted_ ? processingStarted_ : Now();
        if (validate && api_.targetValid && !api_.targetValid(api_.context,input)) { guardRejected_ = true; return {Status::InvalidState}; }
        t.submitStart = Now();
        if (!captureOnly_) {
            MappedFrame mapped; t.submitted = 1;
            t.result = api_.submit(platform_,&t.frame,&mapped);
            if (t.result) {
                lastSubmitted_ = t.frame;
                contactActive_ = t.frame.phase != Phase::End && t.frame.phase != Phase::Cancel;
            } else if (t.result.code == Status::NativeFailure) contactActive_ = false; // DeviceSession has faulted and destroyed its native device.
        }
        t.returned = Now(); Record(t);
        if (!t.result) return t.result;
    }
    return {};
}
Result ContentEngine::Process(const MouseInput& input) noexcept {
    processingStarted_ = Now();
    guardRejected_ = false;
    if (!Ready()) return {Status::InvalidState};
    if (input.kind == MouseInputKind::Down) {
        if (state_ == DragState::Pending || state_ == DragState::Active) return Abort({Status::InvalidState},CancelReason::IncompleteInput);
        const auto reset = api_.resetDrag(drag_);
        if (!reset) return Abort(reset,CancelReason::NativeError);
        current_ = input;
        if (!captureOnly_ && released_) {
            ContentTrace t; t.kind = ContentTraceKind::StopInertia; t.input = input;
            t.result = api_.stopInertia(platform_,0); Record(t);
            if (!t.result) return Abort(t.result,CancelReason::NativeError);
            released_ = false;
        }
    } else if (input.session != current_.session || (state_ != DragState::Pending && state_ != DragState::Active)) {
        // Capture cancellation may arrive after a successful End; it still ends the
        // retained device/inertia, but is never fed as a second click.
        if (input.kind == MouseInputKind::Cancel && (!current_.session || input.session == current_.session)) {
            current_ = input; return Abort(input.nativeError ? Result{Status::NativeFailure,input.nativeError,input.cleanupError} : Result{},input.reason);
        }
        return {};
    }
    if (input.kind != MouseInputKind::Cancel && api_.targetValid && !api_.targetValid(api_.context,input)) {
        current_ = input; return Abort({},CancelReason::TargetChanged);
    }
    current_ = input;
    if (input.kind == MouseInputKind::Cancel) return Abort(input.nativeError ? Result{Status::NativeFailure,input.nativeError,input.cleanupError} : Result{},input.reason);
    DragOutput output;
    ContentTrace t; t.kind = ContentTraceKind::Input; t.input = input; t.processStart = Now();
    t.result = api_.feedDrag(drag_,&input,&output); t.input.flags = output.effects; t.returned = Now(); Record(t);
    if (!t.result) return Abort(t.result,CancelReason::IncompleteInput);
    state_ = output.state;
    const auto result = Frames(input,output);
    if (!result) return guardRejected_ ? Abort({},CancelReason::TargetChanged) :
        Abort(result,result.code == Status::OutOfRange ? CancelReason::UnsupportedInput : CancelReason::NativeError);
    if (output.effects & FinishedEffect) {
        released_ = output.frameCount != 0;
        Complete((output.effects & ClickEffect) != 0,false);
    }
    return {};
}
Result ContentEngine::Cancel(CancelReason reason) noexcept { return Abort({},reason); }
Result ContentEngine::Abort(Result first,CancelReason reason) noexcept {
    processingStarted_ = Now();
    if (drag_ && (state_ == DragState::Pending || state_ == DragState::Active)) {
        MouseInput cancel = current_; cancel.kind = MouseInputKind::Cancel; cancel.reason = reason;
        cancel.sequence = current_.sequence == UINT64_MAX ? UINT64_MAX : current_.sequence+1;
        cancel.firstQpc = cancel.lastQpc = (std::max)(Now(),current_.lastQpc);
        DragOutput output;
        const auto r = api_.feedDrag(drag_,&cancel,&output);
        if (r) {
            DragOutput cleanupFrames;
            if (captureOnly_) cleanupFrames = output;
            else if (contactActive_) {
                // Core may have generated frames which never reached the OS.
                // Cancel only an actually submitted contact, at its last native
                // position/sequence; never send a phantom Cancel before Begin.
                cleanupFrames.frameCount = 1; cleanupFrames.effects = CanceledEffect | FinishedEffect;
                cleanupFrames.frames[0] = lastSubmitted_;
                cleanupFrames.frames[0].phase = Phase::Cancel; ++cleanupFrames.frames[0].sequence;
                if (output.frameCount) cleanupFrames.frames[0].logicalSeconds = output.frames[0].logicalSeconds;
            }
            const auto cleanup = Frames(cancel,cleanupFrames,false);
            if (!cleanup && first) first = cleanup;
            else if (!cleanup && !first.cleanupError) first.cleanupError = cleanup.nativeError ? cleanup.nativeError : cleanup.cleanupError;
        }
        else if (first) first = r;
    }
    state_ = DragState::Canceled;
    const auto cleanup = Close();
    if (!cleanup && first) first = cleanup;
    else if (!cleanup && !first.cleanupError) first.cleanupError = cleanup.nativeError ? cleanup.nativeError : cleanup.cleanupError;
    Complete(false,true);
    ContentTrace t; t.kind = ContentTraceKind::Failure; t.input = current_; t.input.reason = reason; t.result = first; Record(t);
    return first;
}
Result ContentEngine::Close() noexcept {
    Result result;
    if (platform_) {
        result = api_.destroyPlatform(&platform_);
        ContentTrace t; t.kind = ContentTraceKind::Cleanup; t.input = current_; t.result = result; Record(t);
    }
    if (drag_) api_.destroyDrag(&drag_);
    state_ = DragState::Idle; released_ = contactActive_ = false; return result;
}
}
