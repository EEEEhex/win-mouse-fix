#pragma once
#include "MouseCapture.h"

namespace wmf::platform {
struct ActionNative {
    void* context{};
    UINT (*send)(void*,UINT,const INPUT*){};
    bool (*held)(void*,std::uint32_t){};
    DWORD (*error)(void*){};
};
ActionNative RealActionNative() noexcept;
Result ExecuteAction(const ActionRequest&,const ActionNative&) noexcept;
}
