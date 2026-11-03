#pragma once
#include "ConfigStore.h"
#include "ProductRuntime.h"
#include <condition_variable>
#include <deque>
#include <future>

namespace wmf::host {
class ConfigCoordinator {
public:
    ConfigCoordinator(ConfigStore&,ProductRuntime&);
    ~ConfigCoordinator();
    std::future<ConfigError> Submit(std::uint64_t expected,ProductConfig,bool recovery = false,bool forceEnableIntent = false);
    std::future<ConfigError> SetEnabled(bool); // Runtime control is immediate; only the preference waits for disk.
    void Stop() noexcept;
private:
    struct Job {
        std::uint64_t expected{},epoch{};
        ProductConfig config;
        bool recovery{},forceEnable{},mergeEnable{};
        std::promise<ConfigError> response;
    };
    std::future<ConfigError> Enqueue(Job);
    void Work() noexcept;
    ConfigStore& store_;
    ProductRuntime& runtime_;
    std::mutex mutex_;
    std::condition_variable changed_;
    std::deque<Job> jobs_;
    bool stopping_{};
    std::thread thread_;
};
}
