#include "ContentDragApi.h"
#include <cmath>
#include <limits>
#include <new>

namespace wmf {
struct DragInstance {
    DragConfig config;
    DragState state{DragState::Idle};
    std::uint64_t session{}, inputSequence{}, frameSequence{}, started{}, last{};
    std::int64_t x{}, y{};
};
namespace {
bool ValidConfig(const DragConfig& c) noexcept {
    return std::isfinite(c.thresholdCounts) && c.thresholdCounts > 0 && c.thresholdCounts <= 1000000 &&
        std::isfinite(c.mmPerCountX) && std::abs(c.mmPerCountX) >= 0.000001 && std::abs(c.mmPerCountX) <= 100 &&
        std::isfinite(c.mmPerCountY) && std::abs(c.mmPerCountY) >= 0.000001 && std::abs(c.mmPerCountY) <= 100 &&
        c.qpcFrequency > 0 && c.qpcFrequency <= 1000000000000ull;
}
bool Add(std::int64_t a, std::int64_t b, std::int64_t& out) noexcept {
    constexpr std::int64_t limit = 9007199254740991ll;
    if (b > limit || b < -limit || (b > 0 && a > limit-b) || (b < 0 && a < -limit-b)) return false;
    out = a+b; return true;
}
void Frame(DragInstance& d, DragOutput& o, Phase phase) noexcept {
    auto& f = o.frames[o.frameCount++];
    f.sequence = d.frameSequence++; f.phase = phase; f.fingers = 2;
    if (phase != Phase::Begin) {
        f.offsetXmm = static_cast<double>(d.x)*d.config.mmPerCountX;
        f.offsetYmm = static_cast<double>(d.y)*d.config.mmPerCountY;
    }
    f.logicalSeconds = static_cast<double>(d.last-d.started)/static_cast<double>(d.config.qpcFrequency);
}
}
}
extern "C" wmf::Result WmfDragCreate(const wmf::DragConfig* c, wmf::DragHandle* out) noexcept {
    using namespace wmf;
    if (!c || !out || c->size != sizeof(*c)) return {Status::InvalidArgument};
    if (c->version != AbiVersion) return {Status::VersionMismatch};
    if (*out) return {Status::InvalidState};
    if (!ValidConfig(*c)) return {Status::InvalidArgument};
    *out = new (std::nothrow) DragInstance{*c};
    return *out ? Result{} : Result{Status::AllocationFailure};
}
extern "C" wmf::Result WmfDragFeed(wmf::DragHandle h, const wmf::MouseInput* e, wmf::DragOutput* out) noexcept {
    using namespace wmf;
    if (!h || !e || !out || e->size != sizeof(*e) || out->size != sizeof(*out)) return {Status::InvalidArgument};
    if (e->version != AbiVersion || out->version != AbiVersion) return {Status::VersionMismatch};
    if (e->kind > MouseInputKind::Cancel || !e->session || !e->sequence || !e->firstQpc ||
        e->lastQpc < e->firstQpc || !e->samples) return {Status::InvalidArgument};
    auto next = *h;
    DragOutput o;
    if (e->kind == MouseInputKind::Down) {
        if (next.state != DragState::Idle || e->session <= next.session || e->dx || e->dy) return {Status::InvalidState};
        next.session = e->session; next.inputSequence = e->sequence;
        next.started = e->firstQpc; next.last = e->lastQpc;
        next.frameSequence = 0; next.x = next.y = 0; next.state = DragState::Pending;
    } else {
        if ((next.state != DragState::Pending && next.state != DragState::Active) || e->session != next.session ||
            e->sequence <= next.inputSequence || e->firstQpc < next.started || e->lastQpc < next.last) return {Status::InvalidState};
        next.last = e->lastQpc; next.inputSequence = e->sequence;
        if (e->kind == MouseInputKind::Cancel) {
            if (next.state == DragState::Active) Frame(next,o,Phase::Cancel);
            next.state = DragState::Canceled; o.effects = FinishedEffect | CanceledEffect;
        } else {
            if (!Add(next.x,e->dx,next.x) || !Add(next.y,e->dy,next.y)) return {Status::OutOfRange};
            const bool moved = e->dx || e->dy;
            if (next.state == DragState::Pending &&
                std::hypot(static_cast<double>(next.x),static_cast<double>(next.y)) >= next.config.thresholdCounts) {
                next.state = DragState::Active; Frame(next,o,Phase::Begin); Frame(next,o,Phase::Update);
            } else if (next.state == DragState::Active && moved) Frame(next,o,Phase::Update);
            if (e->kind == MouseInputKind::Up) {
                if (next.state == DragState::Active) Frame(next,o,Phase::End);
                else o.effects |= ClickEffect;
                next.state = DragState::Ended; o.effects |= FinishedEffect;
            }
        }
    }
    o.state = next.state; o.rawX = next.x; o.rawY = next.y;
    *h = next; *out = o; return {};
}
extern "C" wmf::Result WmfDragReset(wmf::DragHandle h) noexcept {
    if (!h) return {wmf::Status::InvalidArgument};
    if (h->state == wmf::DragState::Pending || h->state == wmf::DragState::Active) return {wmf::Status::InvalidState};
    h->state = wmf::DragState::Idle; return {};
}
extern "C" wmf::Result WmfDragDestroy(wmf::DragHandle* h) noexcept {
    if (!h) return {wmf::Status::InvalidArgument};
    delete *h; *h = nullptr; return {};
}
