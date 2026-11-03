#pragma once
#include <Windows.h>
#include "ProductApi.h"
#include "WheelApi.h"

namespace wmf::host {
enum class ProductTraceKind : std::uint32_t { Input, Frame, Action, Cleanup, Cancellation, Initialization, StopInertia };
struct ProductTrace {
    ProductTraceKind kind{};
    Action action{Action::None};
    Direction direction{Direction::None};
    InputEvent input;
    ContactFrame frame;
    Result result;
    std::uint64_t processStarted{},submitStarted{},returned{};
    std::uint32_t submitted{},boundary{};
    std::uint32_t stationary{}; // Contact maintenance, excluded from input latency samples.
    std::uint32_t wheelLedger{};
    double wheelAcceptedX{},wheelAcceptedY{},wheelDiscardedX{},wheelDiscardedY{},wheelEmittedX{},wheelEmittedY{};
};
struct ProductFunctions {
    decltype(&WmfBindingsCreate) createBindings{WmfBindingsCreate};
    decltype(&WmfBindingsFeed) feedBindings{WmfBindingsFeed};
    decltype(&WmfBindingsDeadline) deadline{WmfBindingsDeadline};
    decltype(&WmfBindingsDestroy) destroyBindings{WmfBindingsDestroy};
    decltype(&WmfPlatformCapabilities) capabilities{WmfPlatformCapabilities};
    decltype(&WmfPlatformCreate) createPlatform{WmfPlatformCreate};
    decltype(&WmfPlatformOpen) openPlatform{WmfPlatformOpen};
    decltype(&WmfPlatformMapContacts) map{WmfPlatformMapContacts};
    decltype(&WmfPlatformSubmitContacts) submit{WmfPlatformSubmitContacts};
    decltype(&WmfPlatformStopInertia) stopInertia{WmfPlatformStopInertia};
    decltype(&WmfPlatformDestroy) destroyPlatform{WmfPlatformDestroy};
    decltype(&WmfExecuteAction) execute{WmfExecuteAction};
    void* context{};
    std::uint64_t (*clock)(void*) noexcept{};
    bool (*targetValid)(void*,const InputEvent&,TargetPolicy,bool anchor) noexcept{};
    Result (*policy)(void*,std::uint64_t,TargetPolicy) noexcept{};
    void (*complete)(void*,std::uint64_t,bool canceled) noexcept{};
    void (*trace)(void*,const ProductTrace&) noexcept{};
};
bool ProductTargetValid(const InputEvent&,TargetPolicy,bool anchor) noexcept;
class ProductEngine {
public:
    explicit ProductEngine(BindingConfig,ProductFunctions = {}) noexcept;
    ~ProductEngine() { Close(); }
    Result Initialize() noexcept;
    Result Configure(const BindingConfig&) noexcept;
    Result Process(const InputEvent&) noexcept;
    Result Tick(std::uint64_t) noexcept;
    Result Cancel(CancelReason) noexcept;
    Result Close() noexcept;
    std::uint64_t Deadline() const noexcept;
    std::uint64_t Revision() const noexcept { return config_.revision; }
    bool Ready() const noexcept { return bindings_ && platform_; }
    bool Pressed() const noexcept { return pressed_; }
    bool ContactActive() const noexcept { return contact_; }
private:
    std::uint64_t Now() const noexcept;
    void Trace(ProductTrace) noexcept;
    bool Target(const InputEvent&,TargetPolicy,bool) const noexcept;
    void Complete(bool) noexcept;
    Result Decide(const InputEvent&,const BindingDecision&) noexcept;
    Result CancelActual() noexcept;
    Result Abort(Result,CancelReason) noexcept;
    Result Wheel(const InputEvent&) noexcept;
    Result WheelFrame(const WheelSample&,std::uint64_t,bool begin = false) noexcept;
    void RetireWheel(std::uint64_t) noexcept;
    BindingConfig config_;
    ProductFunctions api_;
    DeviceConfig device_;
    BindingHandle bindings_{};
    PlatformHandle platform_{};
    InputEvent current_;
    ContactFrame lastSubmitted_;
    Action action_{Action::None};
    TargetPolicy policy_{TargetPolicy::Application};
    bool pressed_{},contact_{},inertia_{};
    std::uint64_t processStarted_{};
    WheelHandle wheel_{};
    std::uint64_t wheelDeadline_{},wheelStarted_{};
    WheelSample wheelLedger_;
    double wheelEmittedX_{},wheelEmittedY_{};
    bool wheelHasLedger_{};
    InputEvent retiredWheel_;
    std::uint64_t wheelRetiredUntil_{};
};
}
