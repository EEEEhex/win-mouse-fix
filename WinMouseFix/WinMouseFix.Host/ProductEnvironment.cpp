#include "ProductEnvironment.h"
#include <sddl.h>
#include <shlobj.h>
#include <array>
#include <atomic>
#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace wmf::host {
namespace {
std::wstring Sid() {
    HANDLE token{}; if (!OpenProcessToken(GetCurrentProcess(),TOKEN_QUERY,&token)) throw std::runtime_error("Cannot read current user token");
    alignas(void*) std::array<std::byte,1024> buffer{}; DWORD bytes{};
    const auto read = GetTokenInformation(token,TokenUser,buffer.data(),static_cast<DWORD>(buffer.size()),&bytes); CloseHandle(token);
    if (!read) throw std::runtime_error("Cannot read current user SID");
    LPWSTR sid{}; if (!ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(buffer.data())->User.Sid,&sid)) throw std::runtime_error("Cannot format user SID");
    std::wstring result(sid); LocalFree(sid); return result;
}
std::filesystem::path UserData() {
    PWSTR path{}; const auto result = SHGetKnownFolderPath(FOLDERID_LocalAppData,KF_FLAG_DONT_VERIFY,nullptr,&path);
    if (FAILED(result)) throw std::runtime_error("Cannot locate user data directory");
    const std::filesystem::path directory = std::filesystem::path(path)/L"WinMouseFix"; CoTaskMemFree(path); return directory;
}
std::wstring DesktopHash() {
    wchar_t name[256]{}; DWORD bytes{};
    if (!GetUserObjectInformationW(GetThreadDesktop(GetCurrentThreadId()),UOI_NAME,name,sizeof(name),&bytes)) throw std::runtime_error("Cannot identify desktop");
    // FNV-1a over little-endian UTF-16, shared with the C# endpoint resolver.
    std::uint64_t hash = 14695981039346656037ull;
    for (const auto unit : std::wstring_view(name)) for (const auto byte : {unit&255,(unit>>8)&255}) { hash ^= byte; hash *= 1099511628211ull; }
    std::wostringstream text; text << std::hex << std::setw(16) << std::setfill(L'0') << hash; return text.str();
}
void Names(ProductEnvironment& e) {
    e.pipe = L"\\\\.\\pipe\\"+e.scope+L".Control";
    e.hostMutex = L"Local\\"+e.scope+L".Host"; e.settingsMutex = L"Local\\"+e.scope+L".Settings";
}
bool Within(const std::filesystem::path& child,const std::filesystem::path& parent) {
    const auto c = std::filesystem::weakly_canonical(child).wstring(),p = std::filesystem::weakly_canonical(parent).wstring();
    if (c.size() < p.size() || _wcsnicmp(c.c_str(),p.c_str(),p.size())) return false;
    return c.size() == p.size() || c[p.size()] == L'\\' || c[p.size()] == L'/';
}
}
ProductEnvironment ProductEnvironment::Production() {
    ProductEnvironment e; e.sid = Sid(); e.dataDirectory = UserData();
    if (!ProcessIdToSessionId(GetCurrentProcessId(),&e.session)) throw std::runtime_error("Cannot identify current Windows session");
    wchar_t executable[32768]{};
    const auto count = GetModuleFileNameW(nullptr,executable,static_cast<DWORD>(std::size(executable)));
    if (!count || count >= std::size(executable)) throw std::runtime_error("Cannot locate executable directory");
    e.executableDirectory = std::filesystem::path(executable).parent_path();
    e.scope = L"WinMouseFix.v1."+e.sid+L"."+std::to_wstring(e.session)+L"."+DesktopHash(); Names(e); return e;
}
ProductEnvironment ProductEnvironment::Simulation(const std::filesystem::path& repository,std::wstring_view instanceTag) {
    auto e = Production(); static std::atomic<std::uint64_t> serial{}; LARGE_INTEGER time{}; QueryPerformanceCounter(&time);
    const auto unique = std::to_wstring(GetCurrentProcessId())+L"."+std::to_wstring(time.QuadPart)+L"."+std::to_wstring(++serial);
    const auto directory = std::filesystem::absolute(repository)/L"artifacts/product-validation/isolated"/unique;
    if (Within(directory,e.dataDirectory)) throw std::runtime_error("Simulation directory overlaps production user data");
    e.dataDirectory = std::filesystem::weakly_canonical(directory); e.simulation = true;
    if (instanceTag.size() > 64 || instanceTag.find_first_not_of(L"abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-") != std::wstring_view::npos)
        throw std::invalid_argument("Invalid shared simulation tag");
    const auto identity = instanceTag.empty() ? unique : std::wstring(instanceTag);
    e.scope += L".simulation."+identity; e.inputOwnerSuffix = L".simulation."+identity; Names(e); return e;
}
UserSecurity::UserSecurity(const std::wstring& sid) {
    const auto sddl = L"D:P(A;;GA;;;SY)(A;;GA;;;"+sid+L")";
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(),SDDL_REVISION_1,&descriptor_,nullptr)) throw std::runtime_error("Cannot construct current-user access control");
    attributes_.lpSecurityDescriptor = descriptor_;
}
}
