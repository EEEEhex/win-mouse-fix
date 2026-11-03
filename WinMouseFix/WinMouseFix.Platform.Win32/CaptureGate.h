#pragma once
#include "ProductApi.h"
#include <array>
#include <atomic>
#include <algorithm>

namespace wmf::platform {
enum class CaptureMetric : unsigned { RawPackets, RawMoves, BlockedMoves, CapturedDowns, RawDowns, RawUps,
    ReplayCalls, ReplayFailures, Cancellations, HookCalls, HookTicks, HookMaxTicks, RawWheels, BlockedWheels, InjectedWheelHooks, KeyboardTakeovers, Count };
class CaptureCounters {
public:
    void Add(CaptureMetric m, std::uint64_t n = 1) noexcept { values_[static_cast<unsigned>(m)].fetch_add(n,std::memory_order_relaxed); }
    void Maximum(CaptureMetric m, std::uint64_t n) noexcept {
        auto& a = values_[static_cast<unsigned>(m)]; auto old = a.load(std::memory_order_relaxed);
        while (old < n && !a.compare_exchange_weak(old,n,std::memory_order_relaxed)) {}
    }
    CaptureStats Read() const noexcept {
        CaptureStats s;
        s.rawPackets = Get(0); s.rawMoves = Get(1); s.blockedMoves = Get(2); s.capturedDowns = Get(3);
        s.rawDowns = Get(4); s.rawUps = Get(5); s.replayCalls = Get(6); s.replayFailures = Get(7);
        s.cancellations = Get(8); s.hookCalls = Get(9); s.hookTicks = Get(10); s.hookMaxTicks = Get(11);
        s.rawWheels = Get(12); s.blockedWheels = Get(13); s.injectedWheelHooks = Get(14); s.keyboardTakeovers = Get(15);
        return s;
    }
private:
    std::uint64_t Get(unsigned i) const noexcept { return values_[i].load(std::memory_order_relaxed); }
    std::array<std::atomic<std::uint64_t>,static_cast<unsigned>(CaptureMetric::Count)> values_{};
};
struct CaptureTarget { std::uint64_t window{}; std::int32_t x{}, y{}; };
struct RawMousePacket {
    std::uint64_t device{}, qpc{};
    std::int64_t dx{}, dy{};
    bool down{}, up{}, unsupported{}, other{};
    std::int64_t wheelX{},wheelY{};
    Button button{Button::Back};
};
struct CaptureServices {
    void* context{};
    bool (*emit)(void*, const MouseInput&) noexcept{};
    bool (*targetValid)(void*, const CaptureTarget&) noexcept{};
    Result (*replay)(void*) noexcept{};
    void (*fault)(void*, Result) noexcept{};
};
// Owner-thread policy. It has no global hooks, windows or real input side effects.
class CaptureGate {
public:
    CaptureGate(CaptureServices services, CaptureCounters& counters, std::uint64_t timeout) noexcept
        : io_(services), counters_(counters), timeout_(timeout) {}
    void Enable(bool value, bool alreadyHeld, std::uint64_t now) noexcept {
        if (!value) { pendingEnable_ = false; enabled_ = false; Cancel(CancelReason::Pause,now); return; }
        if (owned_) { pendingEnable_ = true; return; }
        if (enabled_) return;
        enabled_ = true; held_ = passThrough_ = alreadyHeld; preCount_ = 0;
    }
    bool Down(CaptureTarget target, std::uint64_t now,Button button = Button::Back,bool freeze = true) noexcept {
        if (held_) return owned_ && !passThrough_;
        held_ = true;
        if (!enabled_ || owned_ || !target.window) { passThrough_ = true; preCount_ = 0; return false; }
        passThrough_ = false;
        owned_ = true; frozen_ = freeze; button_ = button; rawStarted_ = rawEnded_ = released_ = complete_ = click_ = canceled_ = false;
        target_ = target; ++session_; sequence_ = 0; device_ = 0; deadline_ = now+timeout_;
        counters_.Add(CaptureMetric::CapturedDowns);
        const auto count = preCount_; preCount_ = 0;
        if (count && now >= pre_[0].qpc && now-pre_[0].qpc <= timeout_) {
            if (pre_[0].button == button) for (unsigned i = 0; i < count && !canceled_; ++i) Process(pre_[i]);
        }
        return true;
    }
    bool Up(std::uint64_t now) noexcept {
        held_ = false; preCount_ = 0;
        if (passThrough_) { passThrough_ = false; return false; }
        if (!owned_) return false;
        released_ = true; deadline_ = now+timeout_;
        if (canceled_) Clear(); else Finish(now);
        return true;
    }
    bool Move() noexcept {
        if (frozen_) { counters_.Add(CaptureMetric::BlockedMoves); return true; }
        return false;
    }
    void Raw(const RawMousePacket& packet) noexcept {
        counters_.Add(CaptureMetric::RawPackets);
        if (packet.dx || packet.dy) counters_.Add(CaptureMetric::RawMoves);
        if (packet.down) counters_.Add(CaptureMetric::RawDowns);
        if (packet.up) counters_.Add(CaptureMetric::RawUps);
        if (owned_) {
            if (canceled_) {
                // The pointer is already free, but a real raw Up still lets us
                // retire ownership if its hook notification was lost.
                if (packet.up && packet.button == button_ && !packet.unsupported && packet.device && (!device_ || packet.device == device_)) {
                    rawEnded_ = true; deadline_ = packet.qpc+timeout_;
                }
            } else Process(packet);
            return;
        }
        // Raw messages may precede the synchronous hook. Buffer a bounded envelope,
        // never arbitrary idle motion and never a press already passed through.
        if (!enabled_ || held_ || !packet.device || packet.unsupported) return;
        if (packet.down) { preCount_ = 0; pre_[preCount_++] = packet; }
        else if (preCount_) {
            if (packet.device != pre_[0].device || packet.qpc < pre_[0].qpc || packet.qpc-pre_[0].qpc > timeout_) { preCount_ = 0; return; }
            if (preCount_ == pre_.size()) { preCount_ = 0; return; }
            pre_[preCount_++] = packet;
        }
    }
    void Complete(std::uint64_t session, bool click, std::uint64_t now) noexcept {
        if (!owned_ || canceled_ || session != session_) return;
        complete_ = true; click_ = click; Finish(now);
    }
    void Cancel(CancelReason reason, std::uint64_t now) noexcept {
        preCount_ = 0; frozen_ = false;
        if (!owned_ || canceled_) return;
        canceled_ = true; counters_.Add(CaptureMetric::Cancellations);
        Emit(MouseInputKind::Cancel,now,0,0,reason);
        if (!held_ || released_) Clear();
    }
    void Tick(std::uint64_t now, bool targetStillValid) noexcept {
        if (!owned_) return;
        if (rawEnded_ && !released_ && now >= deadline_) {
            held_ = false; released_ = true; Cancel(CancelReason::IncompleteInput,now);
        }
        if (!owned_) return;
        if (!targetStillValid) Cancel(CancelReason::TargetChanged,now);
        if (!canceled_ && ((!rawStarted_) || (released_ && (!rawEnded_ || !complete_))) && now >= deadline_)
            Cancel(CancelReason::IncompleteInput,now);
        if (canceled_ && !held_) Clear();
    }
    bool Owned() const noexcept { return owned_; }
    bool Frozen() const noexcept { return frozen_; }
    bool Enabled() const noexcept { return enabled_; }
    bool Held() const noexcept { return held_; }
    bool Canceled() const noexcept { return canceled_; }
    void DropPrebuffer() noexcept { preCount_ = 0; }
    // A confirmed input-desktop boundary ends ownership on that desktop; an
    // asynchronous key-state sample during a captured press never does.
    void DesktopEnded(std::uint64_t now) noexcept { Cancel(CancelReason::Pause,now); held_ = passThrough_ = false; Clear(); enabled_ = false; }
    void DeviceEnded(std::uint64_t now) noexcept { Cancel(CancelReason::DeviceRemoved,now); held_ = passThrough_ = false; Clear(); }
    std::uint64_t Session() const noexcept { return session_; }
    std::uint64_t Device() const noexcept { return device_; }
    const CaptureTarget& Target() const noexcept { return target_; }
private:
    void Emit(MouseInputKind kind, std::uint64_t qpc, std::int64_t dx = 0, std::int64_t dy = 0,
        CancelReason reason = CancelReason::None) noexcept {
        MouseInput e; e.kind = kind; e.reason = reason; e.session = session_; e.sequence = ++sequence_;
        e.firstQpc = e.lastQpc = qpc; e.device = device_; e.target = target_.window;
        e.dx = dx; e.dy = dy; e.anchorX = target_.x; e.anchorY = target_.y;
        if (!io_.emit(io_.context,e) && kind != MouseInputKind::Cancel) {
            enabled_ = false; Cancel(CancelReason::Overflow,qpc);
            if (io_.fault) io_.fault(io_.context,{Status::Busy});
        }
    }
    void Process(const RawMousePacket& p) noexcept {
        if (!owned_ || canceled_) return;
        if (p.unsupported || !p.device || (p.down && p.up)) { Cancel(CancelReason::UnsupportedInput,p.qpc); return; }
        if (p.other) { Cancel(CancelReason::OtherInput,p.qpc); return; }
        if ((p.down || p.up) && p.button != button_) { Cancel(CancelReason::OtherInput,p.qpc); return; }
        if (p.down && rawStarted_ && p.device != device_) { Cancel(CancelReason::OtherInput,p.qpc); return; }
        if (p.down && !rawStarted_) {
            device_ = p.device; rawStarted_ = true; Emit(MouseInputKind::Down,p.qpc);
        }
        if (!rawStarted_ || p.device != device_ || rawEnded_ || canceled_) return;
        if (p.dx || p.dy) Emit(MouseInputKind::Move,p.qpc,p.dx,p.dy);
        if (p.wheelX || p.wheelY) Emit(MouseInputKind::Wheel,p.qpc,p.wheelX,p.wheelY);
        if (p.up && !canceled_) { rawEnded_ = true; deadline_ = p.qpc+timeout_; Emit(MouseInputKind::Up,p.qpc); Finish(p.qpc); }
    }
    void Finish(std::uint64_t now) noexcept {
        if (!owned_ || canceled_ || !complete_ || !released_ || !rawEnded_) return;
        if (click_) {
            if (!io_.targetValid(io_.context,target_)) { Cancel(CancelReason::TargetChanged,now); return; }
            counters_.Add(CaptureMetric::ReplayCalls);
            const auto result = io_.replay(io_.context);
            if (!result) {
                enabled_ = false; counters_.Add(CaptureMetric::ReplayFailures);
                Cancel(CancelReason::NativeError,now);
                if (io_.fault) io_.fault(io_.context,result);
                return;
            }
        }
        Clear();
    }
    void Clear() noexcept {
        owned_ = frozen_ = rawStarted_ = rawEnded_ = released_ = complete_ = click_ = canceled_ = false;
        preCount_ = 0; device_ = 0; target_ = {};
        if (pendingEnable_) { enabled_ = true; pendingEnable_ = false; }
    }
    CaptureServices io_;
    CaptureCounters& counters_;
    std::uint64_t timeout_{}, session_{}, sequence_{}, device_{}, deadline_{};
    CaptureTarget target_;
    Button button_{Button::Back};
    bool enabled_{}, held_{}, owned_{}, frozen_{}, rawStarted_{}, rawEnded_{}, released_{}, complete_{}, click_{}, canceled_{};
    bool pendingEnable_{};
    bool passThrough_{};
    std::array<RawMousePacket,32> pre_{};
    unsigned preCount_{};
};
}
