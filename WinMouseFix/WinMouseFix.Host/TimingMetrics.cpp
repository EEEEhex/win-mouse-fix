#include "TimingMetrics.h"
#include "DiagnosticText.h"
#include <algorithm>
#include <cmath>
#include <sstream>
namespace wmf::host {
namespace {
double Percentile(std::vector<double> values,double fraction) {
    if (values.empty()) return 0;
    std::sort(values.begin(),values.end()); return values[static_cast<std::size_t>(std::ceil(fraction*values.size()))-1];
}
}
void TimingMetrics::Observe(const ProductTrace& t) {
    if (t.kind != ProductTraceKind::Frame || t.stationary) return;
    const auto action = t.action == Action::NativeZoom ? "pinch" :
        t.input.kind == InputKind::Wheel && t.input.button == Button::None ? "wheel-plan" :
        t.action == Action::ShellGestures ? "shell-"+std::to_string(static_cast<unsigned>(t.direction)) : "content-drag";
    const auto key = std::string(action)+"/"+std::to_string(t.input.revision);
    if (!groups_.contains(key) && groups_.size() == 64) {
        const auto oldest = std::min_element(groups_.begin(),groups_.end(),[](const auto& a,const auto& b) { return a.second.updated < b.second.updated; });
        groups_.erase(oldest); ++evicted_;
    }
    auto& s = groups_[key]; s.updated = ++serial_;
    if (t.frame.phase != Phase::Update || t.boundary) { ++s.boundaries; return; }
    if (!t.result || !frequency_ || t.input.firstQpc > t.input.lastQpc || t.processStarted < t.input.lastQpc ||
        t.submitStarted < t.processStarted || t.returned < t.submitStarted) { ++invalid_; return; }
    const auto index = static_cast<std::size_t>(s.seen++%4096);
    const auto put = [index](std::vector<double>& values,double value) { if (values.size() < 4096) values.push_back(value); else values[index] = value; };
    const double scale = 1000.0/static_cast<double>(frequency_);
    put(s.earliest,(t.returned-t.input.firstQpc)*scale); put(s.latest,(t.returned-t.input.lastQpc)*scale);
    put(s.queue,(t.processStarted-t.input.firstQpc)*scale); put(s.processing,(t.returned-t.processStarted)*scale);
}
std::string TimingMetrics::Json() const {
    std::ostringstream out; out << "{\"qpcFrequency\":" << frequency_ << ",\"evictedGroups\":" << evicted_ << ",\"invalidSamples\":" << invalid_ << ",\"groups\":[";
    bool comma{}; for (const auto& [name,s] : groups_) {
        if (comma) out << ','; comma = true;
        out << "{\"group\":" << wmf::host::Json(name) << ",\"seen\":" << s.seen << ",\"retained\":" << s.earliest.size() << ",\"boundaryFrames\":" << s.boundaries
            << ",\"earliestP50Ms\":" << Percentile(s.earliest,0.5) << ",\"earliestP95Ms\":" << Percentile(s.earliest,0.95)
            << ",\"latestP50Ms\":" << Percentile(s.latest,0.5) << ",\"latestP95Ms\":" << Percentile(s.latest,0.95)
            << ",\"queueP50Ms\":" << Percentile(s.queue,0.5) << ",\"queueP95Ms\":" << Percentile(s.queue,0.95)
            << ",\"processingP50Ms\":" << Percentile(s.processing,0.5) << ",\"processingP95Ms\":" << Percentile(s.processing,0.95) << '}';
    }
    out << "],\"timingScope\":\"host receipt to native return; ordinary updates only; stationary contact maintenance excluded; wheel-plan and pinch separate; boundary frames counted separately; rolling last 4096 per group\"}"; return out.str();
}
}
