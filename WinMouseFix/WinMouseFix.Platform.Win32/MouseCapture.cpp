#include "MouseCapture.h"
#include <new>
#include <array>

namespace wmf::platform {
namespace {
constexpr wchar_t CaptureClass[] = L"WinMouseFix.RawMouseCapture.v2";
constexpr UINT CommandMessage = WM_APP+41;
constexpr UINT BindingMessage = WM_APP+42;
constexpr UINT RecordingMessage = WM_APP+43;
thread_local MouseCapture* currentCapture{};
std::atomic<DWORD> registeredOwner{};
std::uint64_t Frequency() noexcept { LARGE_INTEGER f{}; QueryPerformanceFrequency(&f); return static_cast<std::uint64_t>(f.QuadPart); }
HINSTANCE Module() noexcept {
    HMODULE m{};
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,CaptureClass,&m);
    return m;
}
DWORD Integrity(DWORD pid) noexcept {
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,FALSE,pid);
    if (!process) return MAXDWORD;
    HANDLE token{};
    if (!OpenProcessToken(process,TOKEN_QUERY,&token)) { CloseHandle(process); return MAXDWORD; }
    alignas(void*) std::array<std::byte,512> buffer{};
    DWORD bytes{}; DWORD level = MAXDWORD;
    if (GetTokenInformation(token,TokenIntegrityLevel,buffer.data(),static_cast<DWORD>(buffer.size()),&bytes)) {
        const auto sid = reinterpret_cast<TOKEN_MANDATORY_LABEL*>(buffer.data())->Label.Sid;
        const auto count = *GetSidSubAuthorityCount(sid);
        if (count) level = *GetSidSubAuthority(sid,count-1);
    }
    CloseHandle(token); CloseHandle(process); return level;
}
HWND Create(void*,void* instance) {
    WNDCLASSEXW cls{sizeof(cls)}; cls.hInstance = Module(); cls.lpszClassName = CaptureClass;
    cls.lpfnWndProc = MouseCapture::WindowProc;
    if (!RegisterClassExW(&cls) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return nullptr;
    const auto window = CreateWindowExW(0,CaptureClass,L"",0,0,0,0,0,HWND_MESSAGE,nullptr,cls.hInstance,instance);
    if (!window) { const auto error = GetLastError(); UnregisterClassW(CaptureClass,cls.hInstance); SetLastError(error); }
    return window;
}
BOOL Register(void*,HWND w,bool add) {
    RAWINPUTDEVICE device{0x01,0x02,static_cast<DWORD>(add ? RIDEV_INPUTSINK | RIDEV_DEVNOTIFY : RIDEV_REMOVE),add ? w : nullptr};
    return RegisterRawInputDevices(&device,1,sizeof(device));
}
BOOL RegisterKeyboard(void*,HWND w,bool add) {
    RAWINPUTDEVICE device{0x01,0x06,static_cast<DWORD>(add ? RIDEV_INPUTSINK | RIDEV_DEVNOTIFY : RIDEV_REMOVE),add ? w : nullptr};
    return RegisterRawInputDevices(&device,1,sizeof(device));
}
BOOL PostMessageAdapter(void*,HWND w,UINT m,WPARAM a,LPARAM b) { return PostMessageW(w,m,a,b); }
std::uint32_t ButtonBit(Button b) noexcept { return 1u << static_cast<unsigned>(b); }
Button HookButton(WPARAM kind,const MSLLHOOKSTRUCT& data) noexcept {
    if (kind == WM_MBUTTONDOWN || kind == WM_MBUTTONUP) return Button::Middle;
    if (kind == WM_XBUTTONDOWN || kind == WM_XBUTTONUP) {
        if (HIWORD(data.mouseData) == XBUTTON1) return Button::Back;
        if (HIWORD(data.mouseData) == XBUTTON2) return Button::Forward;
    }
    return Button::None;
}
USHORT DownFlag(Button b) noexcept { return b == Button::Back ? RI_MOUSE_BUTTON_4_DOWN : b == Button::Forward ? RI_MOUSE_BUTTON_5_DOWN : RI_MOUSE_MIDDLE_BUTTON_DOWN; }
USHORT UpFlag(Button b) noexcept { return b == Button::Back ? RI_MOUSE_BUTTON_4_UP : b == Button::Forward ? RI_MOUSE_BUTTON_5_UP : RI_MOUSE_MIDDLE_BUTTON_UP; }
bool IgnoreLegacy(void*,const MouseInput*) noexcept { return true; }
bool ActiveDesktop() noexcept {
    const auto input = OpenInputDesktop(0,FALSE,DESKTOP_READOBJECTS);
    if (!input) return false;
    wchar_t currentName[256]{},inputName[256]{}; DWORD bytes{};
    const bool active = GetUserObjectInformationW(GetThreadDesktop(GetCurrentThreadId()),UOI_NAME,currentName,sizeof(currentName),&bytes) &&
        GetUserObjectInformationW(input,UOI_NAME,inputName,sizeof(inputName),&bytes) && wcscmp(currentName,inputName) == 0;
    CloseDesktop(input); return active;
}
HHOOK Install(void*) { return SetWindowsHookExW(WH_MOUSE_LL,MouseCapture::HookProc,Module(),0); }
BOOL Remove(void*,HHOOK h) { return UnhookWindowsHookEx(h); }
BOOL Destroy(void*,HWND w) {
    if (IsWindow(w) && !DestroyWindow(w)) return FALSE;
    if (!UnregisterClassW(CaptureClass,Module()) && GetLastError() != ERROR_CLASS_DOES_NOT_EXIST) return FALSE;
    return TRUE;
}
BOOL Timer(void*,HWND w,bool add) { return add ? SetTimer(w,1,10,nullptr) != 0 : KillTimer(w,1); }
UINT Send(void*,UINT count,const INPUT* inputs) { return SendInput(count,const_cast<INPUT*>(inputs),sizeof(INPUT)); }
DWORD Error(void*) { return GetLastError(); }
}
CaptureNative RealCaptureNative() noexcept {
    CaptureNative api{nullptr,Create,Register,Install,Remove,Destroy,Timer,Send,Error};
    api.registerKeyboard = RegisterKeyboard; api.post = PostMessageAdapter; return api;
}
MouseCapture::MouseCapture(CaptureConfig config, CaptureNative api) noexcept
    : config_(config), api_(api), owner_(GetCurrentThreadId()), integrity_(Integrity(GetCurrentProcessId())), frequency_(Frequency()),
      gate_({this,
        [](void* p,const MouseInput& e) noexcept { return static_cast<MouseCapture*>(p)->Forward(e); },
        [](void* p,const CaptureTarget& t) noexcept { return static_cast<MouseCapture*>(p)->ValidTarget(t); },
        [](void* p) noexcept { return static_cast<MouseCapture*>(p)->Replay(); },
        [](void* p,Result r) noexcept { static_cast<MouseCapture*>(p)->Fault(r); }},counters_,frequency_*config.confirmationTimeoutMs/1000) {}
MouseCapture::MouseCapture(ProductCaptureConfig config,CaptureNative api) noexcept
    : MouseCapture(CaptureConfig{sizeof(CaptureConfig),AbiVersion,nullptr,IgnoreLegacy,config.confirmationTimeoutMs,0},api) {
    product_ = true; productSink_ = config.sink; productContext_ = config.context;
    bindings_ = activeBindings_ = config.bindings; productFlags_ = config.flags;
    appliedRevision_ = bindings_.revision;
}
bool MouseCapture::Forward(const MouseInput& input) noexcept {
    if (!product_) return config_.sink(config_.context,&input);
    if (input.kind == MouseInputKind::Down) {
        gateSession_ = input.session; productSession_ = ++sessionCounter_;
        currentSession_ = productSession_; desktopPolicySession_ = 0;
    }
    InputEvent e;
    e.kind = static_cast<InputKind>(input.kind); e.button = selected_; e.modifiers = selectedModifiers_;
    e.session = productSession_; e.sequence = input.sequence; e.firstQpc = input.firstQpc; e.lastQpc = input.lastQpc;
    e.device = input.device; e.target = input.target; e.revision = activeBindings_.revision;
    e.anchorX = input.anchorX; e.anchorY = input.anchorY; e.samples = input.samples;
    e.nativeError = input.nativeError; e.cleanupError = input.cleanupError; e.reason = input.reason;
    e.flags = activeRecording_ ? RecordedInput : 0;
    e.generation = activeGeneration_;
    if (e.kind == InputKind::Wheel) { e.wheelX = input.dx; e.wheelY = input.dy; }
    else { e.dx = input.dx; e.dy = input.dy; }
    return productSink_ && productSink_(productContext_,&e);
}
std::uint64_t MouseCapture::Now() const noexcept { LARGE_INTEGER t{}; QueryPerformanceCounter(&t); return static_cast<std::uint64_t>(t.QuadPart); }
Result MouseCapture::Open() noexcept {
    if (GetCurrentThreadId() != owner_) return {Status::WrongThread};
    if (window_ || currentCapture) return {Status::InvalidState};
    if (!config_.sink || (product_ && (!productSink_ || !api_.registerKeyboard)) || !frequency_ || !api_.createWindow || !api_.registerRaw || !api_.installHook ||
        !api_.removeHook || !api_.destroyWindow || !api_.timer || !api_.send || !api_.error) return {Status::InvalidArgument};
    DWORD expected{};
    if (!registeredOwner.compare_exchange_strong(expected,owner_)) return {Status::Busy};
    registrationLockOwned_ = true;
    window_ = api_.createWindow(api_.context,this);
    if (!window_) { registeredOwner = 0; registrationLockOwned_ = false; return {Status::NativeFailure,api_.error(api_.context)}; }
    currentCapture = this;
    Result r;
    if (!api_.registerRaw(api_.context,window_,true)) r = {Status::NativeFailure,api_.error(api_.context)};
    else {
        registered_ = true;
        hook_ = api_.installHook(api_.context);
        if (!hook_) r = {Status::NativeFailure,api_.error(api_.context)};
        else if (!api_.timer(api_.context,window_,true)) r = {Status::NativeFailure,api_.error(api_.context)};
        else timer_ = true;
    }
    if (!r) { const auto cleanup = Close(); if (!cleanup) r.cleanupError = cleanup.nativeError; return r; }
    if (product_ && !api_.registerKeyboard(api_.context,window_,true)) {
        r = {Status::NativeFailure,api_.error(api_.context)};
        const auto cleanup = Close(); if (!cleanup) r.cleanupError = cleanup.nativeError; return r;
    }
    keyboardRegistered_ = product_; RefreshTarget(); desktopActive_ = DesktopActive(); return {};
}
Result MouseCapture::Close() noexcept {
    if (GetCurrentThreadId() != owner_) return {Status::WrongThread};
    gate_.Enable(false,false,Now());
    // Keep a lightweight hook until the captured physical press is released.
    // Pointer motion is already restored; no orphaned XBUTTONUP is passed on exit.
    if (gate_.Owned()) return {Status::Busy};
    if (hook_) {
        if (!api_.removeHook(api_.context,hook_)) return {Status::NativeFailure,api_.error(api_.context)};
        hook_ = nullptr;
    }
    if (keyboardRegistered_) {
        if (!api_.registerKeyboard(api_.context,window_,false)) return {Status::NativeFailure,api_.error(api_.context)};
        keyboardRegistered_ = false;
    }
    if (registered_) {
        if (!api_.registerRaw(api_.context,window_,false)) return {Status::NativeFailure,api_.error(api_.context)};
        registered_ = false;
    }
    if (timer_) { api_.timer(api_.context,window_,false); timer_ = false; }
    if (window_) {
        if (!api_.destroyWindow(api_.context,window_)) return {Status::NativeFailure,api_.error(api_.context)};
        window_ = nullptr;
    }
    if (currentCapture == this) currentCapture = nullptr;
    if (registrationLockOwned_) { DWORD expected = owner_; registeredOwner.compare_exchange_strong(expected,0); registrationLockOwned_ = false; }
    return {};
}
Result MouseCapture::Post(CaptureCommand command,std::uint64_t session) noexcept {
    if (!window_ || command > CaptureCommand::Cancel) return {Status::InvalidState};
    const auto post = api_.post ? api_.post : PostMessageAdapter;
    return post(api_.context,window_,CommandMessage,static_cast<WPARAM>(command),static_cast<LPARAM>(session)) ?
        Result{} : Result{Status::NativeFailure,GetLastError()};
}
void MouseCapture::Command(CaptureCommand command,std::uint64_t session) noexcept {
    const auto now = Now();
    if (product_) {
        if (command == CaptureCommand::Enable) {
            generation_ = session ? session : 1;
            ApplyPendingBindings(); desktopActive_ = DesktopActive();
            if (!desktopActive_) return;
            if (!gate_.Owned() && !gate_.Enabled()) passedButtons_ |= HeldButtons();
            gate_.Enable(true,false,now); return;
        }
        if (command == CaptureCommand::Disable) CancelWheel(CancelReason::Pause,now);
        if (session && session != productSession_) return;
        if (command == CaptureCommand::CompleteClick || command == CaptureCommand::CompleteDrag) {
            gate_.Complete(gateSession_,false,now); if (!gate_.Owned()) currentSession_ = 0; return;
        }
        if (command == CaptureCommand::Cancel) { gate_.Cancel(CancelReason::NativeError,now); return; }
    }
    switch (command) {
    case CaptureCommand::Enable: gate_.Enable(true,(GetAsyncKeyState(VK_XBUTTON1)&0x8000) != 0,now); break;
    case CaptureCommand::Disable: gate_.Enable(false,false,now); break;
    case CaptureCommand::CompleteDrag: gate_.Complete(session,false,now); break;
    case CaptureCommand::CompleteClick: gate_.Complete(session,true,now); break;
    case CaptureCommand::Cancel: if (!session || session == gate_.Session()) gate_.Cancel(CancelReason::NativeError,now); break;
    }
}
Result MouseCapture::UpdateBindings(const BindingConfig& value) noexcept {
    if (!product_ || !window_) return {Status::InvalidState};
    if (value.size != sizeof(value) || value.version != AbiVersion || !value.revision || value.bindingCount > MaximumBindings) return {Status::InvalidArgument};
    auto* copy = new (std::nothrow) BindingConfig(value);
    if (!copy) return {Status::AllocationFailure};
    delete pendingBindings_.exchange(copy);
    const auto post = api_.post ? api_.post : PostMessageAdapter;
    return post(api_.context,window_,BindingMessage,0,0) ? Result{} : Result{Status::NativeFailure,api_.error(api_.context)};
}
void MouseCapture::ApplyPendingBindings() noexcept {
    if (GetCurrentThreadId() != owner_) return;
    if (auto* copy = pendingBindings_.exchange(nullptr)) {
        if (copy->wheelMode == WheelMode::Off && bindings_.wheelMode != WheelMode::Off) CancelWheel(CancelReason::Pause,Now());
        bindings_ = *copy; appliedRevision_ = bindings_.revision; delete copy;
        if (!gate_.Owned()) gate_.DropPrebuffer();
    }
}
Result MouseCapture::Policy(std::uint64_t session,TargetPolicy policy) noexcept {
    if (!product_ || !session || policy > TargetPolicy::InputDesktop) return {Status::InvalidArgument};
    if (currentSession_.load() != session) return {Status::InvalidState};
    desktopPolicySession_ = policy == TargetPolicy::InputDesktop ? session : 0; return {};
}
Result MouseCapture::Revisions(CaptureRevision& out) const noexcept {
    if (GetCurrentThreadId() != owner_) return {Status::WrongThread};
    if (out.size != sizeof(out)) return {Status::InvalidArgument};
    if (out.version != AbiVersion) return {Status::VersionMismatch};
    out.applied = appliedRevision_; out.active = gate_.Owned() ? activeBindings_.revision : 0; return {};
}
Result MouseCapture::Recording(bool enabled) noexcept {
    if (!product_ || !window_) return {Status::InvalidState};
    if (GetCurrentThreadId() == owner_) {
        gate_.Cancel(CancelReason::Pause,Now()); recording_ = enabled; gate_.DropPrebuffer(); return {};
    }
    const auto post = api_.post ? api_.post : PostMessageAdapter;
    return post(api_.context,window_,RecordingMessage,enabled,0) ? Result{} : Result{Status::NativeFailure,api_.error(api_.context)};
}
std::uint32_t MouseCapture::Modifiers() const noexcept {
    if (api_.modifiers) return api_.modifiers(api_.context);
    return ((GetAsyncKeyState(VK_SHIFT)&0x8000) ? ShiftModifier : 0) |
        ((GetAsyncKeyState(VK_CONTROL)&0x8000) ? ControlModifier : 0) |
        ((GetAsyncKeyState(VK_MENU)&0x8000) ? AltModifier : 0) |
        (((GetAsyncKeyState(VK_LWIN)|GetAsyncKeyState(VK_RWIN))&0x8000) ? WindowsModifier : 0);
}
std::uint32_t MouseCapture::HeldButtons() const noexcept {
    if (api_.heldButtons) return api_.heldButtons(api_.context);
    std::uint32_t held{};
    for (const auto b : {Button::Middle,Button::Back,Button::Forward}) {
        const auto key = b == Button::Middle ? VK_MBUTTON : b == Button::Back ? VK_XBUTTON1 : VK_XBUTTON2;
        if (GetAsyncKeyState(key)&0x8000) held |= ButtonBit(b);
    }
    if (GetAsyncKeyState(VK_LBUTTON)&0x8000) held |= 1u<<8;
    if (GetAsyncKeyState(VK_RBUTTON)&0x8000) held |= 1u<<9;
    return held;
}
bool MouseCapture::DesktopActive() const noexcept { return api_.desktopActive ? api_.desktopActive(api_.context) : ActiveDesktop(); }
CaptureTarget MouseCapture::CurrentTarget() const noexcept {
    if (api_.target) return api_.target(api_.context);
    const auto foreground = GetForegroundWindow(); POINT anchor{};
    if (!foreground || !GetCursorPos(&anchor) || GetAncestor(WindowFromPoint(anchor),GA_ROOT) != foreground) return {};
    DWORD pid{}; GetWindowThreadProcessId(foreground,&pid);
    if (!((foreground == cachedForeground_ && foregroundAllowed_) || (recording_ && pid == GetCurrentProcessId()))) return {};
    return {static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(foreground)),anchor.x,anchor.y};
}
bool MouseCapture::Bound(Button button,std::uint32_t modifiers) const noexcept {
    for (unsigned i = 0; i < bindings_.bindingCount; ++i)
        if (bindings_.bindings[i].button == button && bindings_.bindings[i].modifiers == modifiers) return true;
    return false;
}
bool MouseCapture::WheelEligible(std::int64_t delta) const noexcept {
    if (activeRecording_) return false;
    for (unsigned i = 0; i < activeBindings_.bindingCount; ++i) {
        const auto& b = activeBindings_.bindings[i];
        if (b.action == Action::NativeZoom && !(productFlags_ & ExplicitValidationProbe) && (productFlags_ & (WheelSourceValidated|NativeZoomValidated)) != (WheelSourceValidated|NativeZoomValidated)) continue;
        if (b.button == selected_ && b.modifiers == selectedModifiers_ && (b.trigger == Trigger::Wheel ||
            (b.trigger == Trigger::WheelUp && delta > 0) || (b.trigger == Trigger::WheelDown && delta < 0))) return true;
    }
    return false;
}
void MouseCapture::RefreshTarget() noexcept {
    const auto foreground = GetForegroundWindow();
    if (foreground != cachedForeground_) {
        cachedForeground_ = foreground; DWORD pid{};
        GetWindowThreadProcessId(foreground,&pid);
        const auto level = pid && pid != GetCurrentProcessId() ? Integrity(pid) : MAXDWORD;
        foregroundAllowed_ = level != MAXDWORD && integrity_ != MAXDWORD && level <= integrity_;
    }
}
bool MouseCapture::ValidTarget(const CaptureTarget& target) const noexcept {
    const auto policy = product_ && productSession_ && desktopPolicySession_.load() == productSession_ ? TargetPolicy::InputDesktop : TargetPolicy::Application;
    if (api_.validTarget) return api_.validTarget(api_.context,target,policy,activeFreeze_);
    const auto window = reinterpret_cast<HWND>(static_cast<std::uintptr_t>(target.window));
    if (!window || (policy == TargetPolicy::Application && (!IsWindow(window) || GetForegroundWindow() != window))) return false;
    if (product_ && !desktopActive_) return false;
    if (product_ && !activeFreeze_) return true;
    POINT p{};
    return GetCursorPos(&p) && p.x == target.x && p.y == target.y;
}
void MouseCapture::CancelWheel(CancelReason reason,std::uint64_t now) noexcept {
    if (!ordinaryWheel_) return;
    ordinaryWheel_ = false;
    InputEvent cancel; cancel.kind = InputKind::Cancel; cancel.reason = reason; cancel.generation = generation_;
    cancel.firstQpc = cancel.lastQpc = now;
    if (!productSink_(productContext_,&cancel)) Fault({Status::Busy});
}
bool MouseCapture::ProductHook(WPARAM kind,const MSLLHOOKSTRUCT& data) noexcept {
    const bool wheel = kind == WM_MOUSEWHEEL || kind == WM_MOUSEHWHEEL;
    if ((data.flags & LLMHF_INJECTED) || data.dwExtraInfo == ReplayMarker) {
        if (wheel) counters_.Add(CaptureMetric::InjectedWheelHooks);
        return false;
    }
    const auto now = Now();
    const bool takeover = kind == WM_LBUTTONDOWN || kind == WM_RBUTTONDOWN || kind == WM_MBUTTONDOWN || kind == WM_XBUTTONDOWN ||
        (wheel && Modifiers());
    if (takeover) { if (data.time) wheelBarrier_ = data.time; CancelWheel(CancelReason::OtherInput,now); }
    if (kind == WM_MOUSEMOVE && ordinaryWheel_ && (data.pt.x != ordinaryTarget_.x || data.pt.y != ordinaryTarget_.y)) CancelWheel(CancelReason::TargetChanged,now);
    if (gate_.Owned() && !ValidTarget(gate_.Target())) gate_.Cancel(CancelReason::TargetChanged,now);
    if (kind == WM_MOUSEMOVE) return gate_.Move();
    const auto button = HookButton(kind,data);
    const bool down = kind == WM_XBUTTONDOWN || kind == WM_MBUTTONDOWN;
    const bool up = kind == WM_XBUTTONUP || kind == WM_MBUTTONUP;
    if (button != Button::None && up) {
        if ((retiredButtons_ & ButtonBit(button)) && !gate_.Owned()) { retiredButtons_ &= ~ButtonBit(button); return true; }
        const bool passed = (passedButtons_ & ButtonBit(button)) != 0;
        passedButtons_ &= ~ButtonBit(button);
        if (button != selected_) return false;
        const auto blocked = gate_.Up(now);
        if (!gate_.Owned()) currentSession_ = 0;
        return !passed && blocked;
    }
    if (button != Button::None && down) {
        retiredButtons_ &= ~ButtonBit(button);
        if (passedButtons_ & ButtonBit(button)) return false;
        if (passedButtons_ & (ButtonBit(Button::Middle)|ButtonBit(Button::Back)|ButtonBit(Button::Forward))) {
            passedButtons_ |= ButtonBit(button); gate_.DropPrebuffer(); return false;
        }
        if (gate_.Owned() && button != selected_) {
            gate_.Cancel(CancelReason::OtherInput,now); passedButtons_ |= ButtonBit(button); return false;
        }
        if (gate_.Owned() && gate_.Held()) return true; // Duplicate hook boundary of the owned physical press.
        const auto modifiers = Modifiers();
        if ((!recording_ && !Bound(button,modifiers)) || !desktopActive_ || (HeldButtons() & ((1u<<8)|(1u<<9)))) {
            passedButtons_ |= ButtonBit(button); gate_.DropPrebuffer(); return false;
        }
        const auto target = CurrentTarget();
        if (!target.window) { passedButtons_ |= ButtonBit(button); gate_.DropPrebuffer(); return false; }
        // Never alter the snapshot or button of an old session still awaiting completion.
        if (gate_.Owned()) {
            const auto blocked = gate_.Down(target,now,button);
            if (!blocked) passedButtons_ |= ButtonBit(button);
            return blocked;
        }
        selected_ = button; selectedModifiers_ = modifiers; activeBindings_ = bindings_; activeRecording_ = recording_; activeGeneration_ = generation_;
        // Own the button and raw deltas, while Windows continues to move the
        // pointer normally (including acceleration, screen edges and monitors).
        activeFreeze_ = false;
        const auto blocked = gate_.Down(target,now,button,activeFreeze_);
        if (!blocked) passedButtons_ |= ButtonBit(button);
        return blocked;
    }
    if (wheel) {
        const auto delta = static_cast<SHORT>(HIWORD(data.mouseData));
        if (gate_.Owned()) {
            if (!gate_.Canceled() && kind == WM_MOUSEWHEEL && WheelEligible(delta)) {
                counters_.Add(CaptureMetric::BlockedWheels); return true;
            }
            gate_.Cancel(CancelReason::OtherInput,now); return false;
        }
        if (gate_.Enabled() && !recording_ && (productFlags_ & (WheelSourceValidated|ExplicitValidationProbe)) && bindings_.wheelMode != WheelMode::Off &&
            !Modifiers() && !HeldButtons() && CurrentTarget().window) { counters_.Add(CaptureMetric::BlockedWheels); return true; }
        CancelWheel(CancelReason::OtherInput,now);
        return false;
    }
    if (kind == WM_LBUTTONDOWN || kind == WM_RBUTTONDOWN) gate_.Cancel(CancelReason::OtherInput,now);
    return false;
}
void MouseCapture::ProductRaw(const RAWINPUT& raw) noexcept {
    if (!raw.header.hDevice) return;
    if (raw.header.dwType == RIM_TYPEKEYBOARD) {
        if (raw.data.keyboard.ExtraInformation == ReplayMarker) return;
        const auto messageTime = static_cast<DWORD>(GetMessageTime());
        if (messageTime) wheelBarrier_ = messageTime;
        CancelWheel(CancelReason::OtherInput,Now());
        const auto key = raw.data.keyboard.VKey;
        const bool modifier = key == VK_SHIFT || key == VK_LSHIFT || key == VK_RSHIFT || key == VK_CONTROL ||
            key == VK_LCONTROL || key == VK_RCONTROL || key == VK_MENU || key == VK_LMENU || key == VK_RMENU || key == VK_LWIN || key == VK_RWIN;
        if (modifier && Modifiers() == selectedModifiers_) return;
        // Observe takeover boundaries only. No scan code, character or text is retained.
        if (gate_.Owned()) { counters_.Add(CaptureMetric::KeyboardTakeovers); gate_.Cancel(CancelReason::OtherInput,Now()); }
        return;
    }
    if (raw.header.dwType != RIM_TYPEMOUSE || raw.data.mouse.ulExtraInformation == ReplayMarker) return;
    const auto& m = raw.data.mouse; const auto flags = m.usButtonFlags; const auto now = Now();
    auto wheelY = (flags & RI_MOUSE_WHEEL) ? static_cast<std::int64_t>(static_cast<SHORT>(m.usButtonData)) : 0;
    auto wheelX = (flags & RI_MOUSE_HWHEEL) ? static_cast<std::int64_t>(static_cast<SHORT>(m.usButtonData)) : 0;
    if (wheelX || wheelY) counters_.Add(CaptureMetric::RawWheels);
    // WM_INPUT posted before a physical takeover must not restart its old tail.
    // This is a boundary barrier, never a per-packet Hook/Raw matching heuristic.
    if (wheelBarrier_ && static_cast<LONG>(static_cast<DWORD>(GetMessageTime())-wheelBarrier_) <= 0) wheelX = wheelY = 0;
    if (!gate_.Owned() && (wheelX || wheelY)) {
        if (gate_.Enabled() && !recording_ && (productFlags_ & (WheelSourceValidated|ExplicitValidationProbe)) && bindings_.wheelMode != WheelMode::Off && !Modifiers() && !HeldButtons()) {
            const auto target = CurrentTarget();
            if (target.window && !(m.usFlags & MOUSE_MOVE_ABSOLUTE) && !(flags & ~(RI_MOUSE_WHEEL|RI_MOUSE_HWHEEL))) {
                InputEvent e; e.kind = InputKind::Wheel; e.session = ++sessionCounter_; e.sequence = 1;
                e.firstQpc = e.lastQpc = now; e.device = reinterpret_cast<std::uintptr_t>(raw.header.hDevice);
                e.target = target.window; e.anchorX = target.x; e.anchorY = target.y; e.revision = bindings_.revision;
                e.generation = generation_;
                e.wheelX = wheelX; e.wheelY = wheelY;
                ordinaryWheel_ = true; ordinaryTarget_ = target; ordinaryDevice_ = e.device;
                if (!productSink_(productContext_,&e)) Fault({Status::Busy});
            }
        }
    }
    RawMousePacket packet; packet.device = reinterpret_cast<std::uintptr_t>(raw.header.hDevice); packet.qpc = now;
    packet.dx = m.lLastX; packet.dy = m.lLastY; packet.button = selected_;
    if (!gate_.Owned()) {
        for (const auto button : {Button::Back,Button::Forward,Button::Middle})
            if (flags & (DownFlag(button)|UpFlag(button))) { packet.button = button; break; }
    }
    const auto buttonFlags = DownFlag(packet.button)|UpFlag(packet.button);
    packet.down = (flags & DownFlag(packet.button)) != 0; packet.up = (flags & UpFlag(packet.button)) != 0;
    packet.unsupported = (m.usFlags & MOUSE_MOVE_ABSOLUTE) != 0;
    packet.other = (flags & ~(buttonFlags|RI_MOUSE_WHEEL|RI_MOUSE_HWHEEL)) != 0;
    if (wheelX || wheelY) {
        if (gate_.Owned() && !wheelX && WheelEligible(wheelY)) packet.wheelY = wheelY;
        else packet.other = true;
    }
    gate_.Raw(packet);
}
bool MouseCapture::Hook(WPARAM kind,const MSLLHOOKSTRUCT& data) noexcept {
    if (product_) return ProductHook(kind,data);
    if (data.flags & LLMHF_INJECTED) return false;
    if (gate_.Frozen() && !ValidTarget(gate_.Target())) gate_.Cancel(CancelReason::TargetChanged,Now());
    if (kind == WM_MOUSEMOVE) return gate_.Move();
    const bool binding = (kind == WM_XBUTTONDOWN || kind == WM_XBUTTONUP) && HIWORD(data.mouseData) == XBUTTON1;
    if (binding && kind == WM_XBUTTONUP) {
        if ((retiredButtons_ & ButtonBit(Button::Back)) && !gate_.Owned()) { retiredButtons_ &= ~ButtonBit(Button::Back); return true; }
        return gate_.Up(Now());
    }
    if (binding) {
        retiredButtons_ &= ~ButtonBit(Button::Back);
        CaptureTarget target;
        const auto foreground = GetForegroundWindow();
        POINT anchor{}; const bool positionKnown = GetCursorPos(&anchor) != FALSE;
        const auto underPointer = positionKnown ? GetAncestor(WindowFromPoint(anchor),GA_ROOT) : nullptr;
        if (foreground && foreground == cachedForeground_ && foregroundAllowed_ && underPointer == foreground) {
            target = {static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(foreground)),anchor.x,anchor.y};
        }
        return gate_.Down(target,Now());
    }
    if (kind == WM_LBUTTONDOWN || kind == WM_RBUTTONDOWN || kind == WM_MBUTTONDOWN ||
        kind == WM_XBUTTONDOWN || kind == WM_MOUSEWHEEL || kind == WM_MOUSEHWHEEL)
        gate_.Cancel(CancelReason::OtherInput,Now());
    return false;
}
void MouseCapture::Raw(const RAWINPUT& raw) noexcept {
    if (product_) { ProductRaw(raw); return; }
    if (raw.header.dwType != RIM_TYPEMOUSE || !raw.header.hDevice || raw.data.mouse.ulExtraInformation == ReplayMarker) return;
    const auto& mouse = raw.data.mouse;
    constexpr USHORT binding = RI_MOUSE_BUTTON_4_DOWN | RI_MOUSE_BUTTON_4_UP;
    RawMousePacket packet;
    packet.device = reinterpret_cast<std::uintptr_t>(raw.header.hDevice); packet.qpc = Now();
    packet.dx = mouse.lLastX; packet.dy = mouse.lLastY;
    packet.down = (mouse.usButtonFlags & RI_MOUSE_BUTTON_4_DOWN) != 0;
    packet.up = (mouse.usButtonFlags & RI_MOUSE_BUTTON_4_UP) != 0;
    packet.other = (mouse.usButtonFlags & ~binding) != 0;
    packet.unsupported = (mouse.usFlags & MOUSE_MOVE_ABSOLUTE) != 0;
    gate_.Raw(packet);
}
void MouseCapture::Removed(HANDLE device) noexcept {
    if (product_ && ordinaryDevice_ == reinterpret_cast<std::uintptr_t>(device)) CancelWheel(CancelReason::DeviceRemoved,Now());
    if (gate_.Owned() && (!gate_.Device() || gate_.Device() == reinterpret_cast<std::uintptr_t>(device))) {
        retiredButtons_ |= ButtonBit(selected_); gate_.DeviceEnded(Now()); currentSession_ = 0;
    }
}
void MouseCapture::Tick() noexcept {
    RefreshTarget();
    if (product_) {
        ApplyPendingBindings(); const bool active = DesktopActive();
        if (ordinaryWheel_) {
            const auto target = CurrentTarget();
            if (!active || target.window != ordinaryTarget_.window || target.x != ordinaryTarget_.x || target.y != ordinaryTarget_.y)
                CancelWheel(CancelReason::TargetChanged,Now());
        }
        if (!active && desktopActive_) {
            if (gate_.Owned()) retiredButtons_ |= ButtonBit(selected_);
            gate_.DesktopEnded(Now()); currentSession_ = 0; passedButtons_ = 0;
        }
        desktopActive_ = active;
    }
    // A swallowed XBUTTONDOWN is not reflected reliably in Win32 async key
    // state. Only actual hook/raw button boundaries may end a captured press.
    gate_.Tick(Now(),!gate_.Owned() || ValidTarget(gate_.Target()));
    if (product_ && !gate_.Owned()) currentSession_ = 0;
}
Result MouseCapture::Replay() noexcept {
    return ReplayButtons(api_);
}
Result ReplayButtons(const CaptureNative& api) noexcept {
    INPUT inputs[2]{};
    for (auto& input : inputs) { input.type = INPUT_MOUSE; input.mi.mouseData = XBUTTON1; input.mi.dwExtraInfo = ReplayMarker; }
    inputs[0].mi.dwFlags = MOUSEEVENTF_XDOWN; inputs[1].mi.dwFlags = MOUSEEVENTF_XUP;
    SetLastError(ERROR_SUCCESS);
    const auto sent = api.send(api.context,2,inputs);
    if (sent == 2) return {};
    const auto error = api.error(api.context);
    Result result{Status::NativeFailure,error ? error : ERROR_ACCESS_DENIED};
    if (sent == 1 && api.send(api.context,1,&inputs[1]) != 1) result.cleanupError = api.error(api.context);
    return result;
}
void MouseCapture::Fault(Result result) noexcept {
    gate_.Enable(false,false,Now());
    MouseInput e; e.kind = MouseInputKind::Cancel; e.reason = CancelReason::NativeError;
    e.session = gate_.Session(); e.sequence = UINT64_MAX; e.firstQpc = e.lastQpc = Now();
    e.nativeError = result.nativeError; e.cleanupError = result.cleanupError; Forward(e);
}
LRESULT CALLBACK MouseCapture::HookProc(int code,WPARAM w,LPARAM l) noexcept {
    if (code < 0 || !currentCapture) return CallNextHookEx(nullptr,code,w,l);
    auto& c = *currentCapture;
    const auto started = c.Now();
    const bool blocked = c.Hook(w,*reinterpret_cast<const MSLLHOOKSTRUCT*>(l));
    const auto ticks = c.Now()-started;
    c.counters_.Add(CaptureMetric::HookCalls); c.counters_.Add(CaptureMetric::HookTicks,ticks);
    c.counters_.Maximum(CaptureMetric::HookMaxTicks,ticks);
    return blocked ? 1 : CallNextHookEx(nullptr,code,w,l);
}
LRESULT CALLBACK MouseCapture::WindowProc(HWND w,UINT m,WPARAM a,LPARAM b) noexcept {
    auto* c = reinterpret_cast<MouseCapture*>(GetWindowLongPtrW(w,GWLP_USERDATA));
    if (m == WM_NCCREATE) {
        c = static_cast<MouseCapture*>(reinterpret_cast<CREATESTRUCTW*>(b)->lpCreateParams);
        SetWindowLongPtrW(w,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(c));
    }
    if (c) {
        if (m == CommandMessage) { c->Command(static_cast<CaptureCommand>(a),static_cast<std::uint64_t>(b)); return 0; }
        if (m == BindingMessage) { c->ApplyPendingBindings(); return 0; }
        if (m == RecordingMessage) {
            c->Recording(a != 0); return 0;
        }
        if (m == WM_TIMER) { c->Tick(); return 0; }
        if (m == WM_INPUT_DEVICE_CHANGE && a == GIDC_REMOVAL) { c->Removed(reinterpret_cast<HANDLE>(b)); return 0; }
        if (m == WM_INPUT) {
            RAWINPUT raw{}; UINT bytes = sizeof(raw);
            const auto result = GetRawInputData(reinterpret_cast<HRAWINPUT>(b),RID_INPUT,&raw,&bytes,sizeof(RAWINPUTHEADER));
            if (result != static_cast<UINT>(-1) && result >= sizeof(RAWINPUTHEADER)) c->Raw(raw);
            else c->Fault({Status::NativeFailure,GetLastError()});
            // Foreground raw input needs DefWindowProc for OS cleanup.
        }
    }
    return DefWindowProcW(w,m,a,b);
}
}

namespace wmf { struct CaptureInstance {
    platform::MouseCapture capture;
    explicit CaptureInstance(CaptureConfig c) : capture(c) {}
    explicit CaptureInstance(ProductCaptureConfig c) : capture(c) {}
}; }
extern "C" wmf::Result WmfProductCaptureCreate(const wmf::ProductCaptureConfig* c,wmf::CaptureHandle* out) noexcept {
    using namespace wmf;
    if (!c || !out || c->size != sizeof(*c) || !c->sink || c->bindings.size != sizeof(BindingConfig) ||
        c->bindings.bindingCount > MaximumBindings || !c->bindings.revision || c->confirmationTimeoutMs < 10 ||
        c->confirmationTimeoutMs > 1000 || (c->flags & ~(WheelSourceValidated|NativeZoomValidated|ExplicitValidationProbe))) return {Status::InvalidArgument};
    if (c->version != AbiVersion || c->bindings.version != AbiVersion) return {Status::VersionMismatch};
    if (*out) return {Status::InvalidState};
    auto* instance = new (std::nothrow) CaptureInstance(*c);
    if (!instance) return {Status::AllocationFailure};
    const auto result = instance->capture.Open();
    if (!result && instance->capture.Closed()) { delete instance; return result; }
    *out = instance; return result;
}
extern "C" wmf::Result WmfProductCaptureUpdate(wmf::CaptureHandle h,const wmf::BindingConfig* c) noexcept {
    return h && c ? h->capture.UpdateBindings(*c) : wmf::Result{wmf::Status::InvalidArgument};
}
extern "C" wmf::Result WmfProductCapturePolicy(wmf::CaptureHandle h,std::uint64_t session,wmf::TargetPolicy policy) noexcept {
    return h ? h->capture.Policy(session,policy) : wmf::Result{wmf::Status::InvalidArgument};
}
extern "C" wmf::Result WmfProductCaptureRecording(wmf::CaptureHandle h,std::uint32_t enabled) noexcept {
    if (!h || enabled > 1) return {wmf::Status::InvalidArgument};
    return h->capture.Recording(enabled != 0);
}
extern "C" wmf::Result WmfProductCaptureRevision(wmf::CaptureHandle h,wmf::CaptureRevision* revision) noexcept {
    if (!h || !revision) return {wmf::Status::InvalidArgument};
    return h->capture.Revisions(*revision);
}
extern "C" wmf::Result WmfCaptureCreate(const wmf::CaptureConfig* c,wmf::CaptureHandle* out) noexcept {
    using namespace wmf;
    if (!c || !out || c->size != sizeof(*c) || !c->sink || c->confirmationTimeoutMs < 10 || c->confirmationTimeoutMs > 1000) return {Status::InvalidArgument};
    if (c->version != AbiVersion) return {Status::VersionMismatch};
    if (*out) return {Status::InvalidState};
    auto* instance = new (std::nothrow) CaptureInstance(*c);
    if (!instance) return {Status::AllocationFailure};
    const auto result = instance->capture.Open();
    if (!result && instance->capture.Closed()) { delete instance; return result; }
    *out = instance; return result;
}
extern "C" wmf::Result WmfCapturePost(wmf::CaptureHandle h,wmf::CaptureCommand c,std::uint64_t session) noexcept {
    return h ? h->capture.Post(c,session) : wmf::Result{wmf::Status::InvalidArgument};
}
extern "C" wmf::Result WmfCaptureReadStats(wmf::CaptureHandle h,wmf::CaptureStats* out) noexcept {
    if (!h || !out || out->size != sizeof(*out)) return {wmf::Status::InvalidArgument};
    if (out->version != wmf::AbiVersion) return {wmf::Status::VersionMismatch};
    *out = h->capture.Stats(); return {};
}
extern "C" wmf::Result WmfCaptureDestroy(wmf::CaptureHandle* h) noexcept {
    if (!h) return {wmf::Status::InvalidArgument};
    if (!*h) return {};
    const auto result = (*h)->capture.Close();
    if (result) { delete *h; *h = nullptr; }
    return result;
}
