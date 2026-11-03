#pragma once
#include <Windows.h>
#include <filesystem>
#include <string>

namespace wmf::host {
class NativeModules {
public:
    NativeModules() = default;
    ~NativeModules();
    NativeModules(const NativeModules&) = delete;
    NativeModules& operator=(const NativeModules&) = delete;
    bool Load(const std::filesystem::path& directory, std::wstring& error);
private:
    HMODULE core_{};
    HMODULE platform_{};
};
}
