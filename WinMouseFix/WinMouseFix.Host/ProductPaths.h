#pragma once
#include <Windows.h>
#include <filesystem>

namespace wmf::host {
std::filesystem::path SettingsExecutable(const std::filesystem::path& hostDirectory);
bool ProcessImageMatches(DWORD,const std::filesystem::path&) noexcept;
}
