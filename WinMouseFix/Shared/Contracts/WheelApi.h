#pragma once
#include "ProductApi.h"

namespace wmf {
// Positions and the accepted/emitted/discarded ledger are signed millimetres.
// Reversal explicitly discards only the unsubmitted remainder of that axis.
struct alignas(8) WheelSample {
    std::uint32_t size{sizeof(WheelSample)},version{AbiVersion};
    double xMm{},yMm{},acceptedX{},acceptedY{},discardedX{},discardedY{};
    std::uint64_t nextQpc{};
    std::uint32_t active{},reserved{};
};
static_assert(sizeof(WheelSample) == 72 && std::is_trivially_copyable_v<WheelSample>);
struct WheelInstance;
using WheelHandle = WheelInstance*;
}
extern "C" {
WMF_CORE_API wmf::Result WmfWheelCreate(const wmf::BindingConfig*,wmf::WheelHandle*) noexcept;
WMF_CORE_API wmf::Result WmfWheelFeed(wmf::WheelHandle,const wmf::InputEvent*,std::uint64_t now,wmf::WheelSample*) noexcept;
WMF_CORE_API wmf::Result WmfWheelSample(wmf::WheelHandle,std::uint64_t now,wmf::WheelSample*) noexcept;
WMF_CORE_API wmf::Result WmfWheelDestroy(wmf::WheelHandle*) noexcept;
}
