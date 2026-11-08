#include "ProductConfig.h"
#include "JsonSupport.h"
#include <Windows.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <stdexcept>

namespace wmf::host {
namespace {
using namespace winrt::Windows::Data::Json;
constexpr std::array<std::string_view,4> buttons{"none","middle","back","forward"};
constexpr std::array<std::string_view,7> triggers{"click","double-click","hold","drag","wheel","wheel-up","wheel-down"};
constexpr std::array<std::string_view,14> actions{"none","back","forward","middle-click","content-drag","shell-gestures","native-zoom","shortcut",
    "volume-up","volume-down","mute","play-pause","previous-track","next-track"};
constexpr std::array<std::string_view,3> modes{"off","regular","high"};
void Keys(const JsonObject& object,std::initializer_list<const wchar_t*> keys,std::initializer_list<const wchar_t*> optional = {}) {
    auto expected = keys.size(); for (const auto key : optional) if (object.HasKey(key)) ++expected;
    if (object.Size() != expected) throw std::invalid_argument("Missing or unknown configuration field");
    for (const auto key : keys) if (!object.HasKey(key)) throw std::invalid_argument("Missing configuration field");
}
std::uint64_t Integer(double number,std::uint64_t maximum = UINT32_MAX) {
    if (!std::isfinite(number) || number < 0 || number > static_cast<double>(maximum) || std::floor(number) != number)
        throw std::invalid_argument("Expected a bounded nonnegative integer");
    return static_cast<std::uint64_t>(number);
}
template<std::size_t N> std::uint32_t Enum(const winrt::hstring& text,const std::array<std::string_view,N>& names) {
    const auto value = winrt::to_string(text);
    for (unsigned i = 0; i < names.size(); ++i) if (names[i] == value) return i;
    throw std::invalid_argument("Unknown configuration enum");
}
void Number(JsonObject& o,const wchar_t* key,double value) { o.SetNamedValue(key,JsonValue::CreateNumberValue(value)); }
void String(JsonObject& o,const wchar_t* key,std::string_view value) { o.SetNamedValue(key,JsonValue::CreateStringValue(winrt::to_hstring(value))); }
std::string ValidationPath(ValidationField field) {
    switch (field) {
    case ValidationField::Shortcut: return "bindings.keys";
    case ValidationField::Conflict: case ValidationField::Binding: return "bindings";
    default: return "parameters";
    }
}
}
std::string_view ButtonName(Button value) noexcept { const auto i = static_cast<unsigned>(value); return i < buttons.size() ? buttons[i] : "unknown"; }
std::string_view TriggerName(Trigger value) noexcept { const auto i = static_cast<unsigned>(value); return i < triggers.size() ? triggers[i] : "unknown"; }
std::string_view ActionName(Action value) noexcept { const auto i = static_cast<unsigned>(value); return i < actions.size() ? actions[i] : "unknown"; }
ProductConfig DefaultProductConfig(bool system) noexcept {
    ProductConfig config; LARGE_INTEGER frequency{}; QueryPerformanceFrequency(&frequency); config.input.qpcFrequency = frequency.QuadPart;
    if (system) {
        config.input.doubleClickMs = (std::clamp)(GetDoubleClickTime(),100u,2000u);
        config.input.doubleClickTolerancePixels = static_cast<std::uint32_t>((std::clamp)((std::max)(GetSystemMetrics(SM_CXDOUBLECLK),GetSystemMetrics(SM_CYDOUBLECLK))/2,0,100));
    }
    return config;
}
ConfigError ValidateConfig(const ProductConfig& config) {
    if (config.updateIntervalHours < 1 || config.updateIntervalHours > 168) return {ConfigProblem::Validation,"updateIntervalHours","Update interval must be 1 to 168 hours"};
    const auto& c = config.input;
    const auto range = [](double value,double low,double high) { return std::isfinite(value) && value >= low && value <= high; };
    const auto scale = [&](double value) { return range(std::abs(value),0.000001,100); };
    const auto invalid = [](const char* field) -> ConfigError { return {ConfigProblem::Validation,field,"Value outside the allowed range",0,0,{Status::InvalidArgument}}; };
    if (!range(c.thresholdCounts,0.000001,1000000)) return invalid("content.thresholdCounts");
    if (!scale(c.mmPerCountX)) return invalid("content.mmPerCountX");
    if (!scale(c.mmPerCountY)) return invalid("content.mmPerCountY");
    if (!scale(c.shellMmPerCountX)) return invalid("shell.mmPerCountX");
    if (!scale(c.shellMmPerCountY)) return invalid("shell.mmPerCountY");
    if (!range(c.directionRatio,1.01,8)) return invalid("shell.directionRatio");
    if (c.shellFingers != 3 && c.shellFingers != 4) return invalid("shell.fingers");
    if (!scale(c.wheelMmPerNotch)) return invalid("wheel.mmPerNotch");
    if (!range(c.regularMilliseconds,10,500)) return invalid("wheel.regularMilliseconds");
    if (!range(c.highMilliseconds,c.regularMilliseconds,1000)) return invalid("wheel.highMilliseconds");
    if (!scale(c.zoomMmPerNotch)) return invalid("zoom.mmPerNotch");
    if (!range(c.initialPinchMm,1,100)) return invalid("zoom.initialPinchMm");
    if (c.doubleClickMs < 100 || c.doubleClickMs > 2000) return invalid("timing.doubleClickMs");
    if (c.holdMs < 100 || c.holdMs > 5000) return invalid("timing.holdMs");
    if (c.doubleClickTolerancePixels > 100) return invalid("timing.doubleClickTolerancePixels");
    ValidationError detail; const auto result = WmfValidateBindings(&config.input,&detail);
    if (result) return {};
    ConfigError error; error.problem = ConfigProblem::Validation; error.result = result;
    error.field = ValidationPath(static_cast<ValidationField>(detail.field)); error.message = "Invalid parameter or conflicting binding";
    if (detail.bindingIndex < config.input.bindingCount) error.bindingId = config.input.bindings[detail.bindingIndex].id;
    if (detail.conflictingIndex < config.input.bindingCount) error.conflictingId = config.input.bindings[detail.conflictingIndex].id;
    return error;
}
ConfigError ParseConfig(std::string_view json,ProductConfig& output) noexcept {
    std::string field = "config";
    try {
        const auto root = ParseJsonObject(json);
        field = "schemaVersion"; const auto schema = Integer(root.GetNamedNumber(L"schemaVersion"));
        if (schema != ConfigSchemaVersion) return {ConfigProblem::NewerSchema,field,"Unsupported configuration schema",0,0,{Status::VersionMismatch}};
        Keys(root,{L"schemaVersion",L"revision",L"enabled",L"startup",L"bindings",L"content",L"shell",L"wheel",L"zoom",L"timing"},{L"startupNotification",L"automaticUpdates",L"updateIntervalHours",L"disableInFullscreen"});
        auto config = DefaultProductConfig(false); auto& c = config.input;
        field = "revision"; c.revision = Integer(root.GetNamedNumber(L"revision"),9007199254740991ull);
        field = "enabled"; config.enabled = root.GetNamedBoolean(L"enabled"); field = "startup"; config.startup = root.GetNamedBoolean(L"startup");
        field = "startupNotification"; config.startupNotification = root.HasKey(L"startupNotification") ? root.GetNamedBoolean(L"startupNotification") : true;
        field = "disableInFullscreen"; config.disableInFullscreen = root.HasKey(L"disableInFullscreen") ? root.GetNamedBoolean(L"disableInFullscreen") : false;
        field = "automaticUpdates"; config.automaticUpdates = root.HasKey(L"automaticUpdates") ? root.GetNamedBoolean(L"automaticUpdates") : false;
        field = "updateIntervalHours"; config.updateIntervalHours = root.HasKey(L"updateIntervalHours") ? static_cast<std::uint32_t>(Integer(root.GetNamedNumber(L"updateIntervalHours"),168)) : 6;
        field = "content"; const auto content = root.GetNamedObject(L"content"); Keys(content,{L"thresholdCounts",L"mmPerCountX",L"mmPerCountY"});
        c.thresholdCounts = content.GetNamedNumber(L"thresholdCounts"); c.mmPerCountX = content.GetNamedNumber(L"mmPerCountX"); c.mmPerCountY = content.GetNamedNumber(L"mmPerCountY");
        field = "shell"; const auto shell = root.GetNamedObject(L"shell"); Keys(shell,{L"fingers",L"mmPerCountX",L"mmPerCountY",L"directionRatio"});
        c.shellFingers = static_cast<std::uint32_t>(Integer(shell.GetNamedNumber(L"fingers"))); c.shellMmPerCountX = shell.GetNamedNumber(L"mmPerCountX");
        c.shellMmPerCountY = shell.GetNamedNumber(L"mmPerCountY"); c.directionRatio = shell.GetNamedNumber(L"directionRatio");
        field = "wheel"; const auto wheel = root.GetNamedObject(L"wheel"); Keys(wheel,{L"mode",L"mmPerNotch",L"regularMilliseconds",L"highMilliseconds"});
        c.wheelMode = static_cast<WheelMode>(Enum(wheel.GetNamedString(L"mode"),modes)); c.wheelMmPerNotch = wheel.GetNamedNumber(L"mmPerNotch");
        c.regularMilliseconds = wheel.GetNamedNumber(L"regularMilliseconds"); c.highMilliseconds = wheel.GetNamedNumber(L"highMilliseconds");
        field = "zoom"; const auto zoom = root.GetNamedObject(L"zoom"); Keys(zoom,{L"mmPerNotch",L"initialPinchMm"});
        c.zoomMmPerNotch = zoom.GetNamedNumber(L"mmPerNotch"); c.initialPinchMm = zoom.GetNamedNumber(L"initialPinchMm");
        field = "timing"; const auto timing = root.GetNamedObject(L"timing"); Keys(timing,{L"doubleClickMs",L"holdMs",L"doubleClickTolerancePixels"});
        c.doubleClickMs = static_cast<std::uint32_t>(Integer(timing.GetNamedNumber(L"doubleClickMs"))); c.holdMs = static_cast<std::uint32_t>(Integer(timing.GetNamedNumber(L"holdMs")));
        c.doubleClickTolerancePixels = static_cast<std::uint32_t>(Integer(timing.GetNamedNumber(L"doubleClickTolerancePixels")));
        field = "bindings"; const auto bindings = root.GetNamedArray(L"bindings");
        if (bindings.Size() > MaximumBindings) throw std::invalid_argument("Too many bindings");
        c.bindingCount = bindings.Size();
        for (std::uint32_t i = 0; i < c.bindingCount; ++i) {
            field = "bindings["+std::to_string(i)+"]"; const auto item = bindings.GetObjectAt(i);
            Keys(item,{L"id",L"button",L"trigger",L"modifiers",L"action",L"keys"}); auto& b = c.bindings[i]; b = {};
            b.id = static_cast<std::uint32_t>(Integer(item.GetNamedNumber(L"id"))); b.button = static_cast<Button>(Enum(item.GetNamedString(L"button"),buttons));
            b.trigger = static_cast<Trigger>(Enum(item.GetNamedString(L"trigger"),triggers)); b.action = static_cast<Action>(Enum(item.GetNamedString(L"action"),actions));
            b.modifiers = static_cast<std::uint32_t>(Integer(item.GetNamedNumber(L"modifiers"))); const auto keys = item.GetNamedArray(L"keys");
            if (keys.Size() > MaximumShortcutKeys) throw std::invalid_argument("Too many shortcut keys");
            b.keyCount = keys.Size(); for (unsigned k = 0; k < b.keyCount; ++k) b.keys[k] = static_cast<std::uint32_t>(Integer(keys.GetNumberAt(k)));
        }
        const auto valid = ValidateConfig(config); if (!valid) return valid;
        output = config; return {};
    } catch (const winrt::hresult_error& error) {
        return {ConfigProblem::Format,field,"Invalid JSON field, type or encoding",0,0,{Status::InvalidArgument,static_cast<std::uint32_t>(error.code().value)}};
    } catch (const std::exception& error) { return {ConfigProblem::Format,field,error.what(),0,0,{Status::InvalidArgument}}; }
    catch (...) { return {ConfigProblem::Format,field,"Cannot parse configuration",0,0,{Status::AllocationFailure}}; }
}
std::string SerializeConfig(const ProductConfig& config) {
    if (!ValidateConfig(config)) throw std::invalid_argument("Cannot serialize invalid configuration");
    EnsureJsonApartment(); const auto& c = config.input; JsonObject root;
    Number(root,L"schemaVersion",ConfigSchemaVersion); Number(root,L"revision",static_cast<double>(c.revision));
    root.SetNamedValue(L"enabled",JsonValue::CreateBooleanValue(config.enabled)); root.SetNamedValue(L"startup",JsonValue::CreateBooleanValue(config.startup));
    root.SetNamedValue(L"startupNotification",JsonValue::CreateBooleanValue(config.startupNotification));
    root.SetNamedValue(L"disableInFullscreen",JsonValue::CreateBooleanValue(config.disableInFullscreen));
    root.SetNamedValue(L"automaticUpdates",JsonValue::CreateBooleanValue(config.automaticUpdates)); Number(root,L"updateIntervalHours",config.updateIntervalHours);
    JsonObject content; Number(content,L"thresholdCounts",c.thresholdCounts); Number(content,L"mmPerCountX",c.mmPerCountX); Number(content,L"mmPerCountY",c.mmPerCountY); root.SetNamedValue(L"content",content);
    JsonObject shell; Number(shell,L"fingers",c.shellFingers); Number(shell,L"mmPerCountX",c.shellMmPerCountX); Number(shell,L"mmPerCountY",c.shellMmPerCountY); Number(shell,L"directionRatio",c.directionRatio); root.SetNamedValue(L"shell",shell);
    JsonObject wheel; String(wheel,L"mode",modes[static_cast<unsigned>(c.wheelMode)]); Number(wheel,L"mmPerNotch",c.wheelMmPerNotch); Number(wheel,L"regularMilliseconds",c.regularMilliseconds); Number(wheel,L"highMilliseconds",c.highMilliseconds); root.SetNamedValue(L"wheel",wheel);
    JsonObject zoom; Number(zoom,L"mmPerNotch",c.zoomMmPerNotch); Number(zoom,L"initialPinchMm",c.initialPinchMm); root.SetNamedValue(L"zoom",zoom);
    JsonObject timing; Number(timing,L"doubleClickMs",c.doubleClickMs); Number(timing,L"holdMs",c.holdMs); Number(timing,L"doubleClickTolerancePixels",c.doubleClickTolerancePixels); root.SetNamedValue(L"timing",timing);
    JsonArray bindings;
    for (unsigned i = 0; i < c.bindingCount; ++i) {
        const auto& b = c.bindings[i]; JsonObject item; Number(item,L"id",b.id); String(item,L"button",ButtonName(b.button)); String(item,L"trigger",TriggerName(b.trigger));
        Number(item,L"modifiers",b.modifiers); String(item,L"action",ActionName(b.action)); JsonArray keys;
        for (unsigned k = 0; k < b.keyCount; ++k) keys.Append(JsonValue::CreateNumberValue(b.keys[k]));
        item.SetNamedValue(L"keys",keys); bindings.Append(item);
    }
    root.SetNamedValue(L"bindings",bindings); return winrt::to_string(root.Stringify());
}
}
