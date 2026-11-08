#pragma once
#include <Windows.h>
#include <cstdint>

namespace wmf::host {
struct FullscreenWindow {
    RECT client{},frame{},monitor{};
    LONG_PTR style{};
    bool visible{},minimized{},cloaked{},shell{},maximized{},exclusive{};
};
struct FullscreenReading { bool known{},fullscreen{}; };
bool IsFullscreenWindow(const FullscreenWindow&) noexcept;
// An explicit HWND is used by window fixtures; production samples the foreground.
FullscreenReading ReadFullscreenWindow(HWND window = nullptr) noexcept;

class FullscreenPause {
public:
    bool Update(bool enabled,FullscreenReading,std::uint64_t milliseconds) noexcept;
    bool Paused() const noexcept { return paused_; }
private:
    bool paused_{},leaving_{};
    std::uint64_t leavingSince_{};
};

// Owned by the resident message thread. Events only enqueue a coalesced wakeup.
// The existing maintenance timer also samples, including when hooks fail.
class FullscreenMonitor {
public:
    FullscreenMonitor() = default;
    ~FullscreenMonitor();
    FullscreenMonitor(const FullscreenMonitor&) = delete;
    FullscreenMonitor& operator=(const FullscreenMonitor&) = delete;
    bool Start(HWND,UINT) noexcept;
    void Stop() noexcept;
    void Acknowledge() noexcept { pending_ = false; }
private:
    static void CALLBACK Event(HWINEVENTHOOK,DWORD,HWND,LONG,LONG,DWORD,DWORD) noexcept;
    static thread_local FullscreenMonitor* current_;
    HWND window_{};
    UINT message_{};
    HWINEVENTHOOK foreground_{},location_{};
    bool pending_{};
};
}
