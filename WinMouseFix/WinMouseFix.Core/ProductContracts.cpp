#include "ProductApi.h"
#include <cmath>
#include <initializer_list>

namespace {
using namespace wmf;
bool SignedScale(double v) noexcept { return std::isfinite(v) && std::abs(v) >= 0.000001 && std::abs(v) <= 100; }
bool Between(double v,double lo,double hi) noexcept { return std::isfinite(v) && v >= lo && v <= hi; }
Result Invalid(ValidationError* out,ValidationField field,std::uint32_t i = UINT32_MAX,std::uint32_t other = UINT32_MAX,
    Status code = Status::InvalidArgument) noexcept {
    if (out) { out->field = static_cast<std::uint32_t>(field); out->bindingIndex = i; out->conflictingIndex = other; }
    return {code};
}
bool Compatible(Trigger trigger,Action action) noexcept {
    if (action == Action::None || action > Action::NextTrack || trigger > Trigger::WheelDown) return false;
    if (trigger == Trigger::Drag) return action == Action::ContentDrag || action == Action::ShellGestures;
    if (trigger == Trigger::Wheel) return action == Action::NativeZoom;
    return action != Action::ContentDrag && action != Action::ShellGestures && action != Action::NativeZoom;
}
bool WheelTrigger(Trigger t) noexcept { return t == Trigger::Wheel || t == Trigger::WheelUp || t == Trigger::WheelDown; }
bool Conflicts(const Binding& a,const Binding& b) noexcept {
    if (a.id == b.id) return true;
    if (a.button != b.button || a.modifiers != b.modifiers) return false;
    return a.trigger == b.trigger || (WheelTrigger(a.trigger) && WheelTrigger(b.trigger) && (a.trigger == Trigger::Wheel || b.trigger == Trigger::Wheel));
}
}
extern "C" wmf::Result WmfValidateBindings(const wmf::BindingConfig* c,wmf::ValidationError* error) noexcept {
    using namespace wmf;
    if (error && error->size != sizeof(*error)) return {Status::InvalidArgument};
    if (error && error->version != AbiVersion) return {Status::VersionMismatch};
    if (error) *error = {};
    if (!c || c->size != sizeof(*c)) return Invalid(error,ValidationField::Layout);
    if (c->version != AbiVersion) return Invalid(error,ValidationField::Version,UINT32_MAX,UINT32_MAX,Status::VersionMismatch);
    if (!c->revision || c->revision > 9007199254740991ull || !c->qpcFrequency || c->qpcFrequency > 1000000000000ull ||
        !Between(c->thresholdCounts,0.000001,1000000) || !SignedScale(c->mmPerCountX) || !SignedScale(c->mmPerCountY) ||
        !SignedScale(c->shellMmPerCountX) || !SignedScale(c->shellMmPerCountY) || !Between(c->directionRatio,1.01,8) ||
        !SignedScale(c->wheelMmPerNotch) || !Between(c->regularMilliseconds,10,500) || !Between(c->highMilliseconds,c->regularMilliseconds,1000) ||
        !SignedScale(c->zoomMmPerNotch) || !Between(c->initialPinchMm,1,100) || c->doubleClickMs < 100 || c->doubleClickMs > 2000 ||
        c->holdMs < 100 || c->holdMs > 5000 || (c->shellFingers != 3 && c->shellFingers != 4) ||
        c->wheelMode > WheelMode::High || c->bindingCount > MaximumBindings || c->doubleClickTolerancePixels > 100 || c->reserved[0] || c->reserved[1])
        return Invalid(error,ValidationField::Range);
    for (std::uint32_t i = 0; i < c->bindingCount; ++i) {
        const auto& b = c->bindings[i];
        if (!b.id || b.button < Button::Middle || b.button > Button::Forward || (b.modifiers & ~AllModifiers) ||
            !Compatible(b.trigger,b.action) || b.reserved[0] || b.reserved[1]) return Invalid(error,ValidationField::Binding,i);
        if (b.keyCount > MaximumShortcutKeys || (b.action == Action::Shortcut ? !b.keyCount : b.keyCount != 0))
            return Invalid(error,ValidationField::Shortcut,i);
        for (std::uint32_t k = 0; k < MaximumShortcutKeys; ++k) {
            if (k < b.keyCount) {
                if (b.keys[k] < 8 || b.keys[k] > 254) return Invalid(error,ValidationField::Shortcut,i);
                for (std::uint32_t j = 0; j < k; ++j) if (b.keys[k] == b.keys[j]) return Invalid(error,ValidationField::Shortcut,i);
            } else if (b.keys[k]) return Invalid(error,ValidationField::Shortcut,i);
        }
        for (std::uint32_t j = 0; j < i; ++j) if (Conflicts(b,c->bindings[j])) return Invalid(error,ValidationField::Conflict,i,j);
    }
    return {};
}
extern "C" wmf::Result WmfValidateInput(const wmf::InputEvent* e) noexcept {
    using namespace wmf;
    if (!e || e->size != sizeof(*e)) return {Status::InvalidArgument};
    if (e->version != AbiVersion) return {Status::VersionMismatch};
    if (!e->generation || e->kind > InputKind::Tick || e->button > Button::Forward || (e->modifiers & ~AllModifiers) ||
        (e->source != InputSource::Physical && e->source != InputSource::OwnInjection && e->source != InputSource::OtherInjection && e->source != InputSource::Control) ||
        e->reason > CancelReason::StaleInput || !e->samples || e->lastQpc < e->firstQpc || (e->flags & ~RecordedInput) || e->reserved) return {Status::InvalidArgument};
    if (e->kind != InputKind::Tick && e->kind != InputKind::Cancel && (!e->session || !e->sequence || !e->firstQpc || !e->device || !e->target || !e->revision)) return {Status::InvalidArgument};
    if ((e->kind == InputKind::Down || e->kind == InputKind::Up) && e->button == Button::None) return {Status::InvalidArgument};
    constexpr std::int64_t limit = 9007199254740991ll;
    for (auto v : {e->dx,e->dy,e->wheelX,e->wheelY}) if (v < -limit || v > limit) return {Status::OutOfRange};
    if (e->kind == InputKind::Down && (e->dx || e->dy || e->wheelX || e->wheelY)) return {Status::InvalidArgument};
    if (e->kind != InputKind::Wheel && (e->wheelX || e->wheelY)) return {Status::InvalidArgument};
    return {};
}
extern "C" wmf::Result WmfTranslateContacts(const wmf::GestureFrame* f,wmf::ContactFrame* out) noexcept {
    using namespace wmf;
    if (!f || !out || f->size != sizeof(*f) || out->size != sizeof(*out)) return {Status::InvalidArgument};
    if (f->version != AbiVersion || out->version != AbiVersion) return {Status::VersionMismatch};
    if (f->fingers < 2 || f->fingers > 4 || f->phase > Phase::Cancel || !std::isfinite(f->offsetXmm) ||
        !std::isfinite(f->offsetYmm) || !std::isfinite(f->logicalSeconds) || f->logicalSeconds < 0) return {Status::InvalidArgument};
    ContactFrame frame; frame.sequence = f->sequence; frame.phase = f->phase; frame.count = f->fingers; frame.logicalSeconds = f->logicalSeconds;
    for (std::uint32_t i = 0; i < frame.count; ++i) {
        frame.contacts[i] = {i+1,0,f->offsetXmm+(static_cast<double>(i)-(frame.count-1)/2.0)*10.0,f->offsetYmm};
    }
    *out = frame; return {};
}
