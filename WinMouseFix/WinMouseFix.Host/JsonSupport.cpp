#include "JsonSupport.h"
#include <Windows.h>
#include <roapi.h>
#include <cmath>
#include <stdexcept>

namespace wmf::host {
namespace {
struct Apartment {
    HRESULT result{RoInitialize(RO_INIT_MULTITHREADED)};
    Apartment() { if (FAILED(result) && result != RPC_E_CHANGED_MODE) winrt::throw_hresult(result); }
    ~Apartment() { if (SUCCEEDED(result)) { winrt::clear_factory_cache(); RoUninitialize(); } }
};
}
void EnsureJsonApartment() { thread_local Apartment apartment; }
winrt::Windows::Data::Json::JsonObject ParseJsonObject(std::string_view text) {
    if (text.empty() || text.size() > 65536) throw std::invalid_argument("JSON exceeds size limit");
    // Limit structure depth before invoking the system parser. This scan does
    // not interpret fields or replace JSON syntax/type validation.
    bool quoted{},escaped{}; unsigned depth{};
    for (const auto byte : text) {
        if (quoted) {
            if (escaped) escaped = false;
            else if (byte == '\\') escaped = true;
            else if (byte == '"') quoted = false;
        } else if (byte == '"') quoted = true;
        else if (byte == '{' || byte == '[') { if (++depth > 16) throw std::invalid_argument("JSON exceeds depth limit"); }
        else if (byte == '}' || byte == ']') { if (!depth) throw std::invalid_argument("Invalid JSON structure"); --depth; }
    }
    const auto count = MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,text.data(),static_cast<int>(text.size()),nullptr,0);
    if (!count) throw std::invalid_argument("Invalid UTF-8");
    std::wstring wide(count,L'\0'); MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,text.data(),static_cast<int>(text.size()),wide.data(),count);
    EnsureJsonApartment(); return winrt::Windows::Data::Json::JsonObject::Parse(wide);
}
std::uint64_t JsonUnsigned(const winrt::Windows::Data::Json::JsonObject& object,const wchar_t* key,std::uint64_t maximum) {
    const auto value = object.GetNamedNumber(key);
    if (!std::isfinite(value) || value < 0 || value > static_cast<double>(maximum) || std::floor(value) != value) throw std::invalid_argument("Invalid integer field");
    return static_cast<std::uint64_t>(value);
}
}
