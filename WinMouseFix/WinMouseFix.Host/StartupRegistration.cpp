#include "StartupRegistration.h"
#include <array>
#include <stdexcept>

namespace wmf::host {
namespace {
constexpr wchar_t RunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr wchar_t ValueName[] = L"WinMouseFix";
Result Read(void*,std::wstring& value,bool& present) {
    std::array<wchar_t,32768> buffer{}; DWORD bytes = static_cast<DWORD>(buffer.size()*sizeof(wchar_t));
    const auto result = RegGetValueW(HKEY_CURRENT_USER,RunKey,ValueName,RRF_RT_REG_SZ,nullptr,buffer.data(),&bytes);
    present = result != ERROR_FILE_NOT_FOUND;
    if (result == ERROR_FILE_NOT_FOUND) { value.clear(); return {}; }
    if (result != ERROR_SUCCESS) return {wmf::Status::NativeFailure,static_cast<std::uint32_t>(result)};
    if (!bytes || bytes%sizeof(wchar_t) || buffer[(bytes/sizeof(wchar_t))-1]) return {wmf::Status::InvalidArgument,ERROR_INVALID_DATA};
    value.assign(buffer.data(),bytes/sizeof(wchar_t)-1); return {};
}
Result Write(void*,const std::wstring& value) {
    HKEY key{}; auto error = RegCreateKeyExW(HKEY_CURRENT_USER,RunKey,0,nullptr,0,KEY_SET_VALUE,nullptr,&key,nullptr);
    if (error != ERROR_SUCCESS) return {wmf::Status::NativeFailure,static_cast<std::uint32_t>(error)};
    error = RegSetValueExW(key,ValueName,0,REG_SZ,reinterpret_cast<const BYTE*>(value.c_str()),static_cast<DWORD>((value.size()+1)*sizeof(wchar_t)));
    RegCloseKey(key); return error == ERROR_SUCCESS ? Result{} : Result{wmf::Status::NativeFailure,static_cast<std::uint32_t>(error)};
}
Result Remove(void*) {
    HKEY key{}; auto error = RegOpenKeyExW(HKEY_CURRENT_USER,RunKey,0,KEY_SET_VALUE,&key);
    if (error == ERROR_FILE_NOT_FOUND) return {};
    if (error != ERROR_SUCCESS) return {wmf::Status::NativeFailure,static_cast<std::uint32_t>(error)};
    error = RegDeleteValueW(key,ValueName); RegCloseKey(key);
    return error == ERROR_SUCCESS || error == ERROR_FILE_NOT_FOUND ? Result{} : Result{wmf::Status::NativeFailure,static_cast<std::uint32_t>(error)};
}
}
StartupApi RealStartupApi() noexcept { return {nullptr,Read,Write,Remove,false}; }
StartupRegistration::StartupRegistration(const ProductEnvironment& environment,std::filesystem::path executable,StartupApi api) : api_(api) {
    if (!api.read || !api.write || !api.remove) throw std::invalid_argument("Incomplete startup backend");
    if (environment.simulation && (!api.simulation || api.read == Read || api.write == Write || api.remove == Remove)) throw std::invalid_argument("Simulation cannot access the real startup registry");
    if (!environment.simulation && api.simulation) throw std::invalid_argument("Fake startup backend requires an isolated environment");
    command_ = L"\""+std::filesystem::absolute(executable).wstring()+L"\" --startup";
}
StartupStatus StartupRegistration::Status() const noexcept { std::lock_guard lock(mutex_); return status_; }
StartupStatus StartupRegistration::RefreshLocked() {
    std::wstring actual; bool present{}; status_.result = api_.read(api_.context,actual,present);
    status_.registered = status_.result && present && actual == command_; return status_;
}
StartupStatus StartupRegistration::Refresh() { std::lock_guard lock(mutex_); return RefreshLocked(); }
StartupStatus StartupRegistration::Apply(bool enabled) {
    std::lock_guard lock(mutex_);
    if (enabled && command_.size() > 260) { status_.result = {wmf::Status::OutOfRange,ERROR_FILENAME_EXCED_RANGE}; return status_; }
    const auto result = enabled ? api_.write(api_.context,command_) : api_.remove(api_.context);
    if (!result) { status_.result = result; return status_; }
    auto read = RefreshLocked();
    if (read.result && read.registered != enabled) status_.result = {wmf::Status::NativeFailure,ERROR_INVALID_DATA};
    return status_;
}
}
