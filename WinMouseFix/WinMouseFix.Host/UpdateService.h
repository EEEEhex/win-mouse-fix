#pragma once
#include <array>
#include <condition_variable>
#include <cstdint>
#include <compare>
#include <filesystem>
#include <functional>
#include <mutex>
#include <stop_token>
#include <string>
#include <thread>

namespace wmf::host {
struct ReleaseVersion {
    std::array<std::uint32_t,3> parts{};
    auto operator<=>(const ReleaseVersion&) const = default;
    std::string Text() const;
};
bool ParseReleaseTitle(std::string_view,ReleaseVersion&);
struct ReleaseInfo { ReleaseVersion version; std::string title,notes,url; };
struct ReleasePage { unsigned count{}; bool found{}; ReleaseInfo best; };
ReleasePage ParseReleasePage(std::string_view);
using UpdateFetch = std::function<std::string(unsigned,std::stop_token)>;
std::string FetchGithubReleases(unsigned,std::stop_token);

// Control/timer threads only enqueue work. The worker never owns input resources.
class UpdateService {
public:
    UpdateService(bool simulation,std::filesystem::path reminderFile,UpdateFetch = {});
    ~UpdateService();
    void Tick(std::uint64_t now,bool automatic,unsigned intervalHours);
    void Request(std::uint64_t now);
    void Acknowledge(std::uint64_t checkId);
    bool TakePrompt();
    std::string StatusJson() const;
private:
    void Queue(std::uint64_t now,bool automatic);
    void Run(std::stop_token);
    UpdateFetch fetch_;
    std::filesystem::path reminderFile_;
    mutable std::mutex mutex_;
    std::condition_variable_any ready_;
    bool configured_{},automatic_{},queued_{},checking_{},automaticRequest_{},pendingPrompt_{},promptTaken_{};
    unsigned hours_{6};
    std::uint64_t lastAttempt_{},checkId_{},completedId_{};
    std::string state_{"idle"},error_,checkedUtc_,lastNotified_;
    ReleaseInfo release_;
    std::jthread worker_;
};
}
