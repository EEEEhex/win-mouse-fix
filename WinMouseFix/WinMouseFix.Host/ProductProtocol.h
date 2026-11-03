#pragma once
#include <cstdint>
#include <functional>
#include <future>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>

namespace wmf::host {
inline constexpr std::uint32_t ProtocolVersion = 1;
struct ProtocolRequest {
    std::string requestId,hostRunId,command,payload{"{}"};
    std::uint64_t expectedRevision{},connection{};
    std::uint32_t clientPid{};
};
struct ProtocolReply {
    bool ok{true};
    std::string code{"ok"},message,field,payload{"{}"};
    std::uint64_t revision{},activeRevision{};
    std::uint32_t bindingId{},conflictingId{},nativeError{},cleanupError{};
};
class ProductProtocol {
public:
    using Handler = std::function<ProtocolReply(const ProtocolRequest&)>;
    ProductProtocol(std::string runId,Handler);
    std::string Handle(std::string_view,std::uint64_t connection,std::uint32_t clientPid = 0);
    const std::string& RunId() const noexcept { return runId_; }
private:
    std::string Encode(std::string_view requestId,const ProtocolReply&);
    struct Entry {
        std::string request;
        std::shared_future<std::string> response;
        std::uint64_t serial{};
        bool complete{};
    };
    std::string runId_;
    Handler handler_;
    std::mutex cacheMutex_;
    std::unordered_map<std::string,Entry> cache_;
    std::uint64_t serial_{};
};
}
