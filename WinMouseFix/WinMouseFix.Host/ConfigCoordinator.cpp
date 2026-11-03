#include "ConfigCoordinator.h"
#include <stdexcept>

namespace wmf::host {
namespace {
ConfigError Canceled() { return {ConfigProblem::Storage,"config.json","Configuration service is stopping",0,0,{Status::InvalidState,ERROR_OPERATION_ABORTED}}; }
class RuntimePublication final : public PreparedPublication {
public:
    RuntimePublication(ProductRuntime& runtime,const ProductConfig& config,std::uint64_t epoch,bool enableIntent)
        : runtime_(runtime),enabled_(config.enabled),epoch_(epoch),enableIntent_(enableIntent) {
        if (!runtime_.Prepare(config.input,prepared_)) throw std::runtime_error("Cannot prepare runtime configuration snapshot");
    }
    void Publish() noexcept override {
        const auto result = runtime_.PublishPrepared(std::move(prepared_));
        if (!result) { runtime_.ReportFailure(result); return; }
        if (enableIntent_) runtime_.ApplySavedEnabled(enabled_,epoch_);
    }
private:
    ProductRuntime& runtime_;
    ProductRuntime::PreparedConfig prepared_;
    bool enabled_{};
    std::uint64_t epoch_{};
    bool enableIntent_{};
};
}
ConfigCoordinator::ConfigCoordinator(ConfigStore& store,ProductRuntime& runtime) : store_(store),runtime_(runtime),thread_(&ConfigCoordinator::Work,this) {}
ConfigCoordinator::~ConfigCoordinator() { Stop(); if (thread_.joinable()) thread_.join(); }
std::future<ConfigError> ConfigCoordinator::Enqueue(Job job) {
    auto result = job.response.get_future();
    {
        std::lock_guard lock(mutex_);
        if (stopping_) { job.response.set_value(Canceled()); return result; }
        if (jobs_.size() >= 32) {
            job.response.set_value({ConfigProblem::Storage,"config.json","Too many pending configuration requests",0,0,{Status::Busy,ERROR_BUSY}}); return result;
        }
        jobs_.push_back(std::move(job));
    }
    changed_.notify_one(); return result;
}
std::future<ConfigError> ConfigCoordinator::Submit(std::uint64_t expected,ProductConfig config,bool recovery,bool forceEnable) {
    Job job; job.expected = expected; job.epoch = runtime_.ControlEpoch(); job.config = config; job.recovery = recovery; job.forceEnable = forceEnable;
    return Enqueue(std::move(job));
}
std::future<ConfigError> ConfigCoordinator::SetEnabled(bool enabled) {
    runtime_.Enable(enabled);
    Job job; job.epoch = runtime_.ControlEpoch(); job.mergeEnable = true; job.config.enabled = enabled;
    return Enqueue(std::move(job));
}
void ConfigCoordinator::Stop() noexcept {
    {
        std::lock_guard lock(mutex_); stopping_ = true;
        for (auto& job : jobs_) { try { job.response.set_value(Canceled()); } catch (...) {} }
        jobs_.clear();
    }
    changed_.notify_one();
}
void ConfigCoordinator::Work() noexcept {
    for (;;) {
        Job job;
        {
            std::unique_lock lock(mutex_); changed_.wait(lock,[&] { return stopping_ || !jobs_.empty(); });
            if (stopping_) return; job = std::move(jobs_.front()); jobs_.pop_front();
        }
        try {
            const auto previous = store_.Current();
            if (job.mergeEnable) {
                if (job.epoch != runtime_.ControlEpoch()) {
                    job.response.set_value({ConfigProblem::Conflict,"enabled","A newer runtime control superseded this request"}); continue;
                }
                const auto enabled = job.config.enabled; job.config = previous->config; job.config.enabled = enabled; job.expected = previous->config.input.revision;
            }
            const bool applyEnable = job.forceEnable || job.config.enabled != previous->config.enabled;
            auto result = store_.Commit(job.expected,job.config,job.recovery,[&](const ProductConfig& config) {
                return std::make_unique<RuntimePublication>(runtime_,config,job.epoch,applyEnable);
            });
            job.response.set_value(std::move(result));
        } catch (...) { try { job.response.set_value({ConfigProblem::Storage,"config.json","Configuration service failed",0,0,{Status::NativeFailure,ERROR_UNHANDLED_EXCEPTION}}); } catch (...) {} }
    }
}
}
