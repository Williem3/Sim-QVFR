// Shared by the launcher's tests (tests/launcher_*_test.cpp): the headers they check, a scratch folder of the test's
// own (`folder`, made by each test's main under its own name), fake implicit layer registrations in it (`layer`), and
// small readers of plans and ini files. Everything in an anonymous namespace, as test_util.h's: each test its own copy.
#pragma once

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cwctype>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <string>

#include <windows.h>

#include "games/ace/game.h"
#include "games/acc/game.h"
#include "games/lmu/game.h"
#include "games/scssoftware/ats/game.h"
#include "games/scssoftware/ets2/game.h"
#include "games/xplane/game.h"
#include "input/hotkeys.h"
#include "launcher/launch_plan.h"
#include "launcher/launch_watch.h"
#include "launcher/explicit_layers.h"
#include "launcher/game_tabs.h"
#include "launcher/install.h"
#include "launcher/layer_order.h"
#include "launcher/leftovers.h"
#include "launcher/log_retention.h"
#include "launcher/preflight.h"
#include "launcher/report.h"
#include "launcher/session_history.h"
#include "launcher/settings.h"
#include "launcher/signature_check.h"
#include "launcher/update_check.h"
#include "profile/profiles.h"
#include "sha256_file.h"
#include "test_util.h"

using namespace simqvfr;
using namespace simqvfr::launcher;
namespace fs = std::filesystem;

namespace {

// The test's scratch folder: each test's main sets it (a name of its own) and makes it.
fs::path folder;

// A layer manifest (and, unless `missing`, its library) in its own subfolder; returns the manifest.
inline std::wstring layer(const char* sub, const char* name, const char* disable, const char* enable = nullptr,
                          bool missing = false) {
    const fs::path dir = folder / sub;
    fs::create_directories(dir);
    std::string text = std::string("{\n  \"file_format_version\": \"1.0.0\",\n  \"api_layer\": {\n    \"name\": \"") + name +
                       "\",\n    \"library_path\": \".\\\\" + name + ".dll\",\n    \"api_version\": \"1.0\",\n    \"implementation_version\": \"1\"";
    if (disable)
        text += std::string(",\n    \"disable_environment\": \"") + disable + "\"";
    if (enable)
        text += std::string(",\n    \"enable_environment\": \"") + enable + "\"";
    text += "\n  }\n}\n";
    write_file((dir / (std::string(name) + ".json")).wstring(), text);
    if (!missing)
        write_file((dir / (std::string(name) + ".dll")).wstring(), "MZ");
    return (dir / (std::string(name) + ".json")).wstring();
}

inline std::vector<std::string> names(const LayerPlan& plan) {
    std::vector<std::string> out;
    for (const MovedLayer& m : plan.below)
        out.push_back(m.name);
    return out;
}

inline std::wstring value_of(const std::vector<std::pair<std::wstring, std::wstring>>& vars, const std::wstring& name) {
    for (const auto& [n, v] : vars)
        if (n == name)
            return v;
    return L"<unset>";
}

inline int ini_int(const std::wstring& ini, const wchar_t* section, const wchar_t* key) {
    return static_cast<int>(GetPrivateProfileIntW(section, key, -1, ini.c_str()));
}

}  // namespace
