#pragma once
#include "ContentDragApi.h"
#include "SpscRing.h"
#include <limits>

namespace wmf::host {
class ContentInputBuffer {
public:
    bool Push(const MouseInput& e) noexcept { return queue_.Push(e); }
    bool Pop(MouseInput& e) noexcept {
        if (carried_) { e = carry_; carried_ = false; }
        else if (!queue_.Pop(e)) return false;
        if (e.kind != MouseInputKind::Move) return true;
        MouseInput next;
        while (queue_.Pop(next)) {
            if (!Merge(e,next)) { carry_ = next; carried_ = true; break; }
        }
        return true;
    }
    void Discard() noexcept { carried_ = false; MouseInput e; while (queue_.Pop(e)) {} }
    static bool Merge(MouseInput& a,const MouseInput& b) noexcept {
        if (a.kind != MouseInputKind::Move || b.kind != MouseInputKind::Move || a.session != b.session ||
            a.device != b.device || a.target != b.target || a.anchorX != b.anchorX || a.anchorY != b.anchorY ||
            b.sequence <= a.sequence || b.lastQpc < a.lastQpc ||
            !SameSign(a.dx,b.dx) || !SameSign(a.dy,b.dy) || !CanAdd(a.dx,b.dx) || !CanAdd(a.dy,b.dy) ||
            b.samples > UINT32_MAX-a.samples) return false;
        a.dx += b.dx; a.dy += b.dy; a.lastQpc = b.lastQpc; a.sequence = b.sequence; a.samples += b.samples;
        return true;
    }
private:
    static bool SameSign(std::int64_t a,std::int64_t b) noexcept { return !a || !b || (a < 0) == (b < 0); }
    static bool CanAdd(std::int64_t a,std::int64_t b) noexcept {
        return !(b > 0 && a > (std::numeric_limits<std::int64_t>::max)()-b) &&
            !(b < 0 && a < (std::numeric_limits<std::int64_t>::min)()-b);
    }
    SpscRing<MouseInput,512> queue_;
    MouseInput carry_;
    bool carried_{};
};
}
