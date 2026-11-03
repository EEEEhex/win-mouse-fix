#include "NativeApi.h"
#include <algorithm>
#include <cmath>
#include <new>

namespace wmf {
struct CoreInstance {
    TrajectoryConfig config;
    std::uint64_t sequence{};
    double lastTime{};
    double position{};
    double direction{1.0};
    bool begun{};
    bool held{};
    bool finished{};
    Phase pending{Phase::Update};
};
namespace {
Result Invalid() noexcept { return {Status::InvalidArgument}; }
double Progress(Profile profile, double t) noexcept {
    switch (profile) {
    case Profile::PauseReverse:
        if (t < 0.4) return t / 0.4;
        if (t <= 0.6) return 1.0;
        return (1.0 - t) / 0.4;
    case Profile::SmallSteps: return std::floor(t * 100.0) / 100.0;
    case Profile::SlowRelease: return 1.0 - (1.0 - t) * (1.0 - t);
    case Profile::StartThenMicro:
        // Explicit diagnostic control: 15/16 approach, hold, 1/16 probe, hold.
        if (t < 0.125) return (15.0/16.0) * t/0.125;
        if (t <= 0.5) return 15.0/16.0;
        if (t < 0.75) return 15.0/16.0 + (t-0.5)/0.25/16.0;
        return 1.0;
    default: return t;
    }
}
}
}

extern "C" std::uint32_t WmfCoreVersion() noexcept { return wmf::AbiVersion; }

extern "C" wmf::Result WmfCoreCreate(const wmf::TrajectoryConfig* config, wmf::CoreHandle* out) noexcept {
    using namespace wmf;
    if (!out) return Invalid();
    if (*out) return {Status::InvalidState};
    if (!config || config->size != sizeof(TrajectoryConfig)) return Invalid();
    if (config->version != AbiVersion) return {Status::VersionMismatch};
    if (config->fingers < 2 || config->fingers > 4 || config->interactive > 1 ||
        config->profile > Profile::StartThenMicro || !std::isfinite(config->durationSeconds) ||
        config->durationSeconds < 0.05 || config->durationSeconds > 60.0 ||
        !std::isfinite(config->extentXmm) || !std::isfinite(config->extentYmm)) return Invalid();
    if (config->profile == Profile::StartThenMicro && (config->interactive || config->fingers != 2)) return Invalid();
    *out = new (std::nothrow) CoreInstance{*config};
    return *out ? Result{} : Result{Status::AllocationFailure};
}

extern "C" wmf::Result WmfCoreNext(wmf::CoreHandle h, double elapsed, wmf::GestureFrame* out) noexcept {
    using namespace wmf;
    if (!h || !out || out->size != sizeof(GestureFrame)) return Invalid();
    if (out->version != AbiVersion) return {Status::VersionMismatch};
    if (h->finished) return {Status::InvalidState};
    if (!std::isfinite(elapsed) || elapsed < h->lastTime || elapsed < 0) return Invalid();
    GestureFrame frame;
    frame.fingers = h->config.fingers;
    frame.sequence = h->sequence;
    frame.logicalSeconds = elapsed;
    if (!h->begun) {
        frame.phase = h->pending == Phase::Cancel ? Phase::Cancel : Phase::Begin;
        h->begun = true;
    } else if (h->pending == Phase::End || h->pending == Phase::Cancel) {
        frame.phase = h->pending;
    } else {
        if (h->config.interactive) {
            if (!h->held) h->position += h->direction * (elapsed - h->lastTime) / h->config.durationSeconds;
        } else {
            h->position = Progress(h->config.profile, std::clamp(elapsed / h->config.durationSeconds, 0.0, 1.0));
        }
        frame.phase = !h->config.interactive && elapsed >= h->config.durationSeconds ? Phase::End : Phase::Update;
    }
    frame.offsetXmm = h->position * h->config.extentXmm;
    frame.offsetYmm = h->position * h->config.extentYmm;
    if (!std::isfinite(frame.offsetXmm) || !std::isfinite(frame.offsetYmm)) return {Status::OutOfRange};
    h->lastTime = elapsed;
    h->sequence++;
    h->finished = frame.phase == Phase::End || frame.phase == Phase::Cancel;
    *out = frame;
    return {};
}

extern "C" wmf::Result WmfCoreControl(wmf::CoreHandle h, wmf::Control command) noexcept {
    using namespace wmf;
    if (!h) return Invalid();
    if (h->finished) return {Status::InvalidState};
    if (command == Control::Cancel) { h->pending = Phase::Cancel; return {}; }
    if (!h->begun || h->pending == Phase::End || h->pending == Phase::Cancel) return {Status::InvalidState};
    if (command == Control::Release) { h->pending = Phase::End; return {}; }
    if (!h->config.interactive) return {Status::InvalidState};
    switch (command) {
    case Control::HoldMotion: h->held = true; break;
    case Control::Advance: h->held = false; break;
    case Control::Reverse: h->direction = -h->direction; h->held = false; break;
    default: return Invalid();
    }
    return {};
}

extern "C" wmf::Result WmfCoreDestroy(wmf::CoreHandle* h) noexcept {
    if (!h) return {wmf::Status::InvalidArgument};
    delete *h;
    *h = nullptr;
    return {};
}
