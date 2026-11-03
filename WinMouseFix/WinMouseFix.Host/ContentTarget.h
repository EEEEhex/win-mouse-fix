#pragma once
#include "ContentDragApi.h"

namespace wmf::host {
enum TargetGuardFlag : std::uint32_t { TargetMissing = 1, ForegroundChanged = 2, CursorUnavailable = 4, AnchorChanged = 8 };
struct TargetSample {
    std::uint64_t foreground{};
    std::int32_t x{},y{};
    bool windowValid{},cursorKnown{};
};
inline std::uint32_t CheckTarget(const MouseInput& e,const TargetSample& actual) noexcept {
    std::uint32_t flags{};
    if (!e.target || !actual.windowValid) flags |= TargetMissing;
    if (e.target != actual.foreground) flags |= ForegroundChanged;
    if (!actual.cursorKnown) flags |= CursorUnavailable;
    else if (e.anchorX != actual.x || e.anchorY != actual.y) flags |= AnchorChanged;
    return flags;
}
}
