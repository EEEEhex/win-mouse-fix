#pragma once
#include "ContentDragApi.h"

namespace wmf {
inline constexpr std::uint32_t MaximumBindings = 64;
inline constexpr std::uint32_t MaximumShortcutKeys = 8;
enum class Button : std::uint32_t { None, Middle, Back, Forward };
enum Modifier : std::uint32_t { ShiftModifier = 1, ControlModifier = 2, AltModifier = 4, WindowsModifier = 8 };
inline constexpr std::uint32_t AllModifiers = 15;
enum class InputKind : std::uint32_t { Down, Move, Up, Cancel, Wheel, Tick };
enum class InputSource : std::uint32_t { Physical = 1, OwnInjection = 2, OtherInjection = 4, Control = 8 };
enum class Trigger : std::uint32_t { Click, DoubleClick, Hold, Drag, Wheel, WheelUp, WheelDown };
enum class Action : std::uint32_t {
    None, Back, Forward, MiddleClick, ContentDrag, ShellGestures, NativeZoom, Shortcut,
    VolumeUp, VolumeDown, Mute, PlayPause, PreviousTrack, NextTrack
};
enum class WheelMode : std::uint32_t { Off, Regular, High };
enum class TargetPolicy : std::uint32_t { Application, InputDesktop };
enum ProductInputFlag : std::uint32_t { RecordedInput = 1 };
struct alignas(8) InputEvent {
    std::uint32_t size{sizeof(InputEvent)}, version{AbiVersion};
    InputKind kind{InputKind::Move};
    Button button{Button::None};
    std::uint32_t modifiers{};
    InputSource source{InputSource::Physical};
    std::uint64_t session{},sequence{},firstQpc{},lastQpc{},device{},target{},revision{};
    std::uint64_t generation{1}; // Capture enable generation; a paused press never joins a resumed generation.
    std::int64_t dx{},dy{},wheelX{},wheelY{}; // Relative counts and signed raw wheel units (120 per nominal notch).
    std::int32_t anchorX{},anchorY{};
    std::uint32_t samples{1},nativeError{},cleanupError{};
    CancelReason reason{CancelReason::None};
    std::uint32_t flags{},reserved{};
};
struct Binding {
    std::uint32_t id{};
    Button button{Button::Back};
    Trigger trigger{Trigger::Click};
    std::uint32_t modifiers{};
    Action action{Action::Back};
    std::uint32_t keyCount{};
    std::uint32_t keys[MaximumShortcutKeys]{}; // Explicit virtual-key chord, no text or executable commands.
    std::uint32_t reserved[2]{};
};
struct alignas(8) BindingConfig {
    std::uint32_t size{sizeof(BindingConfig)},version{AbiVersion};
    std::uint64_t revision{1},qpcFrequency{10000000};
    double thresholdCounts{8},mmPerCountX{0.1},mmPerCountY{0.1};
    double shellMmPerCountX{0.1},shellMmPerCountY{0.1},directionRatio{1.35};
    double wheelMmPerNotch{15},regularMilliseconds{90},highMilliseconds{180};
    // Nominal expansion per notch at the initial radius; subsequent pinch
    // targets use the same relative factor, with reciprocal contraction.
    double zoomMmPerNotch{2},initialPinchMm{10};
    std::uint32_t doubleClickMs{500},holdMs{500},shellFingers{4};
    WheelMode wheelMode{WheelMode::Off};
    std::uint32_t bindingCount{},doubleClickTolerancePixels{4},reserved[2]{};
    Binding bindings[MaximumBindings]{};
};
// Independent touch locations relative to the synthetic device centre, in millimetres.
struct ContactPoint {
    std::uint32_t id{},reserved{};
    double xMm{},yMm{};
};
struct alignas(8) ContactFrame {
    std::uint32_t size{sizeof(ContactFrame)},version{AbiVersion};
    std::uint64_t sequence{};
    Phase phase{Phase::Begin};
    std::uint32_t count{2};
    double logicalSeconds{};
    ContactPoint contacts[5]{};
};
struct ValidationError {
    std::uint32_t size{sizeof(ValidationError)},version{AbiVersion};
    std::uint32_t bindingIndex{UINT32_MAX},conflictingIndex{UINT32_MAX};
    std::uint32_t field{},reserved{};
};
enum class BindingState : std::uint32_t { Idle, Pending, WaitingSecondClick, Held, ContentDrag, ShellGesture, Zoom, Ended, Canceled };
enum class Direction : std::uint32_t { None, Up, Down, Left, Right };
enum DecisionEffect : std::uint32_t { DecisionFinished = 1, DecisionCanceled = 2, DecisionExecute = 4, DecisionStopInertia = 8 };
struct alignas(8) BindingDecision {
    std::uint32_t size{sizeof(BindingDecision)},version{AbiVersion};
    BindingState state{BindingState::Idle};
    Action action{Action::None};
    std::uint32_t effects{},frameCount{},repeatCount{1},reserved{};
    Binding binding;
    TargetPolicy targetPolicy{TargetPolicy::Application};
    Direction direction{Direction::None};
    std::int64_t rawX{},rawY{};
    InputEvent actionInput;
    ContactFrame frames[3]{};
};
enum class ValidationField : std::uint32_t { None, Layout, Version, Range, Binding, Conflict, Shortcut, Input, Geometry };
inline BindingConfig DefaultBindingConfig() noexcept {
    BindingConfig c;
    c.bindingCount = 4;
    c.bindings[0] = {1,Button::Back,Trigger::Click,0,Action::Back};
    c.bindings[1] = {2,Button::Back,Trigger::Drag,0,Action::ContentDrag};
    c.bindings[2] = {3,Button::Forward,Trigger::Click,0,Action::Forward};
    c.bindings[3] = {4,Button::Forward,Trigger::Drag,0,Action::ShellGestures};
    return c;
}
static_assert(sizeof(InputEvent) == 152 && sizeof(Binding) == 64);
static_assert(sizeof(BindingConfig) == 4240);
static_assert(sizeof(ContactPoint) == 24 && sizeof(ContactFrame) == 152);
static_assert(sizeof(ValidationError) == 24);
static_assert(sizeof(BindingDecision) == 728);
static_assert(std::is_standard_layout_v<InputEvent> && std::is_trivially_copyable_v<InputEvent>);
static_assert(std::is_standard_layout_v<BindingConfig> && std::is_trivially_copyable_v<BindingConfig>);
static_assert(std::is_standard_layout_v<ContactFrame> && std::is_trivially_copyable_v<ContactFrame>);
static_assert(std::is_standard_layout_v<BindingDecision> && std::is_trivially_copyable_v<BindingDecision>);
struct BindingInstance;
using BindingHandle = BindingInstance*;
using ProductCaptureSink = bool (*)(void*,const InputEvent*) noexcept;
enum ProductCaptureFlag : std::uint32_t { WheelSourceValidated = 1, NativeZoomValidated = 2, ExplicitValidationProbe = 4 };
struct ProductCaptureConfig {
    std::uint32_t size{sizeof(ProductCaptureConfig)},version{AbiVersion};
    void* context{};
    ProductCaptureSink sink{};
    BindingConfig bindings;
    std::uint32_t confirmationTimeoutMs{100},flags{};
};
struct ActionRequest {
    std::uint32_t size{sizeof(ActionRequest)},version{AbiVersion};
    Binding binding;
    std::uint32_t repeatCount{1},reserved{};
};
static_assert(sizeof(ActionRequest) == 80);
struct CaptureRevision {
    std::uint32_t size{sizeof(CaptureRevision)},version{AbiVersion};
    std::uint64_t applied{},active{};
};
}
extern "C" {
WMF_CORE_API wmf::Result WmfValidateBindings(const wmf::BindingConfig*,wmf::ValidationError*) noexcept;
WMF_CORE_API wmf::Result WmfValidateInput(const wmf::InputEvent*) noexcept;
WMF_CORE_API wmf::Result WmfTranslateContacts(const wmf::GestureFrame*,wmf::ContactFrame*) noexcept;
WMF_CORE_API wmf::Result WmfBindingsCreate(const wmf::BindingConfig*,wmf::BindingHandle*) noexcept;
WMF_CORE_API wmf::Result WmfBindingsFeed(wmf::BindingHandle,const wmf::InputEvent*,wmf::BindingDecision*) noexcept;
WMF_CORE_API wmf::Result WmfBindingsDeadline(wmf::BindingHandle,std::uint64_t*) noexcept;
WMF_CORE_API wmf::Result WmfBindingsDestroy(wmf::BindingHandle*) noexcept;
WMF_PLATFORM_API wmf::Result WmfPlatformMapContacts(const wmf::DeviceConfig*,const wmf::ContactFrame*,wmf::MappedFrame*) noexcept;
WMF_PLATFORM_API wmf::Result WmfPlatformSubmitContacts(wmf::PlatformHandle,const wmf::ContactFrame*,wmf::MappedFrame*) noexcept;
WMF_PLATFORM_API wmf::Result WmfProductCaptureCreate(const wmf::ProductCaptureConfig*,wmf::CaptureHandle*) noexcept;
WMF_PLATFORM_API wmf::Result WmfProductCaptureUpdate(wmf::CaptureHandle,const wmf::BindingConfig*) noexcept;
WMF_PLATFORM_API wmf::Result WmfProductCapturePolicy(wmf::CaptureHandle,std::uint64_t,wmf::TargetPolicy) noexcept;
WMF_PLATFORM_API wmf::Result WmfProductCaptureRecording(wmf::CaptureHandle,std::uint32_t enabled) noexcept;
WMF_PLATFORM_API wmf::Result WmfProductCaptureRevision(wmf::CaptureHandle,wmf::CaptureRevision*) noexcept;
WMF_PLATFORM_API wmf::Result WmfExecuteAction(const wmf::ActionRequest*) noexcept;
}
