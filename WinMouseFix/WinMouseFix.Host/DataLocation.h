#pragma once
#include "NativeApi.h"
#include <filesystem>
#include <functional>
#include <string_view>

namespace wmf::host {
enum class DataLocation { AppData, Program };
struct DataPaths {
    std::filesystem::path appData,programData,marker;
    const std::filesystem::path& Directory(DataLocation value) const { return value == DataLocation::Program ? programData : appData; }
};
std::string_view DataLocationName(DataLocation) noexcept;
DataLocation ReadDataLocation(const DataPaths&);
struct DataMigration {
    Result result;
    bool sourceRetained{};
    std::filesystem::path sourceBackup,targetBackup;
};
// Called only after the old Host exits, with the resident/input ownership held.
// The optional callback is for isolated failure injection, never production policy.
DataMigration MigrateData(const DataPaths&,DataLocation from,DataLocation to,const std::function<void(std::string_view)>& checkpoint = {});
}
