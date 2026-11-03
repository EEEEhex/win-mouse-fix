#pragma once
#include "NativeApi.h"
#include <string>
#include <string_view>

namespace wmf::host {
inline const char* StatusName(Status status) noexcept {
    switch (status) {
    case Status::Ok: return "ok";
    case Status::InvalidArgument: return "invalid-argument";
    case Status::VersionMismatch: return "version-mismatch";
    case Status::InvalidState: return "invalid-state";
    case Status::OutOfRange: return "out-of-range";
    case Status::Unsupported: return "unsupported";
    case Status::NativeFailure: return "native-failure";
    case Status::AllocationFailure: return "allocation-failure";
    case Status::WrongThread: return "wrong-thread";
    case Status::Busy: return "busy";
    }
    return "unknown";
}
inline std::string Json(std::string_view text) {
    constexpr char hex[] = "0123456789abcdef";
    std::string out{"\""};
    for (const unsigned char c : text) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (c < 0x20) { out += "\\u00"; out += hex[c >> 4]; out += hex[c & 15]; }
            else out += static_cast<char>(c);
        }
    }
    out += '"';
    return out;
}
inline std::string JsonResult(Result r) {
    return "{\"status\":" + Json(StatusName(r.code)) + ",\"nativeError\":" +
        std::to_string(r.nativeError) + ",\"cleanupError\":" + std::to_string(r.cleanupError) + "}";
}
}
