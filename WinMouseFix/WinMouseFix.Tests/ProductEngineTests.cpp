#include "../WinMouseFix.Host/ProductEngine.h"
#include "../WinMouseFix.Host/ProductInputBuffer.h"
#include <vector>
#include <algorithm>
#include <iostream>

namespace {
using namespace wmf;
using namespace wmf::host;
int checks{},failures{};
#define VERIFY(...) do { ++checks; if (!(__VA_ARGS__)) { ++failures; std::cerr << __FILE__ << ':' << __LINE__ << ": " << #__VA_ARGS__ << '\n'; } } while(false)
struct Fake;
thread_local Fake* active{};
struct Fake {
    bool foreground{true},desktop{true},policyOk{true},failCreate{},failOpen{},failCleanup{};
    int attempts{},failSubmit{-1},destroyed{},stops{},policies{};
    bool nativeContact{};
    std::uint64_t nativeSequence{};
    std::uint32_t nativeCount{};
    std::uint64_t now{1000};
    TargetPolicy selected{TargetPolicy::Application};
    std::vector<ContactFrame> frames;
    std::vector<ActionRequest> actions;
    std::vector<std::uint64_t> completed,canceled;
    std::vector<ProductTrace> trace;
    ProductFunctions Api() {
        active = this; ProductFunctions a; a.context = this;
        a.capabilities = [](Capabilities* caps) noexcept -> Result { caps->apiBits = RequiredApis|ActionApi; return {}; };
        a.createPlatform = [](PlatformHandle* out) noexcept -> Result {
            if (active->failCreate) return {Status::NativeFailure,42}; *out = reinterpret_cast<PlatformHandle>(active); return {};
        };
        a.openPlatform = [](PlatformHandle,const DeviceConfig*) noexcept -> Result { return active->failOpen ? Result{Status::NativeFailure,43} : Result{}; };
        a.submit = [](PlatformHandle,const ContactFrame* f,MappedFrame*) noexcept -> Result {
            if (++active->attempts == active->failSubmit) { active->nativeContact = false; return {Status::NativeFailure,123,456}; }
            if (f->phase == Phase::Begin) { if (active->nativeContact || f->sequence) return {Status::InvalidState}; }
            else if (!active->nativeContact || f->sequence != active->nativeSequence+1 || f->count != active->nativeCount) return {Status::InvalidState};
            active->nativeSequence = f->sequence; active->nativeCount = f->count;
            active->nativeContact = f->phase != Phase::End && f->phase != Phase::Cancel;
            active->frames.push_back(*f); return {};
        };
        a.stopInertia = [](PlatformHandle,std::uint32_t) noexcept -> Result { ++active->stops; return {}; };
        a.destroyPlatform = [](PlatformHandle* out) noexcept -> Result {
            if (!*out) return {}; *out = nullptr; ++active->destroyed;
            return active->failCleanup ? Result{Status::NativeFailure,789} : Result{};
        };
        a.execute = [](const ActionRequest* r) noexcept -> Result { active->actions.push_back(*r); return {}; };
        a.clock = [](void* p) noexcept { return ++static_cast<Fake*>(p)->now; };
        a.targetValid = [](void* p,const InputEvent&,TargetPolicy policy,bool) noexcept {
            auto& f = *static_cast<Fake*>(p); return f.desktop && (policy == TargetPolicy::InputDesktop || f.foreground);
        };
        a.policy = [](void* p,std::uint64_t,TargetPolicy policy) noexcept -> Result {
            auto& f = *static_cast<Fake*>(p); ++f.policies; f.selected = policy;
            return f.policyOk ? Result{} : Result{Status::InvalidState};
        };
        a.complete = [](void* p,std::uint64_t session,bool cancel) noexcept {
            auto& f = *static_cast<Fake*>(p); (cancel ? f.canceled : f.completed).push_back(session);
        };
        a.trace = [](void* p,const ProductTrace& t) noexcept { static_cast<Fake*>(p)->trace.push_back(t); };
        return a;
    }
};
InputEvent Event(InputKind kind,Button button,std::uint64_t session,std::uint64_t sequence,std::int64_t x = 0,std::int64_t y = 0) {
    InputEvent e; e.kind = kind; e.button = button; e.session = session; e.sequence = sequence;
    e.firstQpc = e.lastQpc = 100+sequence+session*100; e.device = 7; e.target = 9; e.revision = 1; e.dx = x; e.dy = y; return e;
}
void DirectPath() {
    Fake f; ProductEngine e(DefaultBindingConfig(),f.Api()); VERIFY(e.Initialize());
    VERIFY(e.Process(Event(InputKind::Down,Button::Back,1,1))); VERIFY(e.Process(Event(InputKind::Up,Button::Back,1,2)));
    VERIFY(f.actions.size() == 1 && f.actions.back().binding.action == Action::Back && f.frames.empty());
    VERIFY(e.Process(Event(InputKind::Down,Button::Back,2,1)));
    VERIFY(e.Process(Event(InputKind::Move,Button::Back,2,2,8,3)));
    VERIFY(f.frames.size() == 2 && f.frames[0].phase == Phase::Begin && f.frames[1].phase == Phase::Update);
    const auto point = f.frames.back().contacts[0]; VERIFY(point.xMm == -4.2 && point.yMm > 0.29 && point.yMm < 0.31);
    VERIFY(e.Process(Event(InputKind::Move,Button::Back,2,3,-2,-1)));
    VERIFY(e.Process(Event(InputKind::Up,Button::Back,2,4,2,1)));
    VERIFY(f.frames.back().phase == Phase::End && f.frames.back().contacts[0].xMm == point.xMm);
    VERIFY(!e.ContactActive() && !e.Pressed() && f.actions.size() == 1);
    VERIFY(e.Process(Event(InputKind::Down,Button::Forward,3,1))); VERIFY(f.stops == 1);
    VERIFY(e.Process(Event(InputKind::Move,Button::Forward,3,2,1,-15)));
    VERIFY(f.selected == TargetPolicy::InputDesktop && f.frames.back().count == 4);
    const auto shellStart = f.frames.size(); f.foreground = false;
    VERIFY(e.Process(Event(InputKind::Move,Button::Forward,3,3,0,5)));
    VERIFY(f.frames.size() == shellStart+1 && f.frames.back().phase == Phase::Update);
    VERIFY(e.Process(Event(InputKind::Up,Button::Forward,3,4)));
    VERIFY(f.frames.back().phase == Phase::End && f.actions.size() == 1);
    VERIFY(e.Close()); VERIFY(f.destroyed == 1 && f.stops == 2);
}
void TargetAndRange() {
    for (bool shell : {false,true}) {
        Fake f; ProductEngine e(DefaultBindingConfig(),f.Api()); VERIFY(e.Initialize());
        const auto button = shell ? Button::Forward : Button::Back;
        VERIFY(e.Process(Event(InputKind::Down,button,1,1)));
        VERIFY(e.Process(Event(InputKind::Move,button,1,2,0,-20)));
        const auto last = f.frames.back(); f.desktop = false;
        VERIFY(e.Process(Event(InputKind::Move,button,1,3,0,-10)));
        VERIFY(f.frames.back().phase == Phase::Cancel && f.frames.back().sequence == last.sequence+1);
        VERIFY(f.frames.back().contacts[0].yMm == last.contacts[0].yMm && f.canceled.size() == 1);
        VERIFY(e.Process(Event(InputKind::Up,button,1,4)) && f.actions.empty());
        f.desktop = true;
        VERIFY(e.Process(Event(InputKind::Down,button,2,1)));
        const auto size = f.frames.size(); VERIFY(e.Process(Event(InputKind::Move,button,2,2,0,100000)));
        VERIFY(f.frames.size() == size && !e.ContactActive() && f.canceled.size() == 2);
        VERIFY(e.Process(Event(InputKind::Down,button,3,1)));
        VERIFY(e.Process(Event(InputKind::Move,button,3,2,0,-10)));
        const auto valid = f.frames.back(); VERIFY(e.Process(Event(InputKind::Move,button,3,3,0,-100000)));
        VERIFY(f.frames.back().phase == Phase::Cancel && f.frames.back().contacts[0].yMm == valid.contacts[0].yMm);
        VERIFY(e.Close());
    }
    Fake rejected; rejected.policyOk = false;
    ProductEngine e(DefaultBindingConfig(),rejected.Api()); VERIFY(e.Initialize());
    VERIFY(e.Process(Event(InputKind::Down,Button::Forward,1,1)));
    VERIFY(e.Process(Event(InputKind::Move,Button::Forward,1,2,0,-10)) && rejected.frames.empty());
}
void FailuresAndSnapshots() {
    for (int failed : {1,2,3}) {
        Fake f; f.failSubmit = failed; ProductEngine e(DefaultBindingConfig(),f.Api()); VERIFY(e.Initialize());
        VERIFY(e.Process(Event(InputKind::Down,Button::Back,1,1)));
        auto result = e.Process(Event(InputKind::Move,Button::Back,1,2,10,0));
        if (failed == 3) result = e.Process(Event(InputKind::Up,Button::Back,1,3));
        VERIFY(result.nativeError == 123 && result.cleanupError == 456);
        const auto attempts = f.attempts; VERIFY(e.Close()); VERIFY(f.attempts == attempts && f.destroyed == 1);
    }
    Fake f; ProductEngine e(DefaultBindingConfig(),f.Api()); VERIFY(e.Initialize());
    VERIFY(e.Process(Event(InputKind::Down,Button::Back,1,1)));
    auto next = DefaultBindingConfig(); next.revision = 2;
    VERIFY(e.Configure(next).code == Status::Busy);
    auto stale = Event(InputKind::Cancel,Button::Back,99,1); VERIFY(e.Process(stale) && e.Pressed());
    VERIFY(e.Process(Event(InputKind::Up,Button::Back,1,2))); VERIFY(e.Configure(next) && e.Revision() == 2);
    auto down = Event(InputKind::Down,Button::Forward,2,1); down.revision = 2;
    VERIFY(e.Process(down)); auto up = down; up.kind = InputKind::Up; ++up.sequence; ++up.lastQpc; up.firstQpc = up.lastQpc;
    VERIFY(e.Process(up)); VERIFY(f.actions.back().binding.action == Action::Forward);
    auto record = down; record.flags = RecordedInput; record.session = 3; VERIFY(e.Process(record));
    const auto actions = f.actions.size(); record.kind = InputKind::Up; ++record.sequence; VERIFY(e.Process(record));
    VERIFY(f.actions.size() == actions && f.frames.empty()); VERIFY(e.Close());
}
void Queue() {
    ProductInputBuffer q; auto a = Event(InputKind::Move,Button::Back,1,1,2,3); auto b = a; ++b.sequence; ++b.lastQpc;
    VERIFY(q.Push(a) && q.Push(b)); InputEvent merged; VERIFY(q.Pop(merged));
    VERIFY(merged.samples == 2 && merged.dx == 4 && merged.firstQpc == a.firstQpc && merged.lastQpc == b.lastQpc);
    for (int field = 0; field < 6; ++field) {
        auto changed = b;
        if (field == 0) ++changed.revision;
        if (field == 1) changed.button = Button::Forward;
        if (field == 2) changed.modifiers = ShiftModifier;
        if (field == 3) changed.flags = RecordedInput;
        if (field == 4) changed.dx = -2;
        if (field == 5) changed.kind = InputKind::Up;
        VERIFY(!ProductInputBuffer::Merge(a,changed));
    }
    for (unsigned i = 0; i < 512; ++i) VERIFY(q.Push(a)); VERIFY(!q.Push(a)); q.Discard(); VERIFY(q.Push(a));
}
void WheelLifecycle() {
    Fake f; auto config = DefaultBindingConfig(); config.wheelMode = WheelMode::High; config.qpcFrequency = 1000000;
    ProductEngine e(config,f.Api()); VERIFY(e.Initialize());
    auto wheel = Event(InputKind::Wheel,Button::None,1,1); wheel.wheelY = 120;
    VERIFY(e.Process(wheel) && e.ContactActive() && !e.Pressed() && e.Deadline());
    VERIFY(f.frames.size() == 1 && f.frames[0].phase == Phase::Begin && f.frames[0].sequence == 0);
    f.now = 90000; VERIFY(e.Tick(f.now)); const auto p = f.frames.back().contacts[0].yMm; VERIFY(p > 0 && p < 15);
    wheel.session = 2; wheel.wheelY = -120; wheel.firstQpc = wheel.lastQpc = f.now;
    VERIFY(e.Process(wheel)); VERIFY(f.frames.back().phase == Phase::Update && f.frames.back().contacts[0].yMm == p);
    f.now += 200000; VERIFY(e.Tick(f.now)); VERIFY(!e.Deadline() && !e.ContactActive());
    VERIFY(f.frames.back().phase == Phase::End && f.frames.back().contacts[0].yMm < 0);
    const auto ledger = f.trace.back(); VERIFY(ledger.wheelLedger && std::abs(ledger.wheelAcceptedY-ledger.wheelDiscardedY-ledger.wheelEmittedY) < 1e-8);
    wheel.session = 3; wheel.firstQpc = wheel.lastQpc = ++f.now; const auto restarted = e.Process(wheel);
    if (!restarted) std::cerr << "wheel restart result " << static_cast<unsigned>(restarted.code) << '/' << restarted.nativeError << '/' << restarted.cleanupError << '\n';
    VERIFY(restarted); VERIFY(f.stops == 1);
    auto down = Event(InputKind::Down,Button::Back,4,1); down.firstQpc = down.lastQpc = ++f.now;
    VERIFY(e.Process(down)); VERIFY(f.frames.back().phase == Phase::Cancel && !e.Deadline() && e.Pressed());
    auto up = down; up.kind = InputKind::Up; ++up.sequence; VERIFY(e.Process(up) && f.actions.size() == 1);
    wheel.session = 5; wheel.firstQpc = wheel.lastQpc = ++f.now; VERIFY(e.Process(wheel));
    f.foreground = false; const auto last = f.frames.back(); f.now += 20000; VERIFY(e.Tick(f.now));
    VERIFY(!e.ContactActive() && !e.Deadline() && f.frames.back().phase == Phase::Cancel && f.frames.back().contacts[0].yMm == last.contacts[0].yMm);
    VERIFY(e.Close());
}
void PinchOwnsOutput() {
    auto config = DefaultBindingConfig(); config.wheelMode = WheelMode::Regular;
    config.bindings[config.bindingCount++] = {5,Button::Back,Trigger::Wheel,0,Action::NativeZoom};
    Fake f; ProductEngine e(config,f.Api()); VERIFY(e.Initialize());
    auto wheel = Event(InputKind::Wheel,Button::None,1,1); wheel.wheelY = 120; VERIFY(e.Process(wheel));
    VERIFY(e.Process(Event(InputKind::Down,Button::Back,2,1))); VERIFY(f.frames.back().phase == Phase::Cancel);
    auto combo = Event(InputKind::Wheel,Button::Back,2,2); combo.wheelY = 60; VERIFY(e.Process(combo));
    f.now += config.qpcFrequency/10; VERIFY(e.Tick(f.now));
    const auto last = f.frames.back(); VERIFY(last.count == 2 && last.phase == Phase::Update);
    VERIFY(last.contacts[0].xMm == -last.contacts[1].xMm && last.contacts[0].yMm == 0 && last.contacts[0].xMm < -10);
    f.foreground = false; combo.sequence = 3; combo.firstQpc = combo.lastQpc = 400; VERIFY(e.Process(combo));
    VERIFY(f.frames.back().phase == Phase::Cancel && f.frames.back().contacts[0].xMm == last.contacts[0].xMm);
    VERIFY(e.Process(Event(InputKind::Up,Button::Back,2,4)) && f.actions.empty()); VERIFY(e.Close());
}
BindingConfig ZoomConfig() {
    auto config = DefaultBindingConfig(); config.qpcFrequency = 1000000;
    config.bindings[config.bindingCount++] = {5,Button::Back,Trigger::Wheel,0,Action::NativeZoom};
    return config;
}
void PinchTrajectoryAndRelease() {
    Fake f; const auto config = ZoomConfig(); ProductEngine e(config,f.Api()); VERIFY(e.Initialize());
    VERIFY(e.Process(Event(InputKind::Down,Button::Back,1,1)));
    auto wheel = Event(InputKind::Wheel,Button::Back,1,2); wheel.wheelY = 120;
    VERIFY(e.Process(wheel)); VERIFY(f.frames.size() == 1 && f.frames.back().phase == Phase::Begin);
    VERIFY(f.frames.back().contacts[1].xMm == 10 && e.Deadline());
    double previous = 10;
    for (int i = 0; i < 6; ++i) {
        f.now += 10000; VERIFY(e.Tick(f.now)); const auto& frame = f.frames.back();
        VERIFY(frame.phase == Phase::Update && frame.contacts[1].xMm > previous && frame.contacts[1].xMm <= 12);
        VERIFY(frame.contacts[0].xMm == -frame.contacts[1].xMm && frame.contacts[0].id == 1 && frame.contacts[1].id == 2);
        previous = frame.contacts[1].xMm;
    }
    VERIFY(std::abs(previous-12) < 1e-10 && e.Deadline() && e.ContactActive());
    wheel.sequence = 3; wheel.wheelY = -120; wheel.firstQpc = wheel.lastQpc = ++f.now; VERIFY(e.Process(wheel));
    f.now += 10000; VERIFY(e.Tick(f.now)); const auto partial = f.frames.back();
    VERIFY(partial.contacts[1].xMm > 10 && partial.contacts[1].xMm < 12);
    auto up = Event(InputKind::Up,Button::Back,1,4); up.firstQpc = up.lastQpc = ++f.now; VERIFY(e.Process(up));
    VERIFY(f.frames.back().phase == Phase::End && f.frames.back().sequence == partial.sequence+1);
    VERIFY(f.frames.back().contacts[1].xMm == partial.contacts[1].xMm && !e.Deadline() && !e.ContactActive());
    const auto count = f.frames.size(); f.now += 100000; VERIFY(e.Tick(f.now));
    VERIFY(f.frames.size() == count && f.actions.empty() && f.stops == 0); VERIFY(e.Close());
}
void PinchBoundaryRecovery() {
    for (const auto amount : {-120000ll,120000ll}) for (bool begun : {false,true}) {
        Fake f; ProductEngine e(ZoomConfig(),f.Api()); VERIFY(e.Initialize());
        VERIFY(e.Process(Event(InputKind::Down,Button::Back,1,1)));
        auto wheel = Event(InputKind::Wheel,Button::Back,1,2); wheel.wheelY = -120;
        if (begun) {
            VERIFY(e.Process(wheel)); f.now += 70000; VERIFY(e.Tick(f.now));
            ++wheel.sequence; wheel.firstQpc = wheel.lastQpc = ++f.now;
        }
        wheel.wheelY = amount; VERIFY(e.Process(wheel));
        f.now += 70000; VERIFY(e.Tick(f.now));
        const double bound = amount < 0 ? 2 : 200;
        VERIFY(e.Ready() && e.Pressed() && e.ContactActive() && e.Deadline() && f.canceled.empty());
        VERIFY(f.frames.back().contacts[1].xMm == bound);
        for (int i = 0; i < 200; ++i) {
            ++wheel.sequence; wheel.firstQpc = wheel.lastQpc = ++f.now; VERIFY(e.Process(wheel));
        }
        f.now += 70000; VERIFY(e.Tick(f.now));
        VERIFY(f.frames.back().contacts[1].xMm == bound && f.trace.back().stationary && f.canceled.empty());
        const auto last = f.frames.back(); ++wheel.sequence; wheel.wheelY = amount < 0 ? 120 : -120;
        wheel.firstQpc = wheel.lastQpc = ++f.now; VERIFY(e.Process(wheel));
        f.now += 10000; VERIFY(e.Tick(f.now)); const auto reversed = f.frames.back();
        VERIFY(reversed.sequence == last.sequence+1 && reversed.phase == Phase::Update);
        VERIFY(amount < 0 ? reversed.contacts[1].xMm > bound : reversed.contacts[1].xMm < bound);
        f.now += 70000; VERIFY(e.Tick(f.now));
        VERIFY(std::abs(f.frames.back().contacts[1].xMm-(amount < 0 ? bound*1.2 : bound/1.2)) < 1e-9);
        VERIFY(std::count_if(f.frames.begin(),f.frames.end(),[](const auto& frame) { return frame.phase == Phase::Begin; }) == 1);
        auto up = wheel; up.kind = InputKind::Up; up.wheelY = 0; ++up.sequence; up.firstQpc = up.lastQpc = ++f.now;
        VERIFY(e.Process(up)); VERIFY(!e.Pressed() && !e.ContactActive() && !e.Deadline() && f.actions.empty());
        VERIFY(e.Close());
    }
}
void PinchQueuedInputAndCancellation() {
    for (bool paused : {false,true}) {
        Fake f; ProductEngine e(ZoomConfig(),f.Api()); VERIFY(e.Initialize());
        VERIFY(e.Process(Event(InputKind::Down,Button::Back,1,1)));
        auto wheel = Event(InputKind::Wheel,Button::Back,1,2); wheel.wheelY = -120000; VERIFY(e.Process(wheel));
        f.now += 70000; VERIFY(e.Tick(f.now));
        // This raw event was captured after the last wheel, but before the
        // previous worker Tick. It remains valid and reverses actual output.
        ++wheel.sequence; ++wheel.firstQpc; wheel.lastQpc = wheel.firstQpc; wheel.wheelY = 120;
        VERIFY(e.Process(wheel)); f.now += 10000; VERIFY(e.Tick(f.now));
        VERIFY(f.frames.back().contacts[1].xMm > 2 && f.frames.back().contacts[1].xMm < 2.4);
        f.now += 70000; VERIFY(e.Tick(f.now)); const auto actual = f.frames.back();
        if (paused) VERIFY(e.Cancel(CancelReason::Pause));
        else { f.foreground = false; f.now = e.Deadline(); VERIFY(e.Tick(f.now)); }
        VERIFY(f.frames.back().phase == Phase::Cancel && f.frames.back().contacts[1].xMm == actual.contacts[1].xMm && !e.Deadline());
        VERIFY(e.Close());
    }
}
void PinchReverseAndFailure() {
    for (bool fail : {false,true}) {
        Fake f; ProductEngine e(ZoomConfig(),f.Api()); VERIFY(e.Initialize());
        VERIFY(e.Process(Event(InputKind::Down,Button::Back,1,1)));
        auto wheel = Event(InputKind::Wheel,Button::Back,1,2); wheel.wheelY = 120; VERIFY(e.Process(wheel));
        f.now += 10000; VERIFY(e.Tick(f.now)); const auto partial = f.frames.back();
        ++wheel.sequence; wheel.wheelY = -120; wheel.firstQpc = wheel.lastQpc = ++f.now; VERIFY(e.Process(wheel));
        if (fail) f.failSubmit = f.attempts+1;
        f.now += 10000; const auto result = e.Tick(f.now);
        if (fail) {
            VERIFY(result.code == Status::NativeFailure && !e.ContactActive() && !e.Deadline());
            const auto attempts = f.attempts; VERIFY(e.Close()); VERIFY(f.attempts == attempts);
        } else {
            VERIFY(result && f.frames.back().contacts[1].xMm < partial.contacts[1].xMm);
            VERIFY(f.frames.back().phase == Phase::Update && f.frames.back().sequence == partial.sequence+1);
            f.foreground = false; const auto actual = f.frames.back(); f.now += 10000; VERIFY(e.Tick(f.now));
            VERIFY(f.frames.back().phase == Phase::Cancel && f.frames.back().contacts[1].xMm == actual.contacts[1].xMm && !e.Deadline());
            VERIFY(e.Close());
        }
    }
}
void PinchHeldAcrossPauses() {
    Fake f; ProductEngine e(ZoomConfig(),f.Api()); VERIFY(e.Initialize() && !e.Deadline());
    VERIFY(e.Process(Event(InputKind::Down,Button::Back,1,1)) && !e.Deadline());
    auto wheel = Event(InputKind::Wheel,Button::Back,1,2); wheel.wheelY = 120; VERIFY(e.Process(wheel));
    f.now += 70000; VERIFY(e.Tick(f.now)); auto last = f.frames.back();
    for (int i = 0; i < 60; ++i) {
        const auto deadline = e.Deadline(); VERIFY(deadline > f.now && deadline-f.now <= 16666);
        f.now = deadline; VERIFY(e.Tick(f.now)); const auto actual = f.frames.back();
        VERIFY(actual.phase == Phase::Update && actual.sequence == last.sequence+1 && actual.logicalSeconds > last.logicalSeconds);
        VERIFY(std::abs(actual.contacts[1].xMm-12) < 1e-10 && actual.contacts[0].xMm == -actual.contacts[1].xMm && actual.contacts[0].id == 1 && actual.contacts[1].id == 2);
        VERIFY(f.trace.back().stationary && e.ContactActive() && e.Pressed()); last = actual;
    }
    ++wheel.sequence; wheel.firstQpc = wheel.lastQpc = ++f.now; VERIFY(e.Process(wheel));
    f.now += 10000; VERIFY(e.Tick(f.now));
    VERIFY(f.frames.back().contacts[1].xMm > 12 && f.frames.back().contacts[1].xMm < 14.4 && !f.trace.back().stationary);
    f.now += 70000; VERIFY(e.Tick(f.now)); const auto held = f.frames.back();
    f.foreground = false; f.now = e.Deadline(); VERIFY(e.Tick(f.now));
    VERIFY(!e.Deadline() && !e.ContactActive() && f.frames.back().phase == Phase::Cancel && f.frames.back().contacts[1].xMm == held.contacts[1].xMm);
    const auto count = f.frames.size(); f.now += 100000; VERIFY(e.Tick(f.now)); VERIFY(f.frames.size() == count);
    VERIFY(e.Close());
}
void WheelRangeDoesNotRecontactWithinBurst() {
    auto config = DefaultBindingConfig(); config.wheelMode = WheelMode::Regular; config.qpcFrequency = 1000000;
    Fake f; ProductEngine e(config,f.Api()); VERIFY(e.Initialize());
    auto input = Event(InputKind::Wheel,Button::None,1,1); input.wheelY = 12000;
    VERIFY(e.Process(input)); f.now = 100000; VERIFY(e.Tick(f.now));
    VERIFY(!e.ContactActive() && !e.Deadline() && f.frames.back().phase == Phase::Cancel);
    const auto frames = f.frames.size();
    for (int i = 0; i < 4; ++i) { f.now += 20000; ++input.session; input.wheelY = 120; input.firstQpc = input.lastQpc = f.now; VERIFY(e.Process(input)); VERIFY(f.frames.size() == frames); }
    f.now += 100000; ++input.session; input.firstQpc = input.lastQpc = f.now; VERIFY(e.Process(input));
    VERIFY(e.ContactActive() && f.frames.size() == frames+1 && f.frames.back().phase == Phase::Begin && !f.frames.back().sequence);
    VERIFY(e.Close());
}
}
int RunProductEngineTests(int& total) {
    DirectPath(); TargetAndRange(); FailuresAndSnapshots(); Queue(); WheelLifecycle(); PinchOwnsOutput(); PinchTrajectoryAndRelease(); PinchBoundaryRecovery(); PinchQueuedInputAndCancellation(); PinchReverseAndFailure(); PinchHeldAcrossPauses(); WheelRangeDoesNotRecontactWithinBurst(); total += checks; return failures;
}
