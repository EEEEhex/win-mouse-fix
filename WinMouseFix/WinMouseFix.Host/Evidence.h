#pragma once
#include <array>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace wmf::host {
enum class Case : unsigned { PanY, PanX, Inertia, TaskView, ShowDesktop, DesktopLeft, DesktopRight };
inline constexpr std::array<const char*, 7> CaseNames{
    "pan-y", "pan-x", "inertia", "task-view", "show-desktop", "desktop-left", "desktop-right"};
enum class Finding : unsigned { NotRun, Pass, Fail, Unsupported, Inconclusive };
inline constexpr std::array<const char*, 5> FindingNames{"not-run", "pass", "fail", "unsupported", "inconclusive"};
enum class Verdict { Go, NoGo, Inconclusive };
inline const char* VerdictName(Verdict v) noexcept {
    return v == Verdict::Go ? "go" : v == Verdict::NoGo ? "no-go" : "inconclusive";
}
struct Observation {
    Case scenario{};
    std::string item;
    Finding finding{Finding::NotRun};
    unsigned experiment{};
    std::string observer, utc, evidence, notes;
    bool HasEvidence() const noexcept {
        return experiment != 0 && !observer.empty() && !utc.empty() && !evidence.empty();
    }
};
inline std::vector<Observation> BaselineObservations() {
    std::vector<Observation> result;
    for (const auto item : {"continuous", "small-motion", "hold", "reverse"}) result.push_back({Case::PanY, item});
    for (const auto item : {"continuous", "hold", "reverse"}) result.push_back({Case::PanX, item});
    for (const auto item : {"slow-release", "fast-inertia", "interrupt"}) result.push_back({Case::Inertia, item});
    for (const auto scenario : {Case::TaskView, Case::ShowDesktop, Case::DesktopLeft, Case::DesktopRight})
        for (const auto item : {"trigger", "continuous", "hold", "reverse-cancel", "release-commit"}) result.push_back({scenario, item});
    return result;
}
inline Verdict Summarize(std::span<const Observation> items, bool nativeUnsupported, bool evidenceComplete) noexcept {
    if (nativeUnsupported) return Verdict::NoGo;
    bool allPass = !items.empty();
    for (const auto& item : items) {
        if ((item.finding == Finding::Fail || item.finding == Finding::Unsupported) && item.HasEvidence()) return Verdict::NoGo;
        if (item.finding != Finding::Pass || !item.HasEvidence()) allPass = false;
    }
    return allPass && evidenceComplete ? Verdict::Go : Verdict::Inconclusive;
}
}
