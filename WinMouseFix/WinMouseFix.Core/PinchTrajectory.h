#pragma once
#include "ProductApi.h"
#include <algorithm>
#include <cmath>

namespace wmf {
// Value state owned by a binding instance: copying a Feed transaction also
// copies the complete planner. radius is the last frame handed to the Host.
struct PinchTrajectory {
    double radius{},origin{},goal{},velocity{},initialVelocity{},minimum{},logGain{};
    static constexpr double Maximum = 200;
    std::uint64_t now{},start{},end{},frequency{},duration{},step{},hold{};

    void Reset(const BindingConfig& config,std::uint64_t at) noexcept {
        *this = {};
        radius = origin = goal = config.initialPinchMm;
        minimum = (std::min)(2.0,radius);
        logGain = std::copysign(std::log1p(std::abs(config.zoomMmPerNotch)/radius),config.zoomMmPerNotch);
        frequency = config.qpcFrequency; now = start = end = at;
        duration = (std::max)(1ull,frequency*60/1000);
        step = (std::max)(1ull,frequency/240); hold = (std::max)(1ull,frequency/60);
    }
    bool Feed(std::int64_t units,std::uint64_t at) noexcept {
        at = (std::max)(at,now); // A physical packet may wait behind a newer Tick.
        if (at > UINT64_MAX-duration) return false;
        const double delta = static_cast<double>(units)*logGain/120;
        const bool reverse = (goal-radius)*delta < 0;
        // Never advance an unsubmitted trajectory merely because input arrived.
        // Reversal drops the old goal; outward boundary input adds no debt.
        const double base = reverse ? radius : goal;
        const double requested = std::log(base)+delta;
        const double target = requested <= std::log(minimum) ? minimum :
            requested >= std::log(Maximum) ? Maximum : std::clamp(base*std::exp(delta),minimum,Maximum);
        now = at;
        if (!reverse && target == goal) return true; // Do not prolong a saturated segment.
        goal = target; origin = radius; start = at; end = at+duration;
        const double seconds = static_cast<double>(duration)/static_cast<double>(frequency);
        const double distance = goal-origin;
        initialVelocity = reverse || !distance ? 0 : std::copysign((std::min)(std::abs(velocity),3*std::abs(distance)/seconds),distance);
        if (!distance) { end = at; velocity = 0; }
        return true;
    }
    void Sample(std::uint64_t at) noexcept {
        now = at;
        if (at >= end || end <= start) { radius = goal; velocity = 0; return; }
        const double u = static_cast<double>(at-start)/static_cast<double>(end-start);
        const double seconds = static_cast<double>(end-start)/static_cast<double>(frequency);
        const double distance = goal-origin;
        radius = std::clamp(origin+distance*(3*u*u-2*u*u*u)+initialVelocity*seconds*(u*u*u-2*u*u+u),
            (std::min)(origin,goal),(std::max)(origin,goal));
        velocity = distance*(6*u-6*u*u)/seconds+initialVelocity*(3*u*u-4*u+1);
    }
    std::uint64_t Deadline() const noexcept {
        const auto interval = now < end ? step : hold;
        const auto next = now > UINT64_MAX-interval ? UINT64_MAX : now+interval;
        return now < end ? (std::min)(end,next) : next;
    }
};
}
