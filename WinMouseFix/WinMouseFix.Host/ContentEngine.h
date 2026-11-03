#pragma once
#include <Windows.h>
#include "ContentDragApi.h"

namespace wmf::host {
enum class ContentTraceKind : unsigned { Input, Frame, Initialization, Cleanup, StopInertia, Failure, TargetGuard };
inline constexpr std::uint32_t ContentBoundary = 0x80000000u;
struct ContentTrace {
    ContentTraceKind kind{};
    std::uint32_t submitted{};
    MouseInput input;
    GestureFrame frame;
    Result result;
    std::uint64_t processStart{}, submitStart{}, returned{};
    std::uint64_t observedForeground{};
    std::int32_t observedX{},observedY{};
    std::uint32_t guardFlags{},reserved{};
};
struct ContentFunctions {
    decltype(&WmfDragCreate) createDrag{WmfDragCreate};
    decltype(&WmfDragFeed) feedDrag{WmfDragFeed};
    decltype(&WmfDragReset) resetDrag{WmfDragReset};
    decltype(&WmfDragDestroy) destroyDrag{WmfDragDestroy};
    decltype(&WmfPlatformCapabilities) capabilities{WmfPlatformCapabilities};
    decltype(&WmfPlatformCreate) createPlatform{WmfPlatformCreate};
    decltype(&WmfPlatformOpen) openPlatform{WmfPlatformOpen};
    decltype(&WmfPlatformSubmit) submit{WmfPlatformSubmit};
    decltype(&WmfPlatformStopInertia) stopInertia{WmfPlatformStopInertia};
    decltype(&WmfPlatformDestroy) destroyPlatform{WmfPlatformDestroy};
    void* context{};
    std::uint64_t (*clock)(void*) noexcept{};
    void (*trace)(void*,const ContentTrace&) noexcept{};
    void (*complete)(void*,std::uint64_t,bool click,bool canceled) noexcept{};
    bool (*targetValid)(void*,const MouseInput&) noexcept{};
};
class ContentEngine {
public:
    ContentEngine(DragConfig,DeviceConfig,bool captureOnly,ContentFunctions = {}) noexcept;
    ~ContentEngine() { Close(); }
    Result Initialize() noexcept;
    Result Process(const MouseInput&) noexcept;
    Result Cancel(CancelReason) noexcept;
    Result Close() noexcept;
    bool Ready() const noexcept { return drag_ && (captureOnly_ || platform_); }
private:
    std::uint64_t Now() const noexcept;
    void Record(ContentTrace) noexcept;
    Result Frames(const MouseInput&,const DragOutput&,bool validate = true) noexcept;
    Result Abort(Result,CancelReason) noexcept;
    void Complete(bool click,bool canceled) noexcept;
    DragConfig config_;
    DeviceConfig device_;
    bool captureOnly_{}, released_{};
    ContentFunctions api_;
    DragHandle drag_{};
    PlatformHandle platform_{};
    MouseInput current_;
    std::uint64_t processingStarted_{};
    GestureFrame lastSubmitted_;
    bool contactActive_{},guardRejected_{};
    DragState state_{DragState::Idle};
};
}
