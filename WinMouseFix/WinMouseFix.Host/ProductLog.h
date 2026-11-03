#pragma once
#include "ProductRuntime.h"
#include "TimingMetrics.h"
#include <deque>
#include <filesystem>
#include <map>
#include <vector>

namespace wmf::host {
class ProductLog {
public:
    ProductLog(std::filesystem::path,ProductRuntime&,std::uint64_t frequency,bool simulation);
    ~ProductLog();
    void Record(std::string event,std::string message,std::uint64_t revision = 0) noexcept;
    void DetailFor(std::uint64_t milliseconds = 60000) noexcept;
    std::uint32_t Error() const noexcept { return error_; }
    std::string Summary();
    std::filesystem::path Export(std::string_view status,std::string_view configuration);
private:
    void Work() noexcept;
    void Write(std::string_view file,std::string_view line,std::uintmax_t limit);
    void Trace(const ProductTrace&);
    std::filesystem::path directory_;
    ProductRuntime& runtime_;
    std::uint64_t frequency_{};
    bool simulation_{};
    std::atomic<bool> stop_{};
    std::atomic<std::uint32_t> error_{};
    std::atomic<std::uint64_t> detailUntil_{},lostControl_{};
    std::mutex controlMutex_,filesMutex_,metricsMutex_;
    std::deque<std::string> control_;
    TimingMetrics metrics_;
    std::thread thread_;
};
}
