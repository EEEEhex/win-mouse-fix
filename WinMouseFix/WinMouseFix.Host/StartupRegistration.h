#pragma once
#include "ProductEnvironment.h"
#include "NativeApi.h"
#include <mutex>

namespace wmf::host {
struct StartupApi {
    void* context{};
    Result (*read)(void*,std::wstring&,bool& present){};
    Result (*write)(void*,const std::wstring&){};
    Result (*remove)(void*){};
    bool simulation{};
};
StartupApi RealStartupApi() noexcept;
struct StartupStatus { bool registered{}; Result result; };
class StartupRegistration {
public:
    StartupRegistration(const ProductEnvironment&,std::filesystem::path executable,StartupApi = RealStartupApi());
    StartupStatus Refresh();
    StartupStatus Apply(bool enabled);
    StartupStatus Status() const noexcept;
    const std::wstring& Command() const noexcept { return command_; }
private:
    StartupStatus RefreshLocked();
    StartupApi api_;
    std::wstring command_;
    mutable std::mutex mutex_;
    StartupStatus status_;
};
}
