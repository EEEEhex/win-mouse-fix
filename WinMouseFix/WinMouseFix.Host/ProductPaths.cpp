#include "ProductPaths.h"

namespace wmf::host {
std::filesystem::path SettingsExecutable(const std::filesystem::path& directory) {
    const auto staged = directory/L"Settings/WinMouseFix.Settings.exe";
    if (std::filesystem::exists(staged)) return staged;
    const auto solution = directory.parent_path().parent_path();
    if (std::filesystem::exists(solution/L"WinMouseFix.sln"))
        return solution/L"WinMouseFix.Settings/bin/x64"/directory.filename()/L"net8.0-windows10.0.22000.0/win-x64/WinMouseFix.Settings.exe";
    return staged;
}
namespace {
std::wstring Canonical(const std::filesystem::path& path) {
    const auto file = CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
    if (file == INVALID_HANDLE_VALUE) return {};
    wchar_t name[32768]{}; const auto count = GetFinalPathNameByHandleW(file,name,static_cast<DWORD>(std::size(name)),FILE_NAME_NORMALIZED);
    CloseHandle(file); return count && count < std::size(name) ? std::wstring(name,count) : std::wstring{};
}
}
bool ProcessImageMatches(DWORD pid,const std::filesystem::path& expected) noexcept {
    try {
        const auto process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,FALSE,pid); if (!process) return false;
        wchar_t path[32768]{}; DWORD size = static_cast<DWORD>(std::size(path));
        const auto read = QueryFullProcessImageNameW(process,0,path,&size); CloseHandle(process); if (!read) return false;
        const auto actual = Canonical(path),trusted = Canonical(expected);
        return !actual.empty() && !trusted.empty() && _wcsicmp(actual.c_str(),trusted.c_str()) == 0;
    } catch (...) { return false; }
}
}
