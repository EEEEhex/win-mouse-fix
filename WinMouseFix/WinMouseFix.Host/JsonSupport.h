#pragma once
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Data.Json.h>
#include <cstdint>
#include <string_view>

namespace wmf::host {
void EnsureJsonApartment();
winrt::Windows::Data::Json::JsonObject ParseJsonObject(std::string_view);
std::uint64_t JsonUnsigned(const winrt::Windows::Data::Json::JsonObject&,const wchar_t*,std::uint64_t maximum = 9007199254740991ull);
}
