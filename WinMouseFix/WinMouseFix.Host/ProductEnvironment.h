#pragma once
#include <Windows.h>
#include <cstdint>
#include <filesystem>
#include <string>

namespace wmf::host {
struct ProductEnvironment {
    std::filesystem::path dataDirectory,executableDirectory;
    std::wstring sid,scope,pipe,hostMutex,settingsMutex,inputOwnerSuffix;
    DWORD session{};
    bool simulation{};
    static ProductEnvironment Production();
    static ProductEnvironment Simulation(const std::filesystem::path& repository,std::wstring_view instanceTag = {});
};
class UserSecurity {
public:
    explicit UserSecurity(const std::wstring& sid);
    ~UserSecurity() { if (descriptor_) LocalFree(descriptor_); }
    UserSecurity(const UserSecurity&) = delete;
    SECURITY_ATTRIBUTES* Attributes() noexcept { return &attributes_; }
private:
    PSECURITY_DESCRIPTOR descriptor_{};
    SECURITY_ATTRIBUTES attributes_{sizeof(SECURITY_ATTRIBUTES),nullptr,FALSE};
};
}
