#pragma once
#include "ContentRuntime.h"
#include <vector>

namespace wmf::host {
struct ContentLaunch {
    ContentOptions options;
    std::filesystem::path output,modules;
};
ContentLaunch ParseContentArguments(const std::vector<std::wstring>&);
bool TryRunContentMode(HINSTANCE,int& result);
}
