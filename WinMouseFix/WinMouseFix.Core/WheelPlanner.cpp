#include "WheelApi.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <new>

namespace wmf {
namespace {
struct Axis {
    double position{},origin{},goal{},velocity{},initialVelocity{},accepted{},discarded{};
    std::uint64_t start{},end{};
    void Sample(std::uint64_t now,std::uint64_t frequency) noexcept {
        if (end <= start || now >= end) { position = goal; velocity = 0; return; }
        const double u = static_cast<double>(now-start)/static_cast<double>(end-start);
        const double seconds = static_cast<double>(end-start)/static_cast<double>(frequency);
        const double distance = goal-origin;
        position = origin+distance*(3*u*u-2*u*u*u)+initialVelocity*seconds*(u*u*u-2*u*u+u);
        velocity = distance*(6*u-6*u*u)/seconds+initialVelocity*(3*u*u-4*u+1);
    }
    void Feed(double delta,std::uint64_t now,std::uint64_t duration,std::uint64_t frequency) noexcept {
        if (!delta) { Sample(now,frequency); return; }
        const bool reverse = (goal-position)*delta < 0;
        if (reverse) { discarded += goal-position; goal = position; velocity = 0; }
        else Sample(now,frequency);
        accepted += delta; goal += delta; origin = position; start = now; end = now+duration;
        const double distance = goal-position;
        const double seconds = static_cast<double>(duration)/static_cast<double>(frequency);
        // A monotone Hermite segment preserves velocity within the new bound;
        // there is never a queue of historic impulses to play after input stops.
        initialVelocity = distance == 0 ? 0 : std::copysign((std::min)(std::abs(velocity),3*std::abs(distance)/seconds),distance);
    }
};
bool OutputValid(const WheelSample* out) noexcept { return out && out->size == sizeof(WheelSample) && out->version == AbiVersion; }
}
struct WheelInstance {
    BindingConfig config;
    Axis x,y;
    std::uint64_t now{},duration{},step{};
    void Output(WheelSample& out) const noexcept {
        out = {}; out.xMm = x.position; out.yMm = y.position; out.acceptedX = x.accepted; out.acceptedY = y.accepted;
        out.discardedX = x.discarded; out.discardedY = y.discarded;
        const auto end = (std::max)(x.end,y.end); out.active = end > now;
        if (out.active) out.nextQpc = (std::min)(end,now+step);
    }
};
}
extern "C" wmf::Result WmfWheelCreate(const wmf::BindingConfig* c,wmf::WheelHandle* out) noexcept {
    using namespace wmf;
    if (!out || *out || !c) return {Status::InvalidArgument};
    const auto valid = WmfValidateBindings(c,nullptr); if (!valid) return valid;
    if (c->wheelMode == WheelMode::Off) return {Status::InvalidState};
    auto instance = new(std::nothrow) WheelInstance; if (!instance) return {Status::NativeFailure};
    instance->config = *c;
    const auto ms = c->wheelMode == WheelMode::Regular ? c->regularMilliseconds : c->highMilliseconds;
    instance->duration = (std::max)(1ull,static_cast<std::uint64_t>(ms*static_cast<double>(c->qpcFrequency)/1000));
    instance->step = (std::max)(1ull,c->qpcFrequency/240); *out = instance; return {};
}
extern "C" wmf::Result WmfWheelFeed(wmf::WheelHandle handle,const wmf::InputEvent* e,std::uint64_t now,wmf::WheelSample* out) noexcept {
    using namespace wmf;
    if (!handle || !e || !OutputValid(out)) return {Status::InvalidArgument};
    const auto valid = WmfValidateInput(e); if (!valid) return valid;
    if (e->kind != InputKind::Wheel || e->button != Button::None || e->modifiers || e->source != InputSource::Physical || e->flags ||
        e->revision != handle->config.revision || now < e->lastQpc || now < handle->now ||
        now > UINT64_MAX-handle->duration || (!e->wheelX && !e->wheelY)) return {Status::InvalidArgument};
    const double scale = handle->config.wheelMmPerNotch/120;
    const double x = static_cast<double>(e->wheelX)*scale,y = static_cast<double>(e->wheelY)*scale;
    // Range errors leave the planner unchanged, including its ledger.
    if (!std::isfinite(x) || !std::isfinite(y) || std::abs(x) > 1000000 || std::abs(y) > 1000000 ||
        std::abs(handle->x.accepted+x) > 1e12 || std::abs(handle->y.accepted+y) > 1e12) return {Status::OutOfRange};
    handle->x.Feed(x,now,handle->duration,handle->config.qpcFrequency);
    handle->y.Feed(y,now,handle->duration,handle->config.qpcFrequency);
    handle->now = now; handle->Output(*out); return {};
}
extern "C" wmf::Result WmfWheelSample(wmf::WheelHandle handle,std::uint64_t now,wmf::WheelSample* out) noexcept {
    using namespace wmf;
    if (!handle || !OutputValid(out) || now < handle->now || now > UINT64_MAX-handle->step) return {Status::InvalidArgument};
    handle->x.Sample(now,handle->config.qpcFrequency); handle->y.Sample(now,handle->config.qpcFrequency);
    handle->now = now; handle->Output(*out); return {};
}
extern "C" wmf::Result WmfWheelDestroy(wmf::WheelHandle* handle) noexcept {
    if (!handle) return {wmf::Status::InvalidArgument}; delete *handle; *handle = nullptr; return {};
}
