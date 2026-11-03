#include "NativeApi.h"
#include "../WinMouseFix.Platform.Win32/DeviceSession.h"
#include "../WinMouseFix.Host/NativeModules.h"
#include "../WinMouseFix.Host/DiagnosticText.h"
#include "../WinMouseFix.Host/UpdateService.h"
#include <cmath>
#include <iostream>
#include <limits>
#include <thread>
#include <vector>

int RunRuntimeTests(int& checks);
int RunContentDragTests(int& checks);
int RunCaptureTests(int& checks);
int RunContentRuntimeTests(int& checks);
int RunProductTests(int& checks);

namespace {
int checks{}, failures{};
#define CHECK(...) do { ++checks; if (!(__VA_ARGS__)) { ++failures; std::cerr << __FILE__ << ':' << __LINE__ << ": " << #__VA_ARGS__ << '\n'; } } while (false)
using namespace wmf;
using namespace wmf::platform;

struct Fake {
    int created{}, injected{}, destroyed{}, actions{};
    int failAt{}, failFrom{};
    bool failCreate{}, failAction{};
    std::uint64_t minimumGap{}, previousTimestamp{}, clockNow{};
    bool invalidTiming{};
    SYNTHETIC_DEVICE_CREATION_PARAMS params{};
    std::vector<std::vector<POINTER_TYPE_INFO>> frames;
    NativeApi Api() {
        return {this,
            [](void* p, SYNTHETIC_DEVICE_CREATION_PARAMS* cfg) -> HSYNTHETICPOINTERDEVICE {
                auto& f = *static_cast<Fake*>(p); ++f.created; f.params = *cfg;
                return f.failCreate ? nullptr : reinterpret_cast<HSYNTHETICPOINTERDEVICE>(static_cast<std::uintptr_t>(1));
            },
            [](void* p, HSYNTHETICPOINTERDEVICE, const POINTER_TYPE_INFO* frame, UINT32 n) -> BOOL {
                auto& f = *static_cast<Fake*>(p); ++f.injected; f.frames.emplace_back(frame, frame + n);
                if (f.minimumGap) {
                    const auto timestamp = frame[0].touchInfo.pointerInfo.PerformanceCount;
                    if (timestamp > f.clockNow || (f.previousTimestamp && timestamp < f.previousTimestamp + f.minimumGap)) f.invalidTiming = true;
                    for (UINT32 i = 0; i < n; ++i)
                        if (frame[i].touchInfo.pointerInfo.PerformanceCount != timestamp) f.invalidTiming = true;
                    f.previousTimestamp = timestamp;
                    if (f.invalidTiming) return FALSE;
                }
                return f.injected != f.failAt && !(f.failFrom && f.injected >= f.failFrom);
            },
            [](void* p, HSYNTHETICPOINTERDEVICE) { ++static_cast<Fake*>(p)->destroyed; },
            [](void* p, HSYNTHETICPOINTERDEVICE, TOUCHPAD_ACTION) -> BOOL {
                auto& f = *static_cast<Fake*>(p); ++f.actions; return !f.failAction;
            },
            [](void* p) -> DWORD { return static_cast<Fake*>(p)->injected <= 1 ? 123u : 456u; }};
    }
};

void ContractsAndCore() {
    CHECK(WmfCoreVersion() == AbiVersion);
    CHECK(!WmfCoreCreate(nullptr, nullptr));
    CoreHandle h{};
    TrajectoryConfig c;
    c.version++;
    CHECK(WmfCoreCreate(&c, &h).code == Status::VersionMismatch && !h);
    c = {}; c.durationSeconds = std::numeric_limits<double>::quiet_NaN();
    CHECK(!WmfCoreCreate(&c, &h) && !h);
    c = {}; c.fingers = 5;
    CHECK(!WmfCoreCreate(&c, &h));
    c = {};
    CHECK(WmfCoreCreate(&c, &h));
    CHECK(WmfCoreCreate(&c, &h).code == Status::InvalidState);
    GestureFrame frame;
    CHECK(WmfCoreNext(h, 0, &frame) && frame.phase == Phase::Begin && frame.sequence == 0);
    CHECK(WmfCoreNext(h, 1, &frame) && std::abs(frame.offsetYmm + 10) < 1e-9);
    CHECK(!WmfCoreNext(h, 0.5, &frame));
    CHECK(!WmfCoreNext(h, std::numeric_limits<double>::infinity(), &frame));
    CHECK(WmfCoreNext(h, 2, &frame) && frame.phase == Phase::End);
    CHECK(WmfCoreNext(h, 3, &frame).code == Status::InvalidState);
    CHECK(WmfCoreDestroy(&h) && !h);
    CHECK(WmfCoreDestroy(&h));
    PlatformHandle instance{};
    for (int i = 0; i < 50; ++i) {
        CHECK(WmfPlatformCreate(&instance));
        CHECK(WmfPlatformCreate(&instance).code == Status::InvalidState);
        CHECK(WmfPlatformDestroy(&instance) && !instance);
        CHECK(WmfPlatformDestroy(&instance));
    }
    for (int run = 0; run < 50; ++run) { CHECK(WmfCoreCreate(&c, &h)); CHECK(WmfCoreDestroy(&h)); }

    for (const auto profile : {Profile::Linear, Profile::PauseReverse, Profile::SmallSteps, Profile::SlowRelease, Profile::QuickRelease, Profile::Diagonal, Profile::StartThenMicro}) {
        CoreHandle a{}, b{};
        c = {}; c.profile = profile; c.extentXmm = 10;
        CHECK(WmfCoreCreate(&c, &a) && WmfCoreCreate(&c, &b));
        for (int i = 0; i <= 240; ++i) {
            GestureFrame x, y;
            CHECK(WmfCoreNext(a, i / 120.0, &x) && WmfCoreNext(b, i / 120.0, &y));
            CHECK(x.phase == y.phase && x.sequence == y.sequence && x.offsetXmm == y.offsetXmm && x.offsetYmm == y.offsetYmm);
        }
        CHECK(WmfCoreDestroy(&a) && WmfCoreDestroy(&b));
    }
    c = {}; c.interactive = 1;
    CHECK(WmfCoreCreate(&c, &h));
    CHECK(WmfCoreControl(h, Control::Reverse).code == Status::InvalidState);
    CHECK(WmfCoreNext(h, 0, &frame));
    CHECK(WmfCoreNext(h, 0.2, &frame));
    const auto heldPosition = frame.offsetYmm;
    CHECK(WmfCoreControl(h, Control::HoldMotion));
    CHECK(WmfCoreNext(h, 0.4, &frame) && frame.offsetYmm == heldPosition && frame.phase == Phase::Update);
    CHECK(WmfCoreControl(h, Control::Reverse));
    CHECK(WmfCoreNext(h, 0.6, &frame) && std::abs(frame.offsetYmm) < 1e-8);
    CHECK(WmfCoreControl(h, Control::Release));
    CHECK(WmfCoreNext(h, 0.7, &frame) && frame.phase == Phase::End);
    CHECK(!WmfCoreControl(h, Control::Advance));
    CHECK(WmfCoreDestroy(&h));
    CHECK(WmfCoreCreate(&c, &h));
    CHECK(WmfCoreNext(h, 0, &frame));
    CHECK(WmfCoreControl(h, Control::Cancel));
    CHECK(WmfCoreNext(h, 0.1, &frame) && frame.phase == Phase::Cancel);
    CHECK(!WmfCoreNext(h, 0.2, &frame));
    CHECK(WmfCoreDestroy(&h));
    c = {}; c.profile = Profile::StartThenMicro; c.durationSeconds = 8; c.extentYmm = -16;
    CHECK(WmfCoreCreate(&c,&h));
    CHECK(WmfCoreNext(h,0,&frame) && frame.phase == Phase::Begin);
    CHECK(WmfCoreNext(h,1,&frame) && frame.offsetYmm == -15 && frame.phase == Phase::Update);
    CHECK(WmfCoreNext(h,3.9,&frame) && frame.offsetYmm == -15 && frame.phase == Phase::Update);
    CHECK(WmfCoreNext(h,4,&frame) && frame.offsetYmm == -15);
    CHECK(WmfCoreNext(h,5,&frame) && frame.offsetYmm == -15.5);
    CHECK(WmfCoreNext(h,6,&frame) && frame.offsetYmm == -16 && frame.phase == Phase::Update);
    CHECK(WmfCoreNext(h,7.9,&frame) && frame.offsetYmm == -16 && frame.phase == Phase::Update);
    CHECK(WmfCoreNext(h,8,&frame) && frame.offsetYmm == -16 && frame.phase == Phase::End);
    CHECK(WmfCoreDestroy(&h));
    c.interactive = 1;
    CHECK(!WmfCoreCreate(&c,&h) && !h);
    c.interactive = 0; c.fingers = 3;
    CHECK(!WmfCoreCreate(&c,&h) && !h);
}

void MappingAndDevice() {
    DeviceConfig c;
    GestureFrame frame;
    MappedFrame mapped;
    for (const std::uint32_t fingers : {2u, 3u, 4u}) {
        frame = {}; frame.fingers = fingers;
        CHECK(MapContacts(c, frame, mapped) && mapped.count == fingers);
        CHECK(mapped.contacts[0].id == 1 && mapped.contacts[fingers - 1].id == fingers);
        frame.offsetYmm = 29.99;
        CHECK(MapContacts(c, frame, mapped));
        frame.offsetYmm = 30;
        CHECK(MapContacts(c, frame, mapped).code == Status::OutOfRange);
    }
    frame = {}; frame.offsetYmm = std::numeric_limits<double>::infinity();
    CHECK(!MapContacts(c, frame, mapped));
    frame = {}; frame.offsetXmm = 0.01;
    CHECK(MapContacts(c, frame, mapped) && mapped.contacts[0].x == 4501);
    Fake f;
    {
        DeviceSession s(f.Api());
        DeviceConfig tiny; tiny.widthHimetric = 1;
        CHECK(s.Open(tiny).code == Status::OutOfRange && f.created == 0);
        CHECK(s.Open(c));
        CHECK(f.params.pointerType == PT_TOUCHPAD && f.params.maxCount == 5);
        CHECK(f.params.options == (SDCO_PHYSICAL_SIZE | SDCO_TOUCHPAD_GESTURE_ONLY));
        CHECK(s.Open(c).code == Status::InvalidState);
        frame = {};
        CHECK(s.Submit(frame, mapped));
        CHECK(f.frames.back()[0].touchInfo.pointerInfo.pointerFlags ==
            (POINTER_FLAG_CONFIDENCE | POINTER_FLAG_INRANGE | POINTER_FLAG_INCONTACT));
        frame.sequence = 1; frame.phase = Phase::Update; frame.offsetYmm = -1;
        CHECK(s.Submit(frame, mapped));
        CHECK(f.frames.front()[0].touchInfo.pointerInfo.pointerId == f.frames.back()[0].touchInfo.pointerInfo.pointerId);
        CHECK(f.frames.back()[0].touchInfo.pointerInfo.ptHimetricLocation.y == 2900);
        frame.sequence = 2; frame.phase = Phase::End;
        CHECK(s.Submit(frame, mapped));
        CHECK(f.frames.back()[0].touchInfo.pointerInfo.pointerFlags == POINTER_FLAG_CONFIDENCE);
        CHECK(s.StopInertia(false));
        CHECK(s.Close()); CHECK(s.Close());
        CHECK(f.destroyed == 1);
    }
    CHECK(f.destroyed == 1);
    for (const unsigned fingers : {3u,4u}) {
        Fake more;
        DeviceSession s(more.Api());
        CHECK(s.Open(c));
        frame = {}; frame.fingers = fingers;
        CHECK(s.Submit(frame,mapped) && more.frames.back().size() == fingers);
        frame.phase = Phase::Update; frame.sequence = 1;
        CHECK(s.Submit(frame,mapped));
        const auto calls = more.injected;
        CHECK(s.Submit(frame,mapped).code == Status::InvalidState && more.injected == calls);
        frame.sequence = 2; frame.fingers = 2;
        CHECK(s.Submit(frame,mapped).code == Status::InvalidState && more.injected == calls);
        frame.fingers = fingers; frame.offsetYmm = 1000;
        CHECK(s.Submit(frame,mapped).code == Status::OutOfRange && more.injected == calls);
        CHECK(s.Close() && more.destroyed == 1);
        CHECK((more.frames.back()[0].touchInfo.pointerInfo.pointerFlags & POINTER_FLAG_CANCELED) != 0);
    }
    Fake optional;
    auto optionalApi = optional.Api(); optionalApi.action = nullptr;
    DeviceSession noAction(optionalApi);
    CHECK(noAction.Open(c));
    CHECK(noAction.StopInertia(false).code == Status::Unsupported);
    CHECK(noAction.Close());
    Fake missing;
    auto absentApi = missing.Api(); absentApi.create = nullptr;
    DeviceSession absent(absentApi);
    CHECK(absent.Open(c).code == Status::Unsupported && missing.created == 0);
    Fake createFailure; createFailure.failCreate = true;
    DeviceSession failingCreation(createFailure.Api());
    CHECK(failingCreation.Open(c).nativeError == 123 && createFailure.destroyed == 0);

    for (int failAt = 1; failAt <= 3; ++failAt) {
        Fake failure; failure.failAt = failAt;
        DeviceSession s(failure.Api());
        CHECK(s.Open(c));
        Result r;
        for (int i = 0; i < failAt; ++i) {
            frame = {}; frame.sequence = static_cast<std::uint64_t>(i);
            frame.phase = i == 0 ? Phase::Begin : Phase::Update;
            r = s.Submit(frame, mapped);
        }
        CHECK(r.code == Status::NativeFailure && failure.destroyed == 1);
        CHECK((failure.frames.back()[0].touchInfo.pointerInfo.pointerFlags & POINTER_FLAG_CANCELED) != 0);
        const auto calls = failure.injected;
        CHECK(s.Submit(frame, mapped).code == Status::InvalidState && failure.injected == calls);
        s.Close(); CHECK(failure.destroyed == 1);
    }
    Fake cleanupFailure; cleanupFailure.failFrom = 1;
    DeviceSession badCleanup(cleanupFailure.Api());
    CHECK(badCleanup.Open(c));
    frame = {};
    const auto r = badCleanup.Submit(frame, mapped);
    CHECK(r.nativeError == 123 && r.cleanupError == 456 && cleanupFailure.destroyed == 1);
    Fake wrongThread;
    DeviceSession owned(wrongThread.Api());
    Result other;
    std::thread t([&] { other = owned.Open(c); }); t.join();
    CHECK(other.code == Status::WrongThread && wrongThread.created == 0);
}
void BoundaryTiming() {
    Fake fake;
    fake.minimumGap = 10000; fake.clockNow = 1000000;
    FrameTiming timing{&fake,10000000,
        [](void* p) -> std::uint64_t { return static_cast<Fake*>(p)->clockNow; },
        [](void* p,std::uint64_t target) -> bool { static_cast<Fake*>(p)->clockNow = target; return true; }};
    DeviceSession session(fake.Api(),timing);
    DeviceConfig config; MappedFrame mapped;
    CHECK(session.Open(config));
    GestureFrame frame;
    CHECK(session.Submit(frame,mapped));
    // Reproduce a 0.055 ms gap before a rapid update/release pair.
    fake.clockNow += 550;
    frame.phase = Phase::Update; frame.sequence = 1; frame.offsetXmm = 1;
    CHECK(session.Submit(frame,mapped));
    frame.phase = Phase::End; frame.sequence = 2;
    CHECK(session.Submit(frame,mapped));
    frame = {};
    CHECK(session.Submit(frame,mapped));
    CHECK(session.Close()); // Cleanup must be paced too.
    CHECK(!fake.invalidTiming && fake.frames.size() == 5);
    for (std::size_t i = 1; i < fake.frames.size(); ++i)
        CHECK(fake.frames[i][0].touchInfo.pointerInfo.PerformanceCount - fake.frames[i-1][0].touchInfo.pointerInfo.PerformanceCount >= fake.minimumGap);
    CHECK((fake.frames.back()[0].touchInfo.pointerInfo.pointerFlags & POINTER_FLAG_CANCELED) != 0);
    FrameClock stalled({&fake,10000000,timing.read,[](void*,std::uint64_t) -> bool { return true; }});
    CHECK(stalled.Initialize());
    std::uint64_t timestamp{};
    CHECK(stalled.Stamp(timestamp));
    CHECK(stalled.Stamp(timestamp).nativeError == ERROR_INVALID_TIME);
}
}

int main(int argc,char** argv) {
    if (argc == 2 && std::string_view(argv[1]) == "--update-probe") {
        try { const auto page = wmf::host::ParseReleasePage(wmf::host::FetchGithubReleases(1,{})); std::cout << "System/current-user automatic proxy; releases=" << page.count << "; best-title-version=" << (page.found ? page.best.version.Text() : "none") << '\n'; return 0; }
        catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
    }
    extern int RunDiscreteTests(int&);
    extern int RunProductEngineTests(int&);
    extern int RunWheelTests(int&);
    extern int RunProductLogTests(int&);
    extern int RunProductDeliveryTests(int&);
    extern int RunUpdateTests(int&);
    extern int RunProductRuntimeTests(int&);
    extern int RunProductConfigTests(int&);
    extern int RunControlPipeTests(int&);
    if (argc == 2 && std::string_view(argv[1]) == "--config-goldens") {
        failures += RunProductConfigTests(checks);
        std::cout << checks << " configuration checks, " << failures << " failures; isolated files only.\n";
        return failures ? 1 : 0;
    }
    ContractsAndCore();
    MappingAndDevice();
    BoundaryTiming();
    CHECK(wmf::host::Json("\"\\\n\t") == "\"\\\"\\\\\\n\\t\"");
    failures += RunRuntimeTests(checks);
    failures += RunContentDragTests(checks);
    failures += RunCaptureTests(checks);
    failures += RunContentRuntimeTests(checks);
    failures += RunProductTests(checks);
    failures += RunDiscreteTests(checks);
    failures += RunProductEngineTests(checks);
    failures += RunWheelTests(checks);
    failures += RunProductLogTests(checks);
    failures += RunProductDeliveryTests(checks);
    failures += RunUpdateTests(checks);
    failures += RunProductRuntimeTests(checks);
    failures += RunProductConfigTests(checks);
    failures += RunControlPipeTests(checks);
    std::cout << checks << " checks, " << failures << " failures; no desktop injection performed.\n";
    return failures ? 1 : 0;
}
