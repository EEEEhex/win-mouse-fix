#include "ProductApi.h"
#include "../WinMouseFix.Core/PinchTrajectory.h"
#include "../WinMouseFix.Platform.Win32/DeviceSession.h"
#include <cmath>
#include <iostream>
#include <limits>
#include <thread>
#include <vector>

namespace {
using namespace wmf;
using namespace wmf::platform;
int checks{},failures{};
#define VERIFY(...) do { ++checks; if (!(__VA_ARGS__)) { ++failures; std::cerr << __FILE__ << ':' << __LINE__ << ": " << #__VA_ARGS__ << '\n'; } } while (false)
void ContractValidation() {
    auto c = DefaultBindingConfig(); ValidationError e;
    VERIFY(WmfValidateBindings(&c,&e)); VERIFY(e.field == 0 && e.bindingIndex == UINT32_MAX);
    VERIFY(c.bindingCount == 4 && c.bindings[1].action == Action::ContentDrag && c.bindings[3].action == Action::ShellGestures);
    c.bindings[2].id = c.bindings[0].id;
    VERIFY(!WmfValidateBindings(&c,&e) && e.field == static_cast<unsigned>(ValidationField::Conflict) && e.bindingIndex == 2 && e.conflictingIndex == 0);
    c = DefaultBindingConfig(); c.bindings[2] = c.bindings[0]; c.bindings[2].id = 3;
    VERIFY(!WmfValidateBindings(&c,&e) && e.field == static_cast<unsigned>(ValidationField::Conflict));
    c = DefaultBindingConfig(); c.bindings[0].modifiers = ControlModifier;
    VERIFY(WmfValidateBindings(&c,&e));
    c = DefaultBindingConfig(); c.bindingCount = 0; VERIFY(WmfValidateBindings(&c,nullptr));
    c.bindingCount = MaximumBindings+1; VERIFY(!WmfValidateBindings(&c,nullptr));
    for (double value : {0.0,std::numeric_limits<double>::quiet_NaN(),std::numeric_limits<double>::infinity(),1000001.0}) {
        c = DefaultBindingConfig(); c.thresholdCounts = value; VERIFY(!WmfValidateBindings(&c,&e) && e.field == static_cast<unsigned>(ValidationField::Range));
    }
    c = DefaultBindingConfig(); c.shellMmPerCountX = -0.05; VERIFY(WmfValidateBindings(&c,&e));
    c.mmPerCountY = 0; VERIFY(!WmfValidateBindings(&c,&e));
    c = DefaultBindingConfig(); c.directionRatio = 1; VERIFY(!WmfValidateBindings(&c,&e));
    c = DefaultBindingConfig(); c.shellFingers = 2; VERIFY(!WmfValidateBindings(&c,&e));
    c = DefaultBindingConfig(); c.highMilliseconds = 1; VERIFY(!WmfValidateBindings(&c,&e));
    c = DefaultBindingConfig(); c.version--; VERIFY(WmfValidateBindings(&c,&e).code == Status::VersionMismatch);
    c = DefaultBindingConfig(); c.size--; VERIFY(WmfValidateBindings(&c,&e).code == Status::InvalidArgument);
    c = DefaultBindingConfig(); c.bindings[0].action = Action::ContentDrag; VERIFY(!WmfValidateBindings(&c,&e));
    c = DefaultBindingConfig(); c.bindings[0].action = Action::Shortcut;
    VERIFY(!WmfValidateBindings(&c,&e) && e.field == static_cast<unsigned>(ValidationField::Shortcut));
    c.bindings[0].keyCount = 2; c.bindings[0].keys[0] = 0x11; c.bindings[0].keys[1] = 0x43;
    VERIFY(WmfValidateBindings(&c,&e));
    c.bindings[0].keys[1] = 0x11; VERIFY(!WmfValidateBindings(&c,&e));
    c.bindings[0].keys[1] = 0x43; c.bindings[0].keys[7] = 0x42; VERIFY(!WmfValidateBindings(&c,&e));
    c = DefaultBindingConfig(); c.bindings[0].trigger = Trigger::Wheel; c.bindings[0].action = Action::NativeZoom;
    VERIFY(WmfValidateBindings(&c,&e));
    c.bindings[1].trigger = Trigger::WheelUp; c.bindings[1].action = Action::VolumeUp;
    VERIFY(!WmfValidateBindings(&c,&e) && e.field == static_cast<unsigned>(ValidationField::Conflict));
    c.bindings[0].trigger = Trigger::WheelDown; c.bindings[0].action = Action::VolumeDown;
    VERIFY(WmfValidateBindings(&c,&e));
    InputEvent input; input.kind = InputKind::Down; input.button = Button::Back;
    input.session = input.sequence = input.firstQpc = input.lastQpc = input.device = input.target = input.revision = 1;
    VERIFY(WmfValidateInput(&input));
    input.dx = 1; VERIFY(!WmfValidateInput(&input)); input.dx = 0;
    input.button = Button::None; VERIFY(!WmfValidateInput(&input)); input.button = Button::Back;
    input.lastQpc = 0; VERIFY(!WmfValidateInput(&input)); input.lastQpc = 1;
    input.kind = InputKind::Wheel; input.button = Button::None; input.wheelY = 1; VERIFY(WmfValidateInput(&input));
    input.wheelY = (std::numeric_limits<std::int64_t>::min)(); VERIFY(WmfValidateInput(&input).code == Status::OutOfRange);
    input = {}; input.kind = InputKind::Cancel; input.source = InputSource::Control; input.reason = CancelReason::Pause;
    VERIFY(WmfValidateInput(&input));
    input.source = static_cast<InputSource>(0); VERIFY(!WmfValidateInput(&input));
}
ContactFrame Pinch() {
    ContactFrame f; f.contacts[0] = {7,0,-5,0}; f.contacts[1] = {9,0,5,0}; return f;
}
void Geometry() {
    DeviceConfig device; MappedFrame mapped;
    for (auto fingers : {2u,3u,4u}) {
        for (double delta : {-0.01,0.0,0.01,4.25}) {
            GestureFrame translate; translate.fingers = fingers; translate.offsetXmm = delta; translate.offsetYmm = -delta;
            ContactFrame independent;
            VERIFY(WmfTranslateContacts(&translate,&independent));
            MappedFrame old;
            VERIFY(MapContacts(device,translate,old)); VERIFY(MapContacts(device,independent,mapped));
            VERIFY(mapped.count == old.count);
            for (unsigned i = 0; i < old.count; ++i) VERIFY(mapped.contacts[i].id == old.contacts[i].id && mapped.contacts[i].x == old.contacts[i].x && mapped.contacts[i].y == old.contacts[i].y);
        }
    }
    auto pinch = Pinch(); VERIFY(MapContacts(device,pinch,mapped));
    VERIFY(mapped.contacts[0].x == 4500 && mapped.contacts[1].x == 5500);
    pinch.contacts[0].xMm = -10; pinch.contacts[1].xMm = 10;
    VERIFY(MapContacts(device,pinch,mapped));
    VERIFY(mapped.contacts[0].x == 4000 && mapped.contacts[1].x == 6000 && mapped.contacts[0].y == 3000);
    pinch.contacts[1].id = pinch.contacts[0].id; VERIFY(!MapContacts(device,pinch,mapped));
    pinch = Pinch(); pinch.contacts[0].xMm = std::numeric_limits<double>::quiet_NaN(); VERIFY(!MapContacts(device,pinch,mapped));
    pinch = Pinch(); pinch.contacts[0].xMm = -50; VERIFY(MapContacts(device,pinch,mapped));
    pinch.contacts[0].xMm = -50.001; VERIFY(MapContacts(device,pinch,mapped).code == Status::OutOfRange);
    pinch = Pinch(); pinch.contacts[1].xMm = 50; VERIFY(MapContacts(device,pinch,mapped).code == Status::OutOfRange);
    pinch = Pinch(); pinch.count = 5; VERIFY(!MapContacts(device,pinch,mapped));
    pinch = Pinch(); pinch.version--; VERIFY(MapContacts(device,pinch,mapped).code == Status::VersionMismatch);
    pinch = Pinch(); pinch.contacts[0].reserved = 1; VERIFY(!MapContacts(device,pinch,mapped));
}
struct FakeDevice {
    int injected{},destroyed{},actions{},failAt{},failFrom{};
    std::uint64_t clock{1000000};
    std::vector<std::vector<POINTER_TYPE_INFO>> frames;
    NativeApi Api() {
        return {this,
            [](void*,SYNTHETIC_DEVICE_CREATION_PARAMS*) -> HSYNTHETICPOINTERDEVICE { return reinterpret_cast<HSYNTHETICPOINTERDEVICE>(static_cast<std::uintptr_t>(1)); },
            [](void* p,HSYNTHETICPOINTERDEVICE,const POINTER_TYPE_INFO* f,UINT32 n) -> BOOL {
                auto& s = *static_cast<FakeDevice*>(p); ++s.injected; s.frames.emplace_back(f,f+n);
                return s.injected != s.failAt && (!s.failFrom || s.injected < s.failFrom);
            },
            [](void* p,HSYNTHETICPOINTERDEVICE) { ++static_cast<FakeDevice*>(p)->destroyed; },
            [](void* p,HSYNTHETICPOINTERDEVICE,TOUCHPAD_ACTION) -> BOOL { ++static_cast<FakeDevice*>(p)->actions; return TRUE; },
            [](void* p) -> DWORD { return static_cast<FakeDevice*>(p)->injected > 2 ? 456 : 123; }};
    }
    FrameTiming Timing() {
        return {this,10000000,[](void* p) -> std::uint64_t { return static_cast<FakeDevice*>(p)->clock; },
            [](void* p,std::uint64_t until) -> bool { static_cast<FakeDevice*>(p)->clock = until; return true; }};
    }
};
void NativeGeometry() {
    DeviceConfig config; MappedFrame mapped;
    {
        FakeDevice f; DeviceSession s(f.Api(),f.Timing()); VERIFY(s.Open(config));
        auto frame = Pinch(); frame.phase = Phase::Cancel;
        VERIFY(s.Submit(frame,mapped).code == Status::InvalidState && f.injected == 0);
        frame = Pinch(); VERIFY(s.Submit(frame,mapped));
        frame.phase = Phase::Update; frame.sequence = 1; frame.logicalSeconds = 0.01;
        frame.contacts[0].xMm = -8; frame.contacts[1].xMm = 8;
        VERIFY(s.Submit(frame,mapped));
        VERIFY(f.frames.back()[0].touchInfo.pointerInfo.pointerId == 7 && f.frames.back()[1].touchInfo.pointerInfo.ptHimetricLocation.x == 5800);
        const auto calls = f.injected;
        VERIFY(s.Submit(frame,mapped).code == Status::InvalidState && f.injected == calls);
        frame.sequence = 2; frame.contacts[0].id = 8; VERIFY(s.Submit(frame,mapped).code == Status::InvalidState && f.injected == calls);
        frame.contacts[0].id = 7; frame.logicalSeconds = 0; VERIFY(s.Submit(frame,mapped).code == Status::InvalidState && f.injected == calls);
        frame.logicalSeconds = 0.02; frame.phase = Phase::End; VERIFY(s.Submit(frame,mapped));
        VERIFY((f.frames.back()[0].touchInfo.pointerInfo.pointerFlags & POINTER_FLAG_INCONTACT) == 0);
        frame = Pinch(); VERIFY(s.Submit(frame,mapped)); VERIFY(s.Close());
        VERIFY((f.frames.back()[0].touchInfo.pointerInfo.pointerFlags & POINTER_FLAG_CANCELED) != 0 && f.destroyed == 1);
    }
    {
        FakeDevice f; DeviceSession s(f.Api(),f.Timing()); VERIFY(s.Open(config));
        auto frame = Pinch(); VERIFY(s.Submit(frame,mapped));
        frame.phase = Phase::Update; frame.sequence = 1; frame.contacts[0].yMm = -1000;
        VERIFY(s.Submit(frame,mapped).code == Status::OutOfRange && f.injected == 1);
        VERIFY(s.Close());
        VERIFY(f.frames.back()[0].touchInfo.pointerInfo.ptHimetricLocation.y == 3000);
    }
    {
        FakeDevice f; f.failFrom = 2; DeviceSession s(f.Api(),f.Timing()); VERIFY(s.Open(config));
        auto frame = Pinch(); VERIFY(s.Submit(frame,mapped)); frame.phase = Phase::Update; frame.sequence = 1;
        const auto r = s.Submit(frame,mapped); VERIFY(r.nativeError == 123 && r.cleanupError == 456 && f.destroyed == 1);
        const auto calls = f.injected; VERIFY(s.Submit(frame,mapped).code == Status::InvalidState && f.injected == calls);
        VERIFY(!s.Close());
    }
    {
        FakeDevice f; DeviceSession s(f.Api(),f.Timing()); VERIFY(s.Open(config)); Result r;
        std::thread t([&] { auto frame = Pinch(); MappedFrame m; r = s.Submit(frame,m); }); t.join();
        VERIFY(r.code == Status::WrongThread && f.injected == 0); VERIFY(s.Close());
    }
}
struct Bindings {
    BindingConfig config;
    BindingHandle handle{};
    InputEvent input;
    std::uint64_t now{1000};
    explicit Bindings(BindingConfig c = DefaultBindingConfig()) : config(c) {
        config.qpcFrequency = 1000; VERIFY(WmfBindingsCreate(&config,&handle));
        input.device = 7; input.target = 9; input.revision = config.revision;
    }
    ~Bindings() { VERIFY(WmfBindingsDestroy(&handle)); }
    BindingDecision Send(InputKind kind,Button button = Button::Back,std::int64_t dx = 0,std::int64_t dy = 0,std::int64_t wheel = 0) {
        if (kind == InputKind::Down) ++input.session;
        ++input.sequence; ++now; input.firstQpc = input.lastQpc = now; input.kind = kind; input.button = button;
        input.dx = dx; input.dy = dy; input.wheelY = wheel;
        BindingDecision out; VERIFY(WmfBindingsFeed(handle,&input,&out)); return out;
    }
    BindingDecision Tick(std::uint64_t at) {
        now = at; InputEvent e; e.kind = InputKind::Tick; e.source = InputSource::Control; e.firstQpc = e.lastQpc = at;
        BindingDecision out; VERIFY(WmfBindingsFeed(handle,&e,&out)); return out;
    }
};
void BindingArbitration() {
    {
        Bindings b; auto out = b.Send(InputKind::Down);
        VERIFY(out.state == BindingState::Pending && (out.effects & DecisionStopInertia) && !out.frameCount);
        std::uint64_t deadline{}; VERIFY(WmfBindingsDeadline(b.handle,&deadline) && !deadline);
        b.Tick(20000); out = b.Send(InputKind::Up);
        VERIFY(out.state == BindingState::Ended && out.action == Action::Back && (out.effects & DecisionExecute));
        VERIFY(out.frameCount == 0 && (out.effects & DecisionFinished));
    }
    {
        Bindings b; b.Send(InputKind::Down); auto out = b.Send(InputKind::Up,Button::Back,8,0);
        VERIFY(out.frameCount == 3 && out.frames[0].phase == Phase::Begin && out.frames[1].phase == Phase::Update && out.frames[2].phase == Phase::End);
        VERIFY(out.frames[1].contacts[0].xMm == out.frames[2].contacts[0].xMm && !(out.effects & DecisionExecute));
    }
    for (auto direction : {Direction::Up,Direction::Down,Direction::Left,Direction::Right}) {
        Bindings b; b.Send(InputKind::Down,Button::Forward);
        const std::int64_t dx = direction == Direction::Left ? -20 : direction == Direction::Right ? 20 : 0;
        const std::int64_t dy = direction == Direction::Up ? -20 : direction == Direction::Down ? 20 : 0;
        auto out = b.Send(InputKind::Move,Button::Forward,dx,dy);
        VERIFY(out.state == BindingState::ShellGesture && out.direction == direction && out.targetPolicy == TargetPolicy::InputDesktop);
        VERIFY(out.frameCount == 2 && out.frames[0].count == 4 && out.frames[0].sequence == 0 && out.frames[1].sequence == 1);
        out = b.Send(InputKind::Move,Button::Forward); VERIFY(out.frameCount == 0);
        out = b.Send(InputKind::Move,Button::Forward,-2*dx,-2*dy);
        VERIFY(out.frameCount == 1 && out.frames[0].phase == Phase::Update && out.direction == direction && out.frames[0].contacts[0].id == 1);
        out = b.Send(InputKind::Up,Button::Forward);
        VERIFY(out.frameCount == 1 && out.frames[0].phase == Phase::End && !(out.effects & DecisionExecute));
    }
    {
        Bindings b; b.Send(InputKind::Down,Button::Forward); auto out = b.Send(InputKind::Move,Button::Forward,9,9);
        VERIFY(out.state == BindingState::Pending && out.frameCount == 0);
        out = b.Send(InputKind::Move,Button::Forward,0,-30);
        VERIFY(out.state == BindingState::ShellGesture && out.direction == Direction::Up);
    }
    auto doubleConfig = DefaultBindingConfig();
    doubleConfig.bindings[doubleConfig.bindingCount++] = {5,Button::Back,Trigger::DoubleClick,0,Action::Mute};
    {
        Bindings b(doubleConfig); b.Send(InputKind::Down); auto out = b.Send(InputKind::Up);
        VERIFY(out.state == BindingState::WaitingSecondClick && !(out.effects & DecisionExecute));
        std::uint64_t deadline{}; VERIFY(WmfBindingsDeadline(b.handle,&deadline) && deadline == b.now+500);
        out = b.Tick(deadline-1); VERIFY(!(out.effects & DecisionExecute));
        out = b.Tick(deadline); VERIFY(out.action == Action::Back && (out.effects & DecisionExecute) && out.actionInput.session == 1);
    }
    {
        Bindings b(doubleConfig); b.Send(InputKind::Down); b.Send(InputKind::Up); b.Send(InputKind::Down);
        auto out = b.Send(InputKind::Up); VERIFY(out.action == Action::Mute && (out.effects & DecisionExecute));
        out = b.Tick(b.now+1000); VERIFY(!(out.effects & DecisionExecute));
    }
    {
        Bindings b(doubleConfig); b.Send(InputKind::Down); b.Send(InputKind::Up); b.Send(InputKind::Down);
        b.Send(InputKind::Move,Button::Back,20); auto out = b.Send(InputKind::Up);
        VERIFY(out.action == Action::ContentDrag && !(out.effects & DecisionExecute));
        out = b.Tick(b.now+1000); VERIFY(!(out.effects & DecisionExecute));
    }
    auto holdConfig = DefaultBindingConfig(); holdConfig.bindings[holdConfig.bindingCount++] = {5,Button::Back,Trigger::Hold,0,Action::Mute};
    {
        Bindings b(holdConfig); b.Send(InputKind::Down); auto out = b.Tick(b.now+499);
        VERIFY(!(out.effects & DecisionExecute)); out = b.Tick(b.now+1);
        VERIFY(out.state == BindingState::Held && out.action == Action::Mute && (out.effects & DecisionExecute));
        out = b.Send(InputKind::Up); VERIFY(!(out.effects & DecisionExecute) && (out.effects & DecisionFinished));
    }
    {
        Bindings b(holdConfig); b.Send(InputKind::Down); b.Send(InputKind::Move,Button::Back,20);
        auto out = b.Tick(b.now+1000); VERIFY(!(out.effects & DecisionExecute));
        out = b.Send(InputKind::Cancel); VERIFY(out.frameCount == 1 && out.frames[0].phase == Phase::Cancel && (out.effects & DecisionCanceled));
    }
    auto zoomConfig = DefaultBindingConfig(); zoomConfig.bindings[zoomConfig.bindingCount++] = {5,Button::Back,Trigger::Wheel,0,Action::NativeZoom};
    {
        Bindings b(zoomConfig); b.Send(InputKind::Down); auto out = b.Send(InputKind::Wheel,Button::Back,0,0,1);
        VERIFY(out.state == BindingState::Zoom && out.frameCount == 1 && out.frames[0].contacts[1].xMm == 10);
        out = b.Send(InputKind::Wheel,Button::Back,0,0,119);
        VERIFY(!out.frameCount); out = b.Tick(b.now+100);
        VERIFY(out.frameCount == 1 && std::abs(out.frames[0].contacts[1].xMm-12) < 1e-10);
        out = b.Send(InputKind::Wheel,Button::Back,0,0,-120);
        VERIFY(!out.frameCount); out = b.Tick(b.now+100);
        VERIFY(out.frames[0].phase == Phase::Update && std::abs(out.frames[0].contacts[1].xMm-10) < 1e-10);
        out = b.Send(InputKind::Up); VERIFY(out.frames[0].phase == Phase::End && !(out.effects & DecisionExecute));
    }
    auto wheelConfig = DefaultBindingConfig();
    wheelConfig.bindings[wheelConfig.bindingCount++] = {5,Button::Back,Trigger::WheelUp,0,Action::VolumeUp};
    wheelConfig.bindings[wheelConfig.bindingCount++] = {6,Button::Back,Trigger::WheelDown,0,Action::VolumeDown};
    {
        Bindings b(wheelConfig); b.Send(InputKind::Down); auto out = b.Send(InputKind::Wheel,Button::Back,0,0,60);
        VERIFY(out.state == BindingState::Held && !(out.effects & DecisionExecute));
        out = b.Send(InputKind::Up); VERIFY(!(out.effects & DecisionExecute));
        b.Send(InputKind::Down); b.Send(InputKind::Wheel,Button::Back,0,0,60); out = b.Send(InputKind::Wheel,Button::Back,0,0,180);
        VERIFY(out.action == Action::VolumeUp && out.repeatCount == 2 && (out.effects & DecisionExecute));
        out = b.Send(InputKind::Wheel,Button::Back,0,0,-120); VERIFY(out.action == Action::VolumeDown && out.repeatCount == 1);
    }
    {
        Bindings b; b.Send(InputKind::Down); InputEvent injected = b.input; injected.kind = InputKind::Move; injected.source = InputSource::OwnInjection;
        injected.dx = 100; BindingDecision out; VERIFY(WmfBindingsFeed(b.handle,&injected,&out));
        VERIFY(out.state == BindingState::Pending && out.rawX == 0);
        auto invalid = b.input; invalid.kind = InputKind::Move; ++invalid.sequence; invalid.modifiers = ShiftModifier;
        VERIFY(WmfBindingsFeed(b.handle,&invalid,&out).code == Status::InvalidState);
        out = b.Send(InputKind::Up); VERIFY(out.action == Action::Back && (out.effects & DecisionExecute));
    }
}
}
int RunProductTests(int& total) {
    ContractValidation(); Geometry(); NativeGeometry(); BindingArbitration();
    auto config = DefaultBindingConfig(); config.qpcFrequency = 1000;
    PinchTrajectory whole,split; whole.Reset(config,100); split.Reset(config,100);
    VERIFY(whole.Feed(120,100));
    for (int i = 0; i < 120; ++i) VERIFY(split.Feed(1,100));
    VERIFY(std::abs(whole.goal-split.goal) < 1e-10 && whole.radius == 10 && split.radius == 10);
    whole.Sample(110); const auto emitted = whole.radius;
    VERIFY(emitted > 10 && emitted < 12 && whole.Feed(-120,120));
    VERIFY(whole.radius == emitted && std::abs(whole.goal-emitted/1.2) < 1e-10);
    whole.Sample(130); VERIFY(whole.radius < emitted);
    whole.Sample(180); VERIFY(std::abs(whole.radius-emitted/1.2) < 1e-10);
    for (const auto delta : {-120000ll,120000ll}) {
        whole.Reset(config,100); VERIFY(whole.Feed(delta,100)); const auto end = whole.end;
        for (int i = 0; i < 100; ++i) VERIFY(whole.Feed(delta,101));
        VERIFY(whole.end == end); whole.Sample(200);
        VERIFY(whole.radius == (delta < 0 ? 2 : 200));
        VERIFY(whole.Feed(delta < 0 ? 120 : -120,201)); whole.Sample(261);
        VERIFY(std::abs(whole.radius-(delta < 0 ? 2*1.2 : 200/1.2)) < 1e-9);
    }
    config.zoomMmPerNotch = -2; whole.Reset(config,100); VERIFY(whole.Feed(120,100)); whole.Sample(160);
    VERIFY(std::abs(whole.radius-10/1.2) < 1e-10);
    config.initialPinchMm = 1; whole.Reset(config,100); VERIFY(whole.Feed(120000,100)); whole.Sample(160);
    VERIFY(whole.radius == 1 && whole.Deadline() > whole.now);
    {
        Bindings b; b.config.bindings[b.config.bindingCount++] = {5,Button::Back,Trigger::Wheel,0,Action::NativeZoom};
        VERIFY(WmfBindingsDestroy(&b.handle)); VERIFY(WmfBindingsCreate(&b.config,&b.handle)); b.Send(InputKind::Down);
        b.Send(InputKind::Wheel,Button::Back,0,0,1); std::uint64_t first{}; VERIFY(WmfBindingsDeadline(b.handle,&first));
        for (int i = 0; i < 20; ++i) b.Send(InputKind::Wheel,Button::Back,0,0,1);
        std::uint64_t deadline{}; VERIFY(WmfBindingsDeadline(b.handle,&deadline) && deadline == first && deadline < b.now);
        const auto output = b.Tick(b.now+1); VERIFY(output.frameCount == 1 && output.frames[0].phase == Phase::Update);
    }
    total += checks; return failures;
}
