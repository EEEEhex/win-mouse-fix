#pragma once
#include "ProductApi.h"
#include "SpscRing.h"
#include <limits>

namespace wmf::host {
// One capture producer and one worker consumer. Control/IPC never push here.
class ProductInputBuffer {
public:
    bool Push(const InputEvent& event) noexcept { return queue_.Push(event); }
    bool Pop(InputEvent& event) noexcept {
        if (carried_) { event = carry_; carried_ = false; }
        else if (!queue_.Pop(event)) return false;
        if (event.kind != InputKind::Move) return true;
        InputEvent next;
        while (queue_.Pop(next)) if (!Merge(event,next)) { carry_ = next; carried_ = true; break; }
        return true;
    }
    void Discard() noexcept { carried_ = false; InputEvent e; while (queue_.Pop(e)) {} }
    static bool Merge(InputEvent& a,const InputEvent& b) noexcept {
        if (a.kind != InputKind::Move || b.kind != InputKind::Move || a.session != b.session || a.device != b.device ||
            a.button != b.button || a.modifiers != b.modifiers || a.revision != b.revision || a.generation != b.generation || a.source != b.source || a.flags != b.flags ||
            a.target != b.target || a.anchorX != b.anchorX || a.anchorY != b.anchorY || b.sequence <= a.sequence || b.lastQpc < a.lastQpc ||
            b.firstQpc < a.firstQpc || !Compatible(a.dx,b.dx) || !Compatible(a.dy,b.dy) || b.samples > UINT32_MAX-a.samples) return false;
        a.dx += b.dx; a.dy += b.dy; a.sequence = b.sequence; a.lastQpc = b.lastQpc; a.samples += b.samples; return true;
    }
private:
    static bool Compatible(std::int64_t a,std::int64_t b) noexcept {
        return (!a || !b || (a < 0) == (b < 0)) &&
            !(b > 0 && a > (std::numeric_limits<std::int64_t>::max)()-b) && !(b < 0 && a < (std::numeric_limits<std::int64_t>::min)()-b);
    }
    SpscRing<InputEvent,512> queue_;
    InputEvent carry_;
    bool carried_{};
};
}
