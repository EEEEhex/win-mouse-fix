#pragma once
#include "ProductEnvironment.h"
#include "ProductApi.h"
#include <mutex>
namespace wmf::host {
// Bump when pinch contact semantics or paired-source isolation change.
// Version/resource rebuilds do not change this contract.
inline constexpr std::uint32_t PinchValidationContract = 1;
// Ordinary wheel and Shell diagnostics remain build-specific. Pinch acceptance
// survives compatible rebuilds on the same OS patch and ABI.
class CapabilityEvidence {
public:
    CapabilityEvidence(ProductEnvironment,Capabilities);
    std::uint32_t Flags() const noexcept;
    const std::string& Identity() const noexcept { return identity_; }
    void Confirm(bool zoom,std::uint64_t expected,std::uint64_t raw,std::uint64_t blocked,std::uint64_t accepted,std::uint64_t frames,bool visualConfirmed,std::string_view notes);
    void RecordFailure(bool zoom,std::uint64_t raw,std::uint64_t blocked,std::uint64_t accepted,std::uint64_t frames,std::string_view notes,Result failure = {});
    void RecordShell(const BindingConfig&,std::string_view payload);
    std::string ShellObservations(const BindingConfig&) const;
private:
    ProductEnvironment environment_;
    std::string identity_;
    bool eligible_{};
    mutable std::mutex shellMutex_;
    std::string shellJson_;
};
}
