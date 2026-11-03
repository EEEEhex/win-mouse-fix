#pragma once
#include "ProductEnvironment.h"
#include "NativeApi.h"
#include <array>
#include <atomic>
#include <functional>
#include <string>
#include <thread>

namespace wmf::host {
inline constexpr std::uint32_t MaximumControlBytes = 65536;
Result ReadControlFrame(HANDLE pipe,HANDLE stop,std::string&,DWORD timeoutMs = 3000) noexcept;
Result WriteControlFrame(HANDLE pipe,HANDLE stop,std::string_view,DWORD timeoutMs = 3000) noexcept;
class ControlPipe {
public:
    using Handler = std::function<std::string(std::string_view,std::uint64_t connection,DWORD clientPid)>;
    using Disconnected = std::function<void(std::uint64_t)>;
    ControlPipe(ProductEnvironment,Handler,Disconnected = {});
    ~ControlPipe();
    Result Start();
    void Stop() noexcept;
    void Join() noexcept;
    DWORD Clients() const noexcept { return clients_; }
private:
    void Listen(HANDLE) noexcept;
    struct Slot { HANDLE pipe{INVALID_HANDLE_VALUE}; std::thread thread; };
    ProductEnvironment environment_;
    Handler handler_;
    Disconnected disconnected_;
    HANDLE stop_{};
    std::array<Slot,4> slots_;
    std::atomic<DWORD> clients_{};
    std::atomic<std::uint64_t> serial_{};
    bool started_{};
};
}
