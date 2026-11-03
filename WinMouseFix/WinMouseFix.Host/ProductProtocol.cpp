#include "ProductProtocol.h"
#include "JsonSupport.h"
#include <algorithm>
#include <chrono>
#include <stdexcept>

namespace wmf::host {
namespace {
using namespace winrt::Windows::Data::Json;
bool Identifier(std::string_view text) {
    return !text.empty() && text.size() <= 64 && std::all_of(text.begin(),text.end(),[](unsigned char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '-' || c == '_' || c == '.';
    });
}
void Text(JsonObject& object,const wchar_t* key,std::string_view text) { object.SetNamedValue(key,JsonValue::CreateStringValue(winrt::to_hstring(text))); }
void Number(JsonObject& object,const wchar_t* key,std::uint64_t value) { object.SetNamedValue(key,JsonValue::CreateNumberValue(static_cast<double>(value))); }
ProtocolReply Error(const char* code,const char* message) { ProtocolReply r; r.ok = false; r.code = code; r.message = message; return r; }
}
ProductProtocol::ProductProtocol(std::string runId,Handler handler) : runId_(std::move(runId)),handler_(std::move(handler)) {
    if (runId_.empty() || !handler_) throw std::invalid_argument("Protocol requires a run ID and handler");
}
std::string ProductProtocol::Encode(std::string_view requestId,const ProtocolReply& reply) {
    EnsureJsonApartment(); JsonObject root; Number(root,L"protocolVersion",ProtocolVersion);
    Text(root,L"requestId",requestId); Text(root,L"hostRunId",runId_); root.SetNamedValue(L"ok",JsonValue::CreateBooleanValue(reply.ok));
    Text(root,L"code",reply.code); Text(root,L"message",reply.message); Text(root,L"field",reply.field);
    Number(root,L"revision",reply.revision); Number(root,L"activeRevision",reply.activeRevision);
    Number(root,L"bindingId",reply.bindingId); Number(root,L"conflictingId",reply.conflictingId);
    Number(root,L"nativeError",reply.nativeError); Number(root,L"cleanupError",reply.cleanupError);
    root.SetNamedValue(L"payload",ParseJsonObject(reply.payload));
    const auto json = winrt::to_string(root.Stringify());
    if (json.size() > 65536) throw std::invalid_argument("Protocol response exceeds limit"); return json;
}
std::string ProductProtocol::Handle(std::string_view json,std::uint64_t connection,std::uint32_t clientPid) {
    ProtocolRequest request; request.connection = connection; request.clientPid = clientPid;
    try {
        const auto root = ParseJsonObject(json);
        if (root.HasKey(L"requestId")) request.requestId = winrt::to_string(root.GetNamedString(L"requestId"));
        if (!Identifier(request.requestId)) return Encode("",Error("bad-request","Invalid request ID"));
        if (JsonUnsigned(root,L"protocolVersion",UINT32_MAX) != ProtocolVersion)
            return Encode(request.requestId,Error("protocol-mismatch","Unsupported protocol version"));
        if (root.Size() != 6 || !root.HasKey(L"command") || !root.HasKey(L"hostRunId") || !root.HasKey(L"expectedRevision") || !root.HasKey(L"payload"))
            return Encode(request.requestId,Error("bad-request","Missing or unknown request fields"));
        request.command = winrt::to_string(root.GetNamedString(L"command"));
        if (!Identifier(request.command)) return Encode(request.requestId,Error("bad-request","Invalid command"));
        request.hostRunId = winrt::to_string(root.GetNamedString(L"hostRunId"));
        if (request.hostRunId.size() > 128 || (request.command == "hello" ? !request.hostRunId.empty() && request.hostRunId != runId_ : request.hostRunId != runId_))
            return Encode(request.requestId,Error("host-restarted","Host run changed; query current configuration before retrying"));
        request.expectedRevision = JsonUnsigned(root,L"expectedRevision");
        request.payload = winrt::to_string(root.GetNamedObject(L"payload").Stringify());
    } catch (...) { return Encode(Identifier(request.requestId) ? request.requestId : "",Error("bad-request","Invalid JSON, encoding, depth or field type")); }
    std::shared_future<std::string> pending;
    std::shared_ptr<std::promise<std::string>> promise;
    {
        std::lock_guard lock(cacheMutex_);
        const auto found = cache_.find(request.requestId);
        if (found != cache_.end()) {
            if (found->second.request != json) return Encode(request.requestId,Error("request-id-reused","A request ID cannot identify different operations"));
            pending = found->second.response;
        } else {
            if (cache_.size() >= 128) {
                auto oldest = cache_.end();
                for (auto entry = cache_.begin(); entry != cache_.end(); ++entry)
                    if (entry->second.complete && (oldest == cache_.end() || entry->second.serial < oldest->second.serial)) oldest = entry;
                if (oldest == cache_.end()) return Encode(request.requestId,Error("busy","Too many pending requests"));
                cache_.erase(oldest);
            }
            promise = std::make_shared<std::promise<std::string>>(); pending = promise->get_future().share();
            cache_.emplace(request.requestId,Entry{std::string(json),pending,++serial_,false});
        }
    }
    if (!promise) {
        if (pending.wait_for(std::chrono::seconds(3)) != std::future_status::ready)
            return Encode(request.requestId,Error("pending","Operation still pending; retry the same request ID or query current revision"));
        return pending.get();
    }
    std::string response;
    try { response = Encode(request.requestId,handler_(request)); }
    catch (...) { response = Encode(request.requestId,Error("internal-error","Request failed; input processing is independent")); }
    promise->set_value(response);
    { std::lock_guard lock(cacheMutex_); const auto found = cache_.find(request.requestId); if (found != cache_.end()) found->second.complete = true; }
    return response;
}
}
