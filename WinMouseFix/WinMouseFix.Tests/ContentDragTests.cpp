#include "ContentDragApi.h"
#include <cmath>
#include <iostream>
#include <limits>

namespace {
using namespace wmf;
int checks{}, failures{};
#define VERIFY(...) do { ++checks; if (!(__VA_ARGS__)) { ++failures; std::cerr << __FILE__ << ':' << __LINE__ << ": " << #__VA_ARGS__ << '\n'; } } while(false)
struct Driver {
    DragHandle handle{};
    DragConfig config;
    MouseInput event;
    DragOutput output;
    explicit Driver(DragConfig c = {}) : config(c) { VERIFY(WmfDragCreate(&config,&handle)); event.session = 1; }
    ~Driver() { VERIFY(WmfDragDestroy(&handle)); }
    Result Feed(MouseInputKind kind, std::int64_t x = 0, std::int64_t y = 0) {
        event.kind = kind; event.dx = x; event.dy = y; ++event.sequence;
        event.firstQpc = event.lastQpc = event.sequence*10000;
        return WmfDragFeed(handle,&event,&output);
    }
};
void Contracts() {
    DragHandle h{}; DragConfig c;
    VERIFY(!WmfDragCreate(nullptr,&h));
    c.version++; VERIFY(WmfDragCreate(&c,&h).code == Status::VersionMismatch); c.version = AbiVersion;
    for (const double value : {0.0,-1.0,std::numeric_limits<double>::infinity(),std::numeric_limits<double>::quiet_NaN()}) {
        c.thresholdCounts = value; VERIFY(!WmfDragCreate(&c,&h));
    }
    c = {}; c.mmPerCountX = 0; VERIFY(!WmfDragCreate(&c,&h));
    c = {}; c.qpcFrequency = 0; VERIFY(!WmfDragCreate(&c,&h));
    c = {}; VERIFY(WmfDragCreate(&c,&h)); VERIFY(WmfDragCreate(&c,&h).code == Status::InvalidState);
    VERIFY(WmfDragDestroy(&h) && !h); VERIFY(WmfDragDestroy(&h));
}
void ClickAndThreshold() {
    for (const auto offset : {0,1,7}) {
        Driver d; VERIFY(d.Feed(MouseInputKind::Down));
        VERIFY(d.Feed(MouseInputKind::Move,offset,0));
        VERIFY(d.output.state == DragState::Pending && d.output.frameCount == 0);
        VERIFY(d.Feed(MouseInputKind::Up));
        VERIFY(d.output.effects == (ClickEffect | FinishedEffect) && d.output.frameCount == 0);
        VERIFY(!d.Feed(MouseInputKind::Up)); // No second click.
    }
    Driver d; VERIFY(d.Feed(MouseInputKind::Down));
    d.event.sequence = 1000000; VERIFY(d.Feed(MouseInputKind::Move)); // Long hold is not a gesture.
    VERIFY(d.output.state == DragState::Pending);
    VERIFY(d.Feed(MouseInputKind::Move,8,0));
    VERIFY(d.output.frameCount == 2 && d.output.frames[0].phase == Phase::Begin &&
        d.output.frames[0].offsetXmm == 0 && std::abs(d.output.frames[1].offsetXmm-0.8) < 1e-12);
    VERIFY(d.Feed(MouseInputKind::Move,-8,0));
    VERIFY(d.output.state == DragState::Active && d.output.frames[0].offsetXmm == 0);
    VERIFY(d.Feed(MouseInputKind::Up));
    VERIFY(d.output.effects == FinishedEffect && d.output.frames[0].phase == Phase::End);
    VERIFY(!d.Feed(MouseInputKind::Move,10));
    VERIFY(WmfDragReset(d.handle)); ++d.event.session; VERIFY(d.Feed(MouseInputKind::Down));
    VERIFY(!WmfDragReset(d.handle)); VERIFY(d.Feed(MouseInputKind::Cancel));
    VERIFY(d.output.frameCount == 0 && d.output.effects == (FinishedEffect | CanceledEffect));
    DragConfig c; c.thresholdCounts = 5;
    Driver diagonal(c); VERIFY(diagonal.Feed(MouseInputKind::Down));
    VERIFY(diagonal.Feed(MouseInputKind::Move,3,4)); VERIFY(diagonal.output.state == DragState::Active);
}
void AccumulationAndBoundaries() {
    DragConfig c; c.thresholdCounts = 1; c.mmPerCountX = 0.003; c.mmPerCountY = -0.007;
    Driver split(c), whole(c);
    VERIFY(split.Feed(MouseInputKind::Down) && whole.Feed(MouseInputKind::Down));
    for (int i = 0; i < 1000; ++i) VERIFY(split.Feed(MouseInputKind::Move,1,2));
    VERIFY(whole.Feed(MouseInputKind::Move,1000,2000));
    VERIFY(split.output.rawX == whole.output.rawX && split.output.rawY == whole.output.rawY);
    VERIFY(std::abs(split.output.frames[0].offsetXmm-3) < 1e-12);
    VERIFY(std::abs(split.output.frames[0].offsetYmm+14) < 1e-12);
    VERIFY(split.Feed(MouseInputKind::Move,-1000,-2000));
    VERIFY(split.output.frames[0].offsetXmm == 0 && split.output.frames[0].offsetYmm == 0);
    VERIFY(split.Feed(MouseInputKind::Move)); VERIFY(split.output.frameCount == 0); // Held, no invented displacement.
    VERIFY(split.Feed(MouseInputKind::Up,2,3));
    VERIFY(split.output.frameCount == 2 && split.output.frames[0].phase == Phase::Update && split.output.frames[1].phase == Phase::End);
    VERIFY(split.output.frames[0].offsetXmm == split.output.frames[1].offsetXmm);
    VERIFY(split.output.frames[0].offsetYmm == split.output.frames[1].offsetYmm);
    VERIFY(split.output.frames[1].sequence == split.output.frames[0].sequence+1);
    VERIFY(whole.Feed(MouseInputKind::Cancel));
    VERIFY(whole.output.frameCount == 1 && whole.output.frames[0].phase == Phase::Cancel && !(whole.output.effects & ClickEffect));
    Driver lastMoment(c); VERIFY(lastMoment.Feed(MouseInputKind::Down)); VERIFY(lastMoment.Feed(MouseInputKind::Up,3,4));
    VERIFY(lastMoment.output.frameCount == 3 && lastMoment.output.frames[2].phase == Phase::End);
    Driver overflow; VERIFY(overflow.Feed(MouseInputKind::Down));
    VERIFY(overflow.Feed(MouseInputKind::Move,std::numeric_limits<std::int64_t>::max()).code == Status::OutOfRange);
    VERIFY(overflow.Feed(MouseInputKind::Up)); VERIFY(overflow.output.effects & ClickEffect); // Rejected event did not mutate state.
}
void InvalidEvents() {
    Driver d; VERIFY(d.Feed(MouseInputKind::Down));
    auto event = d.event; event.kind = MouseInputKind::Move; ++event.sequence; event.session = 2;
    VERIFY(WmfDragFeed(d.handle,&event,&d.output).code == Status::InvalidState);
    event.session = 1; event.lastQpc = event.firstQpc-1;
    VERIFY(!WmfDragFeed(d.handle,&event,&d.output));
    event = d.event; event.kind = MouseInputKind::Move; event.version++;
    VERIFY(WmfDragFeed(d.handle,&event,&d.output).code == Status::VersionMismatch);
    VERIFY(d.Feed(MouseInputKind::Move,8,4));
    const auto output = d.output;
    event = d.event; event.kind = MouseInputKind::Move;
    VERIFY(!WmfDragFeed(d.handle,&event,&d.output));
    VERIFY(d.output.rawX == output.rawX && d.output.rawY == output.rawY);
    VERIFY(d.Feed(MouseInputKind::Up));
}
}
int RunContentDragTests(int& total) {
    Contracts(); ClickAndThreshold(); AccumulationAndBoundaries(); InvalidEvents();
    total += checks; return failures;
}
