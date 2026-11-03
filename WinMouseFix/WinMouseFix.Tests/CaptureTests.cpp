#include "../WinMouseFix.Platform.Win32/MouseCapture.h"
#include <iostream>
#include <vector>
#include <thread>

namespace {
using namespace wmf;
using namespace wmf::platform;
int checks{}, failures{};
#define VERIFY(...) do { ++checks; if (!(__VA_ARGS__)) { ++failures; std::cerr << __FILE__ << ':' << __LINE__ << ": " << #__VA_ARGS__ << '\n'; } } while(false)
struct GateFixture {
    CaptureCounters counters;
    std::vector<MouseInput> events;
    bool valid{true}, emitOk{true}, replayOk{true};
    unsigned replays{}, faults{};
    CaptureGate gate{{this,
        [](void* p,const MouseInput& e) noexcept { auto& f = *static_cast<GateFixture*>(p); f.events.push_back(e); return f.emitOk; },
        [](void* p,const CaptureTarget&) noexcept { return static_cast<GateFixture*>(p)->valid; },
        [](void* p) noexcept { auto& f = *static_cast<GateFixture*>(p); ++f.replays; return f.replayOk ? Result{} : Result{Status::NativeFailure,5}; },
        [](void* p,Result) noexcept { ++static_cast<GateFixture*>(p)->faults; }},counters,100};
    void Enable(bool held = false) { gate.Enable(true,held,1); }
    void Raw(bool down,bool up,std::uint64_t time,std::int64_t dx = 0,std::int64_t dy = 0,std::uint64_t device = 7) {
        gate.Raw({device,time,dx,dy,down,up,false,false});
    }
};
void OrderedAndReordered() {
    for (bool rawFirst : {false,true}) {
        GateFixture f; f.Enable();
        if (rawFirst) f.Raw(true,false,2,1,2);
        VERIFY(f.gate.Down({9,-1920,120},3));
        if (!rawFirst) f.Raw(true,false,4,1,2);
        VERIFY(f.events.size() == 2 && f.events[0].kind == MouseInputKind::Down && f.events[1].kind == MouseInputKind::Move);
        VERIFY(f.events[1].dx == 1 && f.events[1].dy == 2 && f.events[0].anchorX == -1920);
        VERIFY(f.gate.Move());
        f.Raw(false,true,6);
        VERIFY(f.events.back().kind == MouseInputKind::Up);
        f.gate.Complete(f.gate.Session(),true,7);
        VERIFY(f.replays == 0 && f.gate.Frozen());
        VERIFY(f.gate.Up(8));
        VERIFY(f.replays == 1 && !f.gate.Frozen() && !f.gate.Owned());
        f.gate.Complete(f.gate.Session(),true,9); VERIFY(f.replays == 1);
        VERIFY(!f.gate.Up(10));
    }
    GateFixture f; f.Enable();
    VERIFY(f.gate.Down({9,0,0},2)); f.Raw(true,false,3);
    VERIFY(f.gate.Up(4)); f.gate.Complete(f.gate.Session(),false,5);
    VERIFY(f.gate.Owned()); f.Raw(false,true,6);
    VERIFY(!f.gate.Owned() && f.replays == 0);
    GateFixture quick; quick.Enable(); VERIFY(quick.gate.Down({9,0,0},1)); quick.Raw(true,false,2);
    quick.Raw(false,true,3); VERIFY(quick.gate.Up(4));
    VERIFY(!quick.gate.Down({9,0,0},5)); // Passed through while the old engine is finishing.
    VERIFY(!quick.gate.Up(6)); // Its Up must also pass, even before old completion.
    quick.gate.Complete(quick.gate.Session(),false,7); VERIFY(!quick.gate.Owned());
}
void CancellationAndOwnership() {
    GateFixture f;
    VERIFY(!f.gate.Down({9,0,0},1)); VERIFY(!f.gate.Up(2));
    f.Enable(true); VERIFY(!f.gate.Up(3));
    VERIFY(f.gate.Down({9,0,0},4)); f.Raw(true,false,5);
    f.gate.Enable(false,false,6);
    VERIFY(!f.gate.Move() && f.gate.Owned());
    VERIFY(f.events.back().kind == MouseInputKind::Cancel);
    f.gate.Complete(f.gate.Session(),true,7); VERIFY(f.replays == 0);
    f.gate.Enable(true,true,8); VERIFY(f.gate.Owned());
    VERIFY(f.gate.Up(9)); VERIFY(!f.gate.Owned());
    f.Enable(); VERIFY(f.gate.Down({9,0,0},10)); f.Raw(true,false,11);
    const auto count = f.events.size(); f.Raw(false,false,12,100,200,8);
    VERIFY(f.events.size() == count);
    f.gate.Tick(13,false); VERIFY(!f.gate.Frozen()); VERIFY(f.gate.Up(14));
    f.Enable(); VERIFY(f.gate.Down({9,0,0},15));
    f.gate.Tick(116,true);
    VERIFY(!f.gate.Frozen() && f.events.back().reason == CancelReason::IncompleteInput);
    VERIFY(f.gate.Up(117));
    f.Enable(); VERIFY(f.gate.Down({9,0,0},120)); f.Raw(true,false,121);
    f.Raw(false,true,122); f.gate.Tick(123,true); VERIFY(f.gate.Owned());
    f.gate.Tick(223,true); VERIFY(!f.gate.Owned() && !f.gate.Frozen());
}
void HeldAcrossTimerTicks() {
    GateFixture f; f.Enable(); VERIFY(f.gate.Down({9,0,0},1)); f.Raw(true,false,2);
    // Reproduce the first real run: a timer tick 5-10ms after a swallowed
    // Down must not infer a release from the OS async button state.
    for (std::uint64_t tick : {5u,10u,50u,100u,1000u}) {
        f.gate.Tick(tick,true); VERIFY(f.gate.Owned() && f.gate.Frozen());
    }
    f.Raw(false,false,1001,10,4); VERIFY(f.events.back().kind == MouseInputKind::Move);
    f.Raw(false,true,1002); VERIFY(f.gate.Up(1003));
    f.gate.Complete(f.gate.Session(),false,1004);
    VERIFY(!f.gate.Owned() && !f.gate.Frozen() && f.replays == 0);

    GateFixture canceled; canceled.Enable(); VERIFY(canceled.gate.Down({9,0,0},1)); canceled.Raw(true,false,2);
    canceled.gate.Cancel(CancelReason::Pause,3);
    canceled.Raw(false,true,4); canceled.gate.Tick(105,true);
    VERIFY(!canceled.gate.Owned() && !canceled.gate.Frozen() && canceled.replays == 0);
}
void FaultsAndStaleCommands() {
    GateFixture f; f.Enable(); VERIFY(f.gate.Down({9,0,0},1)); f.Raw(true,false,2);
    f.gate.Complete(f.gate.Session()-1,true,3); VERIFY(f.gate.Frozen());
    f.Raw(false,true,4); VERIFY(f.gate.Up(5));
    f.valid = false; f.gate.Complete(f.gate.Session(),true,6);
    VERIFY(f.replays == 0 && !f.gate.Owned());
    GateFixture sendFailure; sendFailure.Enable(); VERIFY(sendFailure.gate.Down({9,0,0},1)); sendFailure.Raw(true,false,2);
    sendFailure.Raw(false,true,3); VERIFY(sendFailure.gate.Up(4)); sendFailure.replayOk = false;
    sendFailure.gate.Complete(sendFailure.gate.Session(),true,5);
    VERIFY(sendFailure.faults == 1 && !sendFailure.gate.Enabled() && !sendFailure.gate.Frozen());
    GateFixture overflow; overflow.Enable(); VERIFY(overflow.gate.Down({9,0,0},1));
    overflow.emitOk = false; overflow.Raw(true,false,2);
    VERIFY(overflow.faults == 1 && !overflow.gate.Frozen() && !overflow.gate.Enabled());
    VERIFY(overflow.gate.Up(3));
    GateFixture unsupported; unsupported.Enable(); VERIFY(unsupported.gate.Down({9,0,0},1));
    unsupported.gate.Raw({7,2,0,0,true,true,false,false});
    VERIFY(!unsupported.gate.Frozen()); VERIFY(unsupported.gate.Up(3));
}
struct NativeFixture {
    int failAt{}, stage{}, created{}, registered{}, installed{}, removed{}, destroyed{}, timers{}, sendCalls{};
    UINT sent{2}; bool failCleanup{};
    CaptureTarget target{9,-1920,120};
    std::uint32_t modifiers{},held{};
    bool desktop{true},valid{true},allowDesktop{true}; int keyboards{};
    CaptureNative Api() {
        return {this,
            [](void* p,void*) -> HWND { auto& f = *static_cast<NativeFixture*>(p); ++f.created; return ++f.stage == f.failAt ? nullptr : reinterpret_cast<HWND>(1); },
            [](void* p,HWND,bool add) -> BOOL { auto& f = *static_cast<NativeFixture*>(p); if (!add) { --f.registered; return TRUE; } if (++f.stage == f.failAt) return FALSE; ++f.registered; return TRUE; },
            [](void* p) -> HHOOK { auto& f = *static_cast<NativeFixture*>(p); if (++f.stage == f.failAt) return nullptr; ++f.installed; return reinterpret_cast<HHOOK>(1); },
            [](void* p,HHOOK) -> BOOL { auto& f = *static_cast<NativeFixture*>(p); if (f.failCleanup) return FALSE; ++f.removed; return TRUE; },
            [](void* p,HWND) -> BOOL { ++static_cast<NativeFixture*>(p)->destroyed; return TRUE; },
            [](void* p,HWND,bool add) -> BOOL { auto& f = *static_cast<NativeFixture*>(p); if (add && ++f.stage == f.failAt) return FALSE; f.timers += add ? 1 : -1; return TRUE; },
            [](void* p,UINT count,const INPUT* input) -> UINT {
                auto& f = *static_cast<NativeFixture*>(p); ++f.sendCalls;
                if (input[0].mi.dwExtraInfo != ReplayMarker || input[0].mi.mouseData != XBUTTON1) return 0;
                return count == 1 ? (f.failCleanup ? 0u : 1u) : f.sent;
            },
            [](void*) -> DWORD { return 5; }};
    }
    CaptureNative ProductApi() {
        auto api = Api();
        api.registerKeyboard = [](void* p,HWND,bool add) -> BOOL {
            auto& f = *static_cast<NativeFixture*>(p); if (add && ++f.stage == f.failAt) return FALSE;
            f.keyboards += add ? 1 : -1; return TRUE;
        };
        api.target = [](void* p) { return static_cast<NativeFixture*>(p)->target; };
        api.validTarget = [](void* p,const CaptureTarget&,TargetPolicy policy,bool) {
            auto& f = *static_cast<NativeFixture*>(p); return f.desktop && (f.valid || (policy == TargetPolicy::InputDesktop && f.allowDesktop));
        };
        api.modifiers = [](void* p) { return static_cast<NativeFixture*>(p)->modifiers; };
        api.heldButtons = [](void* p) { return static_cast<NativeFixture*>(p)->held; };
        api.desktopActive = [](void* p) { return static_cast<NativeFixture*>(p)->desktop; };
        api.post = [](void*,HWND,UINT,WPARAM,LPARAM) -> BOOL { return TRUE; };
        return api;
    }
};
struct ProductFixture {
    NativeFixture native;
    std::vector<InputEvent> events;
    ProductCaptureConfig config;
    ProductFixture() {
        config.context = this; config.bindings = DefaultBindingConfig();
        config.sink = [](void* p,const InputEvent* e) noexcept {
            static_cast<ProductFixture*>(p)->events.push_back(*e); return true;
        };
    }
    MSLLHOOKSTRUCT HookData(Button b = Button::Back) {
        MSLLHOOKSTRUCT h{}; h.mouseData = (b == Button::Forward ? XBUTTON2 : XBUTTON1)<<16; return h;
    }
    void Raw(MouseCapture& capture,USHORT flags = 0,LONG dx = 0,LONG dy = 0,SHORT wheel = 0,std::uint64_t device = 7) {
        RAWINPUT r{}; r.header.dwType = RIM_TYPEMOUSE; r.header.hDevice = reinterpret_cast<HANDLE>(device);
        r.data.mouse.usButtonFlags = flags; r.data.mouse.lLastX = dx; r.data.mouse.lLastY = dy;
        r.data.mouse.usButtonData = static_cast<USHORT>(wheel); capture.Raw(r);
    }
    void Complete(MouseCapture& c) { c.Command(CaptureCommand::CompleteDrag,events.back().session); }
};
void ProductCaptureBoundaries() {
    for (bool rawFirst : {false,true}) for (const auto button : {Button::Back,Button::Forward}) {
        ProductFixture f; MouseCapture c(f.config,f.native.ProductApi()); VERIFY(c.Open()); c.Command(CaptureCommand::Enable,0);
        const auto data = f.HookData(button); const USHORT down = button == Button::Back ? RI_MOUSE_BUTTON_4_DOWN : RI_MOUSE_BUTTON_5_DOWN;
        const USHORT up = button == Button::Back ? RI_MOUSE_BUTTON_4_UP : RI_MOUSE_BUTTON_5_UP;
        if (rawFirst) f.Raw(c,down,1,2);
        VERIFY(c.Hook(WM_XBUTTONDOWN,data));
        if (!rawFirst) f.Raw(c,down,1,2);
        VERIFY(f.events.size() == 2 && f.events.front().button == button && f.events.front().kind == InputKind::Down);
        VERIFY(f.events.back().dx == 1 && f.events.back().dy == 2 && f.events.back().revision == 1);
        auto changed = f.config.bindings; changed.revision = 2; changed.bindingCount = 0;
        VERIFY(c.UpdateBindings(changed)); c.ApplyPendingBindings(); VERIFY(c.AppliedRevision() == 2);
        f.Raw(c,0,4,5); VERIFY(f.events.back().revision == 1 && c.Hook(WM_MOUSEMOVE,data));
        f.Raw(c,up); VERIFY(c.Hook(WM_XBUTTONUP,data)); f.Complete(c);
        VERIFY(!c.Hook(WM_XBUTTONDOWN,data)); VERIFY(!c.Hook(WM_XBUTTONUP,data));
        VERIFY(c.Close()); VERIFY(f.native.sendCalls == 0 && f.native.keyboards == 0);
    }
    ProductFixture f; f.native.held = 1u<<static_cast<unsigned>(Button::Back);
    MouseCapture c(f.config,f.native.ProductApi()); VERIFY(c.Open()); c.Command(CaptureCommand::Enable,0);
    auto back = f.HookData(),forward = f.HookData(Button::Forward);
    VERIFY(!c.Hook(WM_XBUTTONUP,back)); f.native.held = 0;
    VERIFY(!c.Hook(WM_MBUTTONDOWN,back)); VERIFY(!c.Hook(WM_MBUTTONUP,back));
    f.native.modifiers = ControlModifier;
    VERIFY(!c.Hook(WM_XBUTTONDOWN,back)); VERIFY(!c.Hook(WM_XBUTTONUP,back)); f.native.modifiers = 0;
    VERIFY(c.Hook(WM_XBUTTONDOWN,back)); f.Raw(c,RI_MOUSE_BUTTON_4_DOWN);
    const auto session = f.events.back().session;
    VERIFY(!c.Hook(WM_XBUTTONDOWN,forward)); VERIFY(f.events.back().kind == InputKind::Cancel);
    VERIFY(!c.Hook(WM_XBUTTONUP,forward)); VERIFY(c.Hook(WM_XBUTTONUP,back));
    c.Command(CaptureCommand::CompleteDrag,session);
    VERIFY(c.Hook(WM_XBUTTONDOWN,forward)); f.Raw(c,RI_MOUSE_BUTTON_5_DOWN);
    VERIFY(c.Policy(session,TargetPolicy::InputDesktop).code == Status::InvalidState);
    VERIFY(c.Policy(f.events.back().session,TargetPolicy::InputDesktop));
    f.native.valid = false; c.Tick(); VERIFY(f.events.back().kind == InputKind::Down);
    RAWINPUT keyboard{}; keyboard.header.dwType = RIM_TYPEKEYBOARD; keyboard.header.hDevice = reinterpret_cast<HANDLE>(4);
    c.Raw(keyboard); VERIFY(f.events.back().kind == InputKind::Cancel && c.Stats().keyboardTakeovers == 1);
    VERIFY(c.Hook(WM_XBUTTONUP,forward)); f.native.valid = true;
    VERIFY(c.Hook(WM_XBUTTONDOWN,back)); f.Raw(c,RI_MOUSE_BUTTON_4_DOWN);
    f.native.desktop = false; c.Tick(); VERIFY(f.events.back().kind == InputKind::Cancel);
    VERIFY(c.Close()); VERIFY(f.native.keyboards == 0 && f.native.sendCalls == 0);
}
void ProductWheelsAndRecording() {
    {
        // A zoom-only binding still owns an anchor, even without a drag binding.
        // Incidental movement must not cancel its first native pinch frame.
        ProductFixture zoomOnly; zoomOnly.config.flags = ExplicitValidationProbe;
        zoomOnly.config.bindings.bindingCount = 2;
        zoomOnly.config.bindings.bindings[1] = {2,Button::Back,Trigger::Wheel,0,Action::NativeZoom};
        MouseCapture capture(zoomOnly.config,zoomOnly.native.ProductApi()); VERIFY(capture.Open()); capture.Command(CaptureCommand::Enable,0);
        const auto back = zoomOnly.HookData(); VERIFY(capture.Hook(WM_XBUTTONDOWN,back)); zoomOnly.Raw(capture,RI_MOUSE_BUTTON_4_DOWN);
        VERIFY(capture.Hook(WM_MOUSEMOVE,back)); zoomOnly.Raw(capture,0,0,-1);
        auto wheel = back; wheel.mouseData = static_cast<ULONG>(static_cast<USHORT>(120))<<16;
        VERIFY(capture.Hook(WM_MOUSEWHEEL,wheel)); zoomOnly.Raw(capture,RI_MOUSE_WHEEL,0,0,120);
        wheel.mouseData = static_cast<ULONG>(static_cast<USHORT>(-120))<<16;
        VERIFY(capture.Hook(WM_MOUSEWHEEL,wheel)); zoomOnly.Raw(capture,RI_MOUSE_WHEEL,0,0,-120);
        VERIFY(zoomOnly.events.back().kind == InputKind::Wheel && zoomOnly.events.back().wheelY == -120);
        for (int i = 0; i < 100; ++i) {
            VERIFY(capture.Hook(WM_MOUSEWHEEL,wheel)); zoomOnly.Raw(capture,RI_MOUSE_WHEEL,0,0,-120);
        }
        wheel.mouseData = static_cast<ULONG>(static_cast<USHORT>(120))<<16;
        VERIFY(capture.Hook(WM_MOUSEWHEEL,wheel)); zoomOnly.Raw(capture,RI_MOUSE_WHEEL,0,0,120);
        VERIFY(zoomOnly.events.back().kind == InputKind::Wheel && zoomOnly.events.back().wheelY == 120);
        zoomOnly.Raw(capture,RI_MOUSE_BUTTON_4_UP); VERIFY(capture.Hook(WM_XBUTTONUP,back)); zoomOnly.Complete(capture);
        VERIFY(!capture.Hook(WM_MOUSEMOVE,back)); VERIFY(capture.Close());
    }
    ProductFixture f; auto& b = f.config.bindings; b.bindings[b.bindingCount++] = {5,Button::Back,Trigger::Wheel,0,Action::NativeZoom};
    b.wheelMode = WheelMode::High; f.config.flags = NativeZoomValidated|WheelSourceValidated;
    MouseCapture c(f.config,f.native.ProductApi()); VERIFY(c.Open()); c.Command(CaptureCommand::Enable,0);
    auto data = f.HookData(); data.mouseData = static_cast<ULONG>(static_cast<USHORT>(30))<<16;
    f.native.modifiers = ControlModifier;
    VERIFY(!c.Hook(WM_MOUSEWHEEL,data)); // An unbound modifier keeps the application's original wheel semantics.
    f.Raw(c,RI_MOUSE_WHEEL,0,0,30); VERIFY(f.events.empty()); f.native.modifiers = 0;
    auto back = f.HookData(); VERIFY(c.Hook(WM_XBUTTONDOWN,back)); f.Raw(c,RI_MOUSE_BUTTON_4_DOWN);
    VERIFY(c.Hook(WM_MOUSEWHEEL,data)); f.Raw(c,RI_MOUSE_WHEEL,0,0,30);
    VERIFY(f.events.back().kind == InputKind::Wheel && f.events.back().wheelY == 30 && !f.events.back().dx);
    data.flags = LLMHF_INJECTED; VERIFY(!c.Hook(WM_MOUSEWHEEL,data));
    const auto count = f.events.size(); f.Raw(c,RI_MOUSE_WHEEL,0,0,120,0); VERIFY(f.events.size() == count);
    f.Raw(c,RI_MOUSE_BUTTON_4_UP); VERIFY(c.Hook(WM_XBUTTONUP,back)); f.Complete(c);
    VERIFY(c.Recording(true)); VERIFY(c.Hook(WM_MBUTTONDOWN,back)); f.Raw(c,RI_MOUSE_MIDDLE_BUTTON_DOWN);
    VERIFY(f.events.back().button == Button::Middle && (f.events.back().flags & RecordedInput));
    VERIFY(!c.Hook(WM_MOUSEMOVE,back)); f.Raw(c,RI_MOUSE_MIDDLE_BUTTON_UP);
    VERIFY(c.Hook(WM_MBUTTONUP,back)); f.Complete(c); VERIFY(c.Recording(false));
    VERIFY(c.Stats().blockedWheels == 1 && c.Stats().injectedWheelHooks == 1 && c.Stats().rawWheels == 2);
    VERIFY(c.Close());
    ProductFixture allowed; allowed.config.flags = WheelSourceValidated; allowed.config.bindings.wheelMode = WheelMode::Regular;
    MouseCapture a(allowed.config,allowed.native.ProductApi()); VERIFY(a.Open()); a.Command(CaptureCommand::Enable,0);
    data.flags = 0; VERIFY(a.Hook(WM_MOUSEWHEEL,data)); allowed.Raw(a,RI_MOUSE_WHEEL,0,0,-15);
    VERIFY(allowed.events.size() == 1 && allowed.events.back().button == Button::None && allowed.events.back().wheelY == -15);
    allowed.native.modifiers = ShiftModifier; VERIFY(!a.Hook(WM_MOUSEWHEEL,data)); allowed.Raw(a,RI_MOUSE_WHEEL,0,0,15);
    VERIFY(allowed.events.size() == 2 && allowed.events.back().kind == InputKind::Cancel && allowed.events.back().reason == CancelReason::OtherInput);
    allowed.native.modifiers = 0; allowed.Raw(a,RI_MOUSE_WHEEL,0,0,15);
    VERIFY(allowed.events.back().kind == InputKind::Wheel);
    VERIFY(!a.Hook(WM_LBUTTONDOWN,data)); VERIFY(allowed.events.back().kind == InputKind::Cancel);
    VERIFY(!a.Hook(WM_LBUTTONUP,data));
    allowed.Raw(a,RI_MOUSE_WHEEL,0,0,15); auto off = allowed.config.bindings; off.revision = 2; off.wheelMode = WheelMode::Off;
    VERIFY(a.UpdateBindings(off)); a.ApplyPendingBindings();
    VERIFY(allowed.events.back().kind == InputKind::Cancel && allowed.events.back().reason == CancelReason::Pause);
    const auto stoppedCount = allowed.events.size(); VERIFY(!a.Hook(WM_MOUSEWHEEL,data)); allowed.Raw(a,RI_MOUSE_WHEEL,0,0,15); VERIFY(allowed.events.size() == stoppedCount);
    VERIFY(a.Close());
    for (int fail = 1; fail <= 5; ++fail) {
        ProductFixture broken; broken.native.failAt = fail; MouseCapture failed(broken.config,broken.native.ProductApi());
        VERIFY(!failed.Open() && failed.Closed()); VERIFY(broken.native.registered == 0 && broken.native.keyboards == 0);
    }
    ProductFixture unverified; unverified.config.bindings = b; unverified.config.flags = 0;
    MouseCapture u(unverified.config,unverified.native.ProductApi()); VERIFY(u.Open()); u.Command(CaptureCommand::Enable,0);
    VERIFY(!u.Hook(WM_MOUSEWHEEL,data)); unverified.Raw(u,RI_MOUSE_WHEEL,0,0,30); VERIFY(unverified.events.empty());
    VERIFY(u.Hook(WM_XBUTTONDOWN,back)); unverified.Raw(u,RI_MOUSE_BUTTON_4_DOWN);
    VERIFY(!u.Hook(WM_MOUSEWHEEL,data) && unverified.events.back().kind == InputKind::Cancel);
    VERIFY(u.Hook(WM_XBUTTONUP,back)); VERIFY(u.Close());
}
void ProductGenerationsAndRemoval() {
    ProductFixture f; MouseCapture c(f.config,f.native.ProductApi()); VERIFY(c.Open()); c.Command(CaptureCommand::Enable,17);
    auto back = f.HookData(); VERIFY(!c.Hook(WM_LBUTTONDOWN,back)); VERIFY(!c.Hook(WM_LBUTTONUP,back));
    VERIFY(c.Hook(WM_XBUTTONDOWN,back)); f.Raw(c,RI_MOUSE_BUTTON_4_DOWN); const auto first = f.events.back().session;
    VERIFY(f.events.back().generation == 17); c.Removed(reinterpret_cast<HANDLE>(8)); VERIFY(c.Hook(WM_MOUSEMOVE,back));
    c.Removed(reinterpret_cast<HANDLE>(7)); VERIFY(f.events.back().reason == CancelReason::DeviceRemoved && !c.Hook(WM_MOUSEMOVE,back));
    CaptureRevision revisions; VERIFY(c.Revisions(revisions) && !revisions.active);
    VERIFY(c.Hook(WM_XBUTTONUP,back)); // A late orphan Up stays swallowed without retaining an endless owner.
    VERIFY(c.Hook(WM_XBUTTONDOWN,back)); f.Raw(c,RI_MOUSE_BUTTON_4_DOWN);
    c.Command(CaptureCommand::Disable,0); c.Command(CaptureCommand::Enable,18);
    const auto events = f.events.size(); f.Raw(c,0,100,200); VERIFY(f.events.size() == events);
    VERIFY(c.Hook(WM_XBUTTONUP,back)); VERIFY(c.Hook(WM_XBUTTONDOWN,back)); f.Raw(c,RI_MOUSE_BUTTON_4_DOWN);
    VERIFY(f.events.back().generation == 18 && f.events.back().session > first);
    c.Command(CaptureCommand::CompleteDrag,first); VERIFY(c.Hook(WM_MOUSEMOVE,back));
    f.Raw(c,RI_MOUSE_BUTTON_4_UP); VERIFY(c.Hook(WM_XBUTTONUP,back)); f.Complete(c); VERIFY(c.Close());
}
void ResourcesAndReplay() {
    CaptureConfig config; config.sink = [](void*,const MouseInput*) noexcept { return true; };
    for (int fail = 1; fail <= 4; ++fail) {
        NativeFixture f; f.failAt = fail;
        MouseCapture c(config,f.Api());
        VERIFY(!c.Open()); VERIFY(c.Closed()); VERIFY(f.registered == 0 && f.installed == f.removed && f.timers == 0);
        VERIFY(c.Close());
    }
    NativeFixture f; MouseCapture c(config,f.Api()); VERIFY(c.Open());
    VERIFY(c.Open().code == Status::InvalidState);
    NativeFixture otherFixture; MouseCapture other(config,otherFixture.Api());
    VERIFY(!other.Open());
    VERIFY(other.Close()); // Must not release the first instance's registration lock.
    Result wrongThread; std::thread thread([&] { wrongThread = c.Close(); }); thread.join();
    VERIFY(wrongThread.code == Status::WrongThread);
    MSLLHOOKSTRUCT injected{}; injected.flags = LLMHF_INJECTED; injected.mouseData = XBUTTON1<<16;
    VERIFY(!c.Hook(WM_XBUTTONDOWN,injected));
    f.failCleanup = true; VERIFY(!c.Close() && !c.Closed());
    f.failCleanup = false; VERIFY(c.Close() && c.Closed()); VERIFY(c.Close());
    for (UINT count : {0u,1u,2u}) {
        NativeFixture replay; replay.sent = count;
        const auto result = ReplayButtons(replay.Api());
        VERIFY(static_cast<bool>(result) == (count == 2));
        VERIFY(replay.sendCalls == (count == 1 ? 2 : 1));
    }
    CaptureHandle h{}; CaptureConfig invalid;
    NativeFixture partial; partial.sent = 1; partial.failCleanup = true;
    const auto partialResult = ReplayButtons(partial.Api());
    VERIFY(partialResult.nativeError == 5 && partialResult.cleanupError == 5);
    VERIFY(!WmfCaptureCreate(&invalid,&h) && !h);
    VERIFY(WmfCaptureDestroy(&h));
}
}
int RunCaptureTests(int& total) {
    OrderedAndReordered(); CancellationAndOwnership(); HeldAcrossTimerTicks(); FaultsAndStaleCommands(); ResourcesAndReplay();
    ProductCaptureBoundaries(); ProductWheelsAndRecording(); ProductGenerationsAndRemoval();
    total += checks; return failures;
}
