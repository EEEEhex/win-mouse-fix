#pragma once
#include <Windows.h>
#include "CaptureGate.h"

namespace wmf::platform {
inline constexpr ULONG_PTR ReplayMarker = 0x574d4632;
struct CaptureNative {
    void* context{};
    HWND (*createWindow)(void*,void*){};
    BOOL (*registerRaw)(void*,HWND,bool){};
    HHOOK (*installHook)(void*){};
    BOOL (*removeHook)(void*,HHOOK){};
    BOOL (*destroyWindow)(void*,HWND){};
    BOOL (*timer)(void*,HWND,bool){};
    UINT (*send)(void*,UINT,const INPUT*){};
    DWORD (*error)(void*){};
    BOOL (*registerKeyboard)(void*,HWND,bool){};
    CaptureTarget (*target)(void*){};
    bool (*validTarget)(void*,const CaptureTarget&,TargetPolicy,bool){};
    std::uint32_t (*modifiers)(void*){};
    std::uint32_t (*heldButtons)(void*){};
    bool (*desktopActive)(void*){};
    BOOL (*post)(void*,HWND,UINT,WPARAM,LPARAM){};
};
CaptureNative RealCaptureNative() noexcept;
Result ReplayButtons(const CaptureNative&) noexcept;
class MouseCapture {
public:
    explicit MouseCapture(CaptureConfig, CaptureNative = RealCaptureNative()) noexcept;
    explicit MouseCapture(ProductCaptureConfig,CaptureNative = RealCaptureNative()) noexcept;
    ~MouseCapture() { delete pendingBindings_.exchange(nullptr); }
    Result Open() noexcept;
    Result Close() noexcept;
    void Command(CaptureCommand,std::uint64_t) noexcept;
    void Raw(const RAWINPUT&) noexcept;
    bool Hook(WPARAM,const MSLLHOOKSTRUCT&) noexcept;
    void Tick() noexcept;
    void Removed(HANDLE) noexcept;
    void Fault(Result) noexcept;
    Result Post(CaptureCommand,std::uint64_t) noexcept;
    CaptureStats Stats() const noexcept { return counters_.Read(); }
    HWND Window() const noexcept { return window_; }
    Result UpdateBindings(const BindingConfig&) noexcept;
    Result Policy(std::uint64_t,TargetPolicy) noexcept;
    Result Recording(bool) noexcept;
    std::uint64_t AppliedRevision() const noexcept { return appliedRevision_.load(); }
    Result Revisions(CaptureRevision&) const noexcept;
    void ApplyPendingBindings() noexcept;
    bool Closed() const noexcept { return !window_ && !hook_; }
    static LRESULT CALLBACK WindowProc(HWND,UINT,WPARAM,LPARAM) noexcept;
    static LRESULT CALLBACK HookProc(int,WPARAM,LPARAM) noexcept;
private:
    std::uint64_t Now() const noexcept;
    void RefreshTarget() noexcept;
    bool ValidTarget(const CaptureTarget&) const noexcept;
    Result Replay() noexcept;
    bool Forward(const MouseInput&) noexcept;
    CaptureTarget CurrentTarget() const noexcept;
    std::uint32_t Modifiers() const noexcept;
    std::uint32_t HeldButtons() const noexcept;
    bool DesktopActive() const noexcept;
    bool Bound(Button,std::uint32_t) const noexcept;
    bool WheelEligible(std::int64_t) const noexcept;
    bool ProductHook(WPARAM,const MSLLHOOKSTRUCT&) noexcept;
    void CancelWheel(CancelReason,std::uint64_t) noexcept;
    void ProductRaw(const RAWINPUT&) noexcept;
    CaptureConfig config_;
    CaptureNative api_;
    DWORD owner_{};
    HWND window_{};
    HHOOK hook_{};
    bool registered_{}, timer_{}, foregroundAllowed_{}, registrationLockOwned_{};
    HWND cachedForeground_{};
    DWORD integrity_{};
    std::uint64_t frequency_{};
    CaptureCounters counters_;
    CaptureGate gate_;
    bool product_{},keyboardRegistered_{},recording_{},activeRecording_{},activeFreeze_{true},desktopActive_{true};
    ProductCaptureSink productSink_{};
    void* productContext_{};
    BindingConfig bindings_,activeBindings_;
    Button selected_{Button::Back};
    std::uint32_t selectedModifiers_{},passedButtons_{},retiredButtons_{},productFlags_{};
    std::uint64_t productSession_{},gateSession_{},sessionCounter_{},ordinarySequence_{};
    std::uint64_t generation_{1},activeGeneration_{1};
    bool ordinaryWheel_{};
    CaptureTarget ordinaryTarget_;
    std::uint64_t ordinaryDevice_{};
    DWORD wheelBarrier_{};
    std::atomic<std::uint64_t> currentSession_{},desktopPolicySession_{},appliedRevision_{};
    std::atomic<BindingConfig*> pendingBindings_{};
};
}
