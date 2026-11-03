#pragma once
#include "ProductEnvironment.h"
#include "NativeApi.h"
#include <shellapi.h>

namespace wmf::host {
inline constexpr UINT ResidentTrayMessage = WM_APP+16;
struct TrayApi {
    void* context{};
    BOOL (*notify)(void*,DWORD,NOTIFYICONDATAW*){};
    HICON (*load)(void*,HINSTANCE,int size){};
    void (*destroy)(void*,HICON){};
    bool simulation{};
};
TrayApi RealTrayApi() noexcept;
class ResidentTray {
public:
    ResidentTray(const ProductEnvironment&,HWND,HINSTANCE,TrayApi = RealTrayApi());
    ~ResidentTray();
    Result Update(std::wstring_view,bool rebuild = false);
    Result NotifyStartup(bool enabled);
    Result Remove() noexcept;
private:
    TrayApi api_;
    HWND window_{};
    HINSTANCE module_{};
    HICON icon_{};
    bool added_{};
    bool startupNotificationAttempted_{};
    int size_{};
};
}
