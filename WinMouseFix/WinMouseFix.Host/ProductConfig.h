#pragma once
#include "ProductApi.h"
#include <string>
#include <string_view>

namespace wmf::host {
inline constexpr std::uint32_t ConfigSchemaVersion = 1;
inline constexpr std::size_t MaximumConfigBytes = 65536;
struct ProductConfig {
    BindingConfig input{DefaultBindingConfig()};
    bool enabled{},startup{};
    bool startupNotification{true};
    bool automaticUpdates{};
    std::uint32_t updateIntervalHours{6};
};
enum class ConfigProblem { None, Format, NewerSchema, Validation, Conflict, Storage, RecoveryRequired };
struct ConfigError {
    ConfigProblem problem{ConfigProblem::None};
    std::string field,message;
    std::uint32_t bindingId{},conflictingId{};
    Result result;
    explicit operator bool() const noexcept { return problem == ConfigProblem::None; }
};
ProductConfig DefaultProductConfig(bool systemTiming = true) noexcept;
ConfigError ValidateConfig(const ProductConfig&);
ConfigError ParseConfig(std::string_view,ProductConfig&) noexcept;
std::string SerializeConfig(const ProductConfig&);
std::string_view ButtonName(Button) noexcept;
std::string_view TriggerName(Trigger) noexcept;
std::string_view ActionName(Action) noexcept;
}
