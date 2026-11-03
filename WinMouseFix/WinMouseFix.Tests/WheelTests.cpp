#include "WheelApi.h"
#include <cmath>
#include <iostream>
namespace {
using namespace wmf;
int checks{},failures{};
#define VERIFY(...) do { ++checks; if (!(__VA_ARGS__)) { ++failures; std::cerr << __FILE__ << ':' << __LINE__ << ": " << #__VA_ARGS__ << '\n'; } } while(false)
bool Near(double x,double y) { return std::abs(x-y) < 1e-8; }
struct Plan {
    WheelHandle handle{}; BindingConfig config = DefaultBindingConfig(); WheelSample sample;
    Plan(WheelMode mode = WheelMode::Regular,double scale = 15) {
        config.wheelMode = mode; config.wheelMmPerNotch = scale; config.qpcFrequency = 1000000;
        VERIFY(WmfWheelCreate(&config,&handle));
    }
    ~Plan() { VERIFY(WmfWheelDestroy(&handle) && !handle); }
    Result Feed(std::int64_t y,std::uint64_t now,std::int64_t x = 0) {
        InputEvent e; e.kind = InputKind::Wheel; e.session = e.sequence = e.device = e.target = e.revision = 1;
        e.wheelX = x; e.wheelY = y; e.firstQpc = e.lastQpc = now;
        return WmfWheelFeed(handle,&e,now,&sample);
    }
    void At(std::uint64_t now) { VERIFY(WmfWheelSample(handle,now,&sample)); }
};
void Conservation() {
    for (auto mode : {WheelMode::Regular,WheelMode::High}) for (double scale : {15.,-0.1}) {
        Plan whole(mode,scale),split(mode,scale); VERIFY(whole.Feed(120,1000,120));
        for (int i = 0; i < 120; ++i) VERIFY(split.Feed(1,1000,1));
        for (std::uint64_t t = 1000; t <= 200000; t += 1000) {
            whole.At(t); split.At(t); VERIFY(Near(whole.sample.xMm,split.sample.xMm) && Near(whole.sample.yMm,split.sample.yMm));
        }
        VERIFY(Near(whole.sample.yMm,scale) && Near(split.sample.acceptedY,scale));
        VERIFY(!whole.sample.active && !whole.sample.nextQpc && Near(split.sample.discardedY,0));
    }
}
void ReversalAndDeadlines() {
    Plan p; VERIFY(p.Feed(120,1000)); p.At(31000); const auto sent = p.sample.yMm;
    VERIFY(sent > 0 && sent < 15); VERIFY(p.Feed(-120,32000));
    VERIFY(Near(p.sample.yMm,sent)); p.At(122000);
    VERIFY(Near(p.sample.yMm,sent-15) && Near(p.sample.acceptedY,0));
    VERIFY(Near(p.sample.acceptedY,p.sample.yMm+p.sample.discardedY));
    Plan slow; VERIFY(slow.Feed(120,1000)); slow.At(10000000); VERIFY(Near(slow.sample.yMm,15) && !slow.sample.active);
    Plan burst(WheelMode::High); std::uint64_t end{};
    for (std::uint64_t t = 1000; t < 101000; t += 1000) { VERIFY(burst.Feed(1,t)); end = t+180000; VERIFY(burst.sample.nextQpc <= end); }
    burst.At(end); VERIFY(!burst.sample.active && Near(burst.sample.yMm,12.5));
    const auto accepted = burst.sample.acceptedY;
    VERIFY(!burst.Feed(INT64_MAX,end+1)); burst.At(end+2); VERIFY(Near(burst.sample.acceptedY,accepted));
    VERIFY(!WmfWheelSample(burst.handle,1,&burst.sample));
    BindingConfig off = DefaultBindingConfig(); WheelHandle h{}; VERIFY(WmfWheelCreate(&off,&h).code == Status::InvalidState && !h);
}
}
int RunWheelTests(int& total) { Conservation(); ReversalAndDeadlines(); total += checks; return failures; }
