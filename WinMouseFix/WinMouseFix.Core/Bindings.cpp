#include "ProductApi.h"
#include "PinchTrajectory.h"
#include <algorithm>
#include <cmath>
#include <new>

namespace wmf {
struct BindingInstance {
    BindingConfig config;
    BindingState state{BindingState::Idle};
    InputEvent current,firstClick;
    Binding active;
    Direction direction{Direction::None};
    std::uint64_t lastSession{},lastSequence{},started{},lastQpc{},frameSequence{},deadline{};
    std::int64_t x{},y{},wheelRemainder{};
    PinchTrajectory pinch;
    bool second{},contact{};
};
namespace {
const Binding* Find(const BindingInstance& h,Trigger trigger,Button button,std::uint32_t modifiers) noexcept {
    for (std::uint32_t i = 0; i < h.config.bindingCount; ++i) {
        const auto& b = h.config.bindings[i];
        if (b.button == button && b.modifiers == modifiers && b.trigger == trigger) return &b;
    }
    return nullptr;
}
const Binding* Find(const BindingInstance& h,Trigger trigger) noexcept { return Find(h,trigger,h.current.button,h.current.modifiers); }
bool Add(std::int64_t a,std::int64_t b,std::int64_t& out) noexcept {
    constexpr std::int64_t limit = 9007199254740991ll;
    if (b < -limit || b > limit || (b > 0 && a > limit-b) || (b < 0 && a < -limit-b)) return false;
    out = a+b; return true;
}
bool Deadline(BindingInstance& h,std::uint64_t now,std::uint32_t ms) noexcept {
    const auto delta = h.config.qpcFrequency*ms/1000;
    if (now > UINT64_MAX-delta) return false;
    h.deadline = now+delta; return true;
}
void Execute(BindingDecision& out,const Binding* binding,const InputEvent& source,std::uint32_t repeat = 1) noexcept {
    if (!binding) return;
    out.binding = *binding; out.action = binding->action; out.actionInput = source;
    out.effects |= DecisionExecute; out.repeatCount = repeat;
}
void Frame(BindingInstance& h,BindingDecision& out,Phase phase) noexcept {
    auto& frame = out.frames[out.frameCount++];
    frame.sequence = h.frameSequence++; frame.phase = phase;
    frame.logicalSeconds = static_cast<double>(h.lastQpc-h.started)/static_cast<double>(h.config.qpcFrequency);
    if (h.state == BindingState::Zoom) {
        frame.logicalSeconds = static_cast<double>((std::max)(h.lastQpc,h.pinch.now)-h.started)/static_cast<double>(h.config.qpcFrequency);
        frame.count = 2;
        frame.contacts[0] = {1,0,-h.pinch.radius,0}; frame.contacts[1] = {2,0,h.pinch.radius,0};
    } else {
        GestureFrame translation;
        translation.fingers = h.state == BindingState::ShellGesture ? h.config.shellFingers : 2;
        translation.sequence = frame.sequence; translation.phase = phase; translation.logicalSeconds = frame.logicalSeconds;
        if (phase != Phase::Begin) {
            if (h.state == BindingState::ShellGesture) {
                if (h.direction == Direction::Left || h.direction == Direction::Right) translation.offsetXmm = static_cast<double>(h.x)*h.config.shellMmPerCountX;
                else translation.offsetYmm = static_cast<double>(h.y)*h.config.shellMmPerCountY;
            } else {
                translation.offsetXmm = static_cast<double>(h.x)*h.config.mmPerCountX;
                translation.offsetYmm = static_cast<double>(h.y)*h.config.mmPerCountY;
            }
        }
        WmfTranslateContacts(&translation,&frame);
    }
    h.contact = phase != Phase::End && phase != Phase::Cancel;
}
void Cancel(BindingInstance& h,BindingDecision& out) noexcept {
    if (h.contact) Frame(h,out,Phase::Cancel);
    h.state = BindingState::Canceled; h.deadline = 0; h.wheelRemainder = 0;
    out.effects |= DecisionFinished | DecisionCanceled;
}
void Summary(const BindingInstance& h,BindingDecision& out) noexcept {
    out.state = h.state; out.rawX = h.x; out.rawY = h.y; out.direction = h.direction;
    if (!(out.effects & DecisionExecute)) {
        const auto reason = out.actionInput.reason;
        out.binding = h.active; out.action = h.active.action; out.actionInput = h.current;
        if (reason != CancelReason::None) out.actionInput.reason = reason;
    }
    out.targetPolicy = h.state == BindingState::ShellGesture || h.active.action == Action::ShellGestures ? TargetPolicy::InputDesktop : TargetPolicy::Application;
}
bool BeginPress(BindingInstance& h,const InputEvent& e) noexcept {
    h.current = e; h.lastSession = e.session; h.lastSequence = e.sequence; h.started = e.firstQpc; h.lastQpc = e.lastQpc;
    h.x = h.y = h.wheelRemainder = 0; h.frameSequence = 0; h.pinch.Reset(h.config,e.lastQpc);
    h.direction = Direction::None; h.active = {}; h.active.action = Action::None; h.contact = false; h.state = BindingState::Pending;
    h.deadline = 0;
    return !Find(h,Trigger::Hold) || Deadline(h,e.lastQpc,h.config.holdMs);
}
void Move(BindingInstance& h,BindingDecision& out,bool changed) noexcept {
    if (h.state == BindingState::Pending) {
        const auto* drag = Find(h,Trigger::Drag);
        if (!drag || std::hypot(static_cast<double>(h.x),static_cast<double>(h.y)) < h.config.thresholdCounts) return;
        if (drag->action == Action::ShellGestures) {
            const auto ax = std::abs(static_cast<double>(h.x)),ay = std::abs(static_cast<double>(h.y));
            if (ax >= ay*h.config.directionRatio) h.direction = h.x < 0 ? Direction::Left : Direction::Right;
            else if (ay >= ax*h.config.directionRatio) h.direction = h.y < 0 ? Direction::Up : Direction::Down;
            else return;
            h.state = BindingState::ShellGesture;
        } else h.state = BindingState::ContentDrag;
        h.active = *drag; h.deadline = 0;
        Frame(h,out,Phase::Begin); Frame(h,out,Phase::Update);
    } else if (changed && (h.state == BindingState::ContentDrag || h.state == BindingState::ShellGesture)) Frame(h,out,Phase::Update);
}
Result Process(BindingInstance& h,const InputEvent& e,BindingDecision& out) noexcept {
    if (e.flags & RecordedInput) return {};
    if (e.source == InputSource::OwnInjection || e.source == InputSource::OtherInjection) return {};
    if (e.kind == InputKind::Cancel) {
        if (e.source == InputSource::Physical && e.session && e.session != h.current.session) return {};
        Cancel(h,out); return {};
    }
    if (e.kind == InputKind::Tick) {
        if (h.state == BindingState::Zoom) {
            if (e.lastQpc < h.pinch.now) return {Status::InvalidState};
            if (h.deadline && e.lastQpc >= h.deadline) {
                h.pinch.Sample(e.lastQpc); Frame(h,out,Phase::Update); h.deadline = h.pinch.Deadline();
            }
            return {};
        }
        if (e.lastQpc < h.lastQpc) return {Status::InvalidState};
        h.lastQpc = e.lastQpc;
        if (!h.deadline || e.lastQpc < h.deadline) return {};
        if (h.state == BindingState::WaitingSecondClick) {
            Execute(out,Find(h,Trigger::Click,h.firstClick.button,h.firstClick.modifiers),h.firstClick);
            h.state = BindingState::Ended; out.effects |= DecisionFinished; h.deadline = 0;
        } else if (h.state == BindingState::Pending) {
            const auto* hold = Find(h,Trigger::Hold);
            if (hold) { h.active = *hold; h.state = BindingState::Held; Execute(out,hold,h.current); }
            h.deadline = 0;
        }
        return {};
    }
    if (e.kind == InputKind::Down) {
        if (h.state != BindingState::Idle && h.state != BindingState::Ended && h.state != BindingState::Canceled && h.state != BindingState::WaitingSecondClick)
            return {Status::InvalidState};
        if (e.session <= h.lastSession || e.revision != h.config.revision) return {Status::InvalidState};
        bool second{};
        if (h.state == BindingState::WaitingSecondClick) {
            const auto& first = h.firstClick;
            second = e.lastQpc <= h.deadline && e.button == first.button && e.modifiers == first.modifiers && e.target == first.target &&
                std::abs(static_cast<std::int64_t>(e.anchorX)-first.anchorX) <= h.config.doubleClickTolerancePixels &&
                std::abs(static_cast<std::int64_t>(e.anchorY)-first.anchorY) <= h.config.doubleClickTolerancePixels;
            if (!second && e.target == first.target) Execute(out,Find(h,Trigger::Click,first.button,first.modifiers),first);
        }
        h.second = second;
        if (!BeginPress(h,e)) return {Status::OutOfRange};
        out.effects |= DecisionStopInertia; return {};
    }
    if (h.state == BindingState::Idle || h.state == BindingState::Ended || h.state == BindingState::Canceled || h.state == BindingState::WaitingSecondClick) return {};
    if (e.session != h.current.session || e.generation != h.current.generation || e.revision != h.config.revision || e.button != h.current.button || e.modifiers != h.current.modifiers ||
        e.device != h.current.device || e.anchorX != h.current.anchorX || e.anchorY != h.current.anchorY || e.sequence <= h.lastSequence ||
        e.firstQpc < h.started || e.lastQpc < h.lastQpc || e.target != h.current.target) return {Status::InvalidState};
    h.lastSequence = e.sequence; h.lastQpc = e.lastQpc; h.current = e;
    if (e.kind == InputKind::Wheel) {
        if (h.state != BindingState::Pending && h.state != BindingState::Zoom && h.state != BindingState::Held) { Cancel(h,out); return {}; }
        const auto* zoom = Find(h,Trigger::Wheel);
        if (zoom) {
            if (!e.wheelY) return {};
            if (h.state != BindingState::Zoom) { h.active = *zoom; h.state = BindingState::Zoom; h.deadline = 0; Frame(h,out,Phase::Begin); }
            if (!h.pinch.Feed(e.wheelY,e.lastQpc)) return {Status::OutOfRange};
            const auto next = h.pinch.Deadline();
            h.deadline = h.deadline ? (std::min)(h.deadline,next) : next; return {};
        }
        if (!e.wheelY) return {};
        const auto* selected = Find(h,e.wheelY > 0 ? Trigger::WheelUp : Trigger::WheelDown);
        if (!selected) { Cancel(h,out); return {}; }
        h.active = *selected; h.state = BindingState::Held; h.deadline = 0;
        if (!Add(h.wheelRemainder,e.wheelY,h.wheelRemainder)) return {Status::OutOfRange};
        const auto steps = h.wheelRemainder/120;
        if (steps) {
            const auto* action = Find(h,steps > 0 ? Trigger::WheelUp : Trigger::WheelDown);
            if (!action) { Cancel(h,out); return {}; }
            const auto count = std::abs(steps);
            if (count > 32) return {Status::OutOfRange};
            Execute(out,action,e,static_cast<std::uint32_t>(count)); h.active = *action; h.state = BindingState::Held; h.deadline = 0;
            h.wheelRemainder %= 120;
        }
        return {};
    }
    if (e.kind != InputKind::Move && e.kind != InputKind::Up) return {Status::InvalidArgument};
    if (!Add(h.x,e.dx,h.x) || !Add(h.y,e.dy,h.y)) return {Status::OutOfRange};
    Move(h,out,e.dx || e.dy);
    if (e.kind != InputKind::Up) return {};
    if (h.contact) { Frame(h,out,Phase::End); h.state = BindingState::Ended; h.deadline = 0; }
    else if (h.state == BindingState::Held) { h.state = BindingState::Ended; h.deadline = 0; }
    else if (h.second) { Execute(out,Find(h,Trigger::DoubleClick),e); h.state = BindingState::Ended; h.deadline = 0; }
    else if (Find(h,Trigger::DoubleClick)) {
        h.firstClick = e; h.state = BindingState::WaitingSecondClick;
        if (!Deadline(h,e.lastQpc,h.config.doubleClickMs)) return {Status::OutOfRange};
    } else { Execute(out,Find(h,Trigger::Click),e); h.state = BindingState::Ended; h.deadline = 0; }
    out.effects |= DecisionFinished; return {};
}
}
}
extern "C" wmf::Result WmfBindingsCreate(const wmf::BindingConfig* c,wmf::BindingHandle* out) noexcept {
    using namespace wmf;
    if (!out || !c) return {Status::InvalidArgument};
    if (*out) return {Status::InvalidState};
    const auto validation = WmfValidateBindings(c,nullptr); if (!validation) return validation;
    *out = new (std::nothrow) BindingInstance{*c}; return *out ? Result{} : Result{Status::AllocationFailure};
}
extern "C" wmf::Result WmfBindingsFeed(wmf::BindingHandle h,const wmf::InputEvent* e,wmf::BindingDecision* out) noexcept {
    using namespace wmf;
    if (!h || !e || !out || out->size != sizeof(*out)) return {Status::InvalidArgument};
    if (out->version != AbiVersion) return {Status::VersionMismatch};
    const auto valid = WmfValidateInput(e); if (!valid) return valid;
    auto next = *h; BindingDecision decision;
    const auto result = Process(next,*e,decision); if (!result) return result;
    Summary(next,decision); *h = next; *out = decision; return {};
}
extern "C" wmf::Result WmfBindingsDeadline(wmf::BindingHandle h,std::uint64_t* out) noexcept {
    if (!h || !out) return {wmf::Status::InvalidArgument};
    *out = h->deadline; return {};
}
extern "C" wmf::Result WmfBindingsDestroy(wmf::BindingHandle* h) noexcept {
    if (!h) return {wmf::Status::InvalidArgument}; delete *h; *h = nullptr; return {};
}
