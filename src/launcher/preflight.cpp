#include "preflight.h"

#include <cstring>
#include <filesystem>

#include <windows.h>

#include "../games/game.h"

namespace aceqv::launcher {
namespace {

namespace fs = std::filesystem;

constexpr wchar_t kToolkitKey[] = L"SOFTWARE\\OpenXR_Toolkit";

// A layer's place counted from ACE: every layer above the ACE layer 0 (the loader orders those), then
// the layers below it in order. -1: not loaded at this launch.
int rank(const LayerPlan& plan, const char* name) {
    for (const std::string& above : plan.above)
        if (_stricmp(above.c_str(), name) == 0)
            return 0;
    for (size_t i = 0; i < plan.below.size(); ++i)
        if (_stricmp(plan.below[i].name.c_str(), name) == 0)
            return static_cast<int>(i) + 1;
    return -1;
}

bool same_file(const std::wstring& a, const std::wstring& b) {
    return _wcsicmp(fs::path(a).lexically_normal().make_preferred().c_str(),
                    fs::path(b).lexically_normal().make_preferred().c_str()) == 0;
}

// The OpenXR_Toolkit subkey (under kToolkitKey) whose "module" is `exe`, or empty.
std::wstring toolkit_app_key(const std::wstring& exe) {
    HKEY root = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kToolkitKey, 0, KEY_READ, &root) != ERROR_SUCCESS)
        return {};
    std::wstring found;
    for (DWORD i = 0; found.empty(); ++i) {
        wchar_t name[256];
        DWORD length = static_cast<DWORD>(std::size(name));
        if (RegEnumKeyExW(root, i, name, &length, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS)
            break;
        wchar_t module[MAX_PATH] = {};
        DWORD size = sizeof(module) - sizeof(wchar_t);
        if (RegGetValueW(root, name, L"module", RRF_RT_REG_SZ, nullptr, module, &size) == ERROR_SUCCESS &&
            same_file(module, exe))
            found = std::wstring(kToolkitKey) + L"\\" + name;
    }
    RegCloseKey(root);
    return found;
}

}  // namespace

BuildState build_state(const std::string& sha, bool hashing) {
    if (sha.empty())
        return hashing ? BuildState::Hashing : BuildState::Unknown;
    return games::supported_build(games::launcher_game(), sha) ? BuildState::Supported : BuildState::Unsupported;
}

std::vector<Check> preflight_checks(const PreflightInput& in) {
    std::vector<Check> out;
    const auto add = [&](const char* id, CheckTone tone, std::wstring title, std::wstring detail) -> Check& {
        Check c;
        c.id = id;
        c.tone = tone;
        c.title = std::move(title);
        c.detail = std::move(detail);
        return out.emplace_back(std::move(c));
    };
    const bool method = in.mode != LaunchMode::Off;
    const games::GameInfo& game = games::launcher_game();
    const std::wstring name = game.name, exe = game.exeName, shortName = game.shortName;

    // The game and its build.
    if (in.gameExe.empty()) {
        add("game", CheckTone::Bad, name + L" not found",
            L"Steam doesn’t list it in any of its libraries. Set game_dir_" +
                std::wstring(game.id, game.id + std::strlen(game.id)) + L" in the [launcher] section of ace-quadview.ini "
            L"to its folder (the one Steam’s Browse local files opens).");
    } else {
        switch (in.build) {
        case BuildState::Supported:
            add("build", CheckTone::Ok, L"Game build supported",
                exe + L" is the build this version of ace-quadview was made for.");
            break;
        case BuildState::Hashing:
            add("build", CheckTone::Pending, L"Checking the game build…",
                L"Reading " + exe + L" to compare it with the build ace-quadview supports.");
            break;
        case BuildState::Unknown:
            add("build", CheckTone::Warn, L"Game build not checked",
                exe + L" could not be read: " + in.gameExe);
            break;
        case BuildState::Unsupported: {
            Check& c = add("build", method ? CheckTone::Bad : CheckTone::Warn, name + L" was updated",
                           L"Its " + exe + L" isn’t the build this version of ace-quadview supports, so ace-quadview "
                           L"stays off and the game runs as normal, without foveated rendering. Look for an ace-quadview "
                           L"release for the new build.");
            c.fix = CheckFix::OpenReleases;
            c.fixLabel = L"Check for an update";
            c.confirm = method;
            break;
        }
        }
    }

    // Steam and the game: the launch refuses without the one or with the other.
    if (in.steamRunning) {
        add("steam", CheckTone::Ok, L"Steam is running", shortName + L" needs it to start.");
    } else {
        Check& c = add("steam", CheckTone::Bad, L"Steam isn’t running", shortName + L" needs Steam. Start it, then launch.");
        c.fix = CheckFix::StartSteam;
        c.fixLabel = L"Start Steam";
    }
    if (in.gameRunning)
        add("ace_running", CheckTone::Bad, name + L" is already running",
            L"Close it first: the launch starts a new copy of the game with ace-quadview.");

    // OpenComposite, for a game that reaches OpenXR through it.
    if (game.openComposite && in.openComposite != OpenComposite::Unknown) {
        if (in.openComposite == OpenComposite::Installed) {
            add("opencomposite", CheckTone::Ok, L"OpenComposite installed",
                shortName + L" reaches your headset’s OpenXR runtime through it.");
        } else {
            Check& c = add("opencomposite", CheckTone::Bad, L"OpenComposite isn’t installed",
                           shortName + L" uses SteamVR’s OpenVR; ace-quadview needs it on OpenXR, through OpenComposite. "
                           L"Without it the game starts without foveated rendering. The guide shows the one file to copy.");
            c.fix = CheckFix::OpenGuide;
            c.fixLabel = L"Open the guide";
            c.fixTarget = kOpenCompositeGuideUrl;
            c.confirm = method;
        }
    }

    // Cheeky's loader in the game folder.
    if (!in.cheekyLoaders.empty()) {
        for (const std::wstring& loader : in.cheekyLoaders) {
            Check& c = add("cheeky_loader", CheckTone::Bad,
                           L"Cheeky’s " + fs::path(loader).filename().wstring() + L" is in the game folder",
                           L"It’s Cheeky Foveated DLSS’s own loader. ACE fails to start with it there, and ace-quadview loads Cheeky "
                           L"itself. Move it out of the game folder.");
            c.fix = CheckFix::ShowFile;
            c.fixLabel = L"Show file";
            c.fixTarget = loader;
            c.confirm = true;
        }
    } else if (!in.gameExe.empty()) {
        add("cheeky_loader", CheckTone::Ok, L"No Cheeky loader in the game folder",
            L"Neither dxgi.dll nor version.dll there is Cheeky Foveated DLSS’s loader.");
    }

    // Cheeky Foveated DLSS, when it is on.
    if (in.cheekyLoad) {
        if (!in.cheekyRuntimeFound) {
            Check& c = add("cheeky_runtime", CheckTone::Bad, L"Cheeky’s runtime not found",
                           L"Use Cheeky Foveated DLSS is ticked, but CheekyFoveatedDLSSRuntime.dll isn’t where the layer "
                           L"looks for it. The game runs without Cheeky.");
            c.fix = CheckFix::OpenDlss;
            c.fixLabel = L"Open Foveated DLSS";
        }
        if (in.cheekyLayer != CheekyLayer::Enabled) {
            Check& c = add("cheeky_layer", CheckTone::Bad,
                           in.cheekyLayer == CheekyLayer::Disabled ? L"Cheeky’s OpenXR layer is turned off"
                                                                   : L"Cheeky’s OpenXR layer isn’t installed",
                           L"Cheeky’s runtime needs its layer. Run CheekyOpenXRSetup.exe from Cheeky’s official release.");
            c.fix = CheckFix::OpenDlss;
            c.fixLabel = L"Open Foveated DLSS";
        }
        if (in.cheekyRuntimeFound && in.cheekyLayer == CheekyLayer::Enabled)
            add("cheeky", CheckTone::Ok, L"Cheeky Foveated DLSS ready", L"Its runtime and its OpenXR layer were found.");

        // The game's DLSS: Cheeky's own passes (its periphery pass, its presets) need the 310 series.
        if (!in.dlssVersion.empty()) {
            if (version_major(in.dlssVersion) >= kCheekyDlssMajor) {
                add("dlss_version", CheckTone::Ok, shortName + L"’s DLSS is " + in.dlssVersion,
                    L"New enough for Foveated DLSS.");
            } else {
                Check& c = add("dlss_version", CheckTone::Warn, shortName + L"’s DLSS is too old for Foveated DLSS",
                               shortName + L" has DLSS " + in.dlssVersion + L". Foveated DLSS needs DLSS 310 or newer "
                               L"(DLSS 4): with an older one Cheeky can’t make its own periphery pass, and the game’s "
                               L"DLSS runs as it is. Replace nvngx_dlss.dll with a newer one (a tool such as DLSS Swapper "
                               L"does it, or the NVIDIA app’s DLSS override); Steam’s file check puts the original back.");
                c.fix = CheckFix::ShowFile;
                c.fixLabel = L"Show the file";
                c.fixTarget = in.dlssPath;
            }
        }

        const int toolkit = rank(in.layers, kToolkitLayer), cheeky = rank(in.layers, kCheekyLayer);
        if (toolkit >= 0 && cheeky >= 0 && toolkit > cheeky) {
            Check& c = add("toolkit_order", CheckTone::Bad, L"OpenXR Toolkit is below Cheeky’s layer",
                           L"ACE crashes at start in this order. Move OpenXR Toolkit above Cheeky’s layer.");
            c.fix = CheckFix::OpenLayers;
            c.fixLabel = L"Open OpenXR layers";
            c.confirm = true;
        }
        if (in.cheekyFg && toolkit >= 0 && in.toolkitTurbo == 1) {
            Check& c = add("toolkit_turbo", CheckTone::Bad, L"OpenXR Toolkit’s Turbo mode is on",
                           L"ACE crashes with Turbo mode and Cheeky’s frame generation together. Turning it off changes "
                           L"Toolkit’s setting for ACE only.");
            c.fix = CheckFix::TurboOff;
            c.fixLabel = L"Turn off";
            c.confirm = true;
        }
    }

    // Eye tracking at the last launch.
    if (method && in.eyeTracked && in.eyeTracker == EyeTracker::Missing) {
        Check& c = add("eye_tracking", CheckTone::Warn, L"No eye tracking at the last launch",
                       L"The focus area stayed in the centre. Check that the headset’s eye tracker is on and calibrated, "
                       L"or choose Fixed foveation.");
        c.fix = CheckFix::OpenRendering;
        c.fixLabel = L"Open Rendering";
    }
    return out;
}

CheckTone worst_tone(const std::vector<Check>& checks) {
    CheckTone worst = CheckTone::Ok;
    for (const Check& c : checks)
        if (c.tone == CheckTone::Bad)
            return CheckTone::Bad;
        else if (c.tone == CheckTone::Warn)
            worst = CheckTone::Warn;
    return worst;
}

const Check* first_with(const std::vector<Check>& checks, CheckTone tone) {
    for (const Check& c : checks)
        if (c.tone == tone)
            return &c;
    return nullptr;
}

int version_major(const std::wstring& version) {
    if (version.empty() || !iswdigit(version[0]))
        return -1;
    return _wtoi(version.c_str());
}

std::wstring file_version(const std::wstring& path) {
    DWORD ignored = 0;
    const DWORD size = GetFileVersionInfoSizeW(path.c_str(), &ignored);
    if (!size)
        return {};
    std::vector<unsigned char> data(size);
    VS_FIXEDFILEINFO* info = nullptr;
    UINT length = 0;
    if (!GetFileVersionInfoW(path.c_str(), 0, size, data.data()) ||
        !VerQueryValueW(data.data(), L"\\", reinterpret_cast<void**>(&info), &length) || !info)
        return {};
    return std::to_wstring(HIWORD(info->dwFileVersionMS)) + L"." + std::to_wstring(LOWORD(info->dwFileVersionMS)) + L"." +
           std::to_wstring(HIWORD(info->dwFileVersionLS)) + L"." + std::to_wstring(LOWORD(info->dwFileVersionLS));
}

OpenComposite open_composite(const std::wstring& path) {
    std::string bytes;
    if (!read_file(path, bytes))
        return OpenComposite::Missing;
    return bytes.find("OpenComposite") != std::string::npos ? OpenComposite::Installed : OpenComposite::Valve;
}

std::vector<std::wstring> cheeky_loaders(const std::wstring& gameFolder) {
    std::vector<std::wstring> out;
    for (const wchar_t* name : {L"dxgi.dll", L"version.dll"}) {
        const std::wstring path = (fs::path(gameFolder) / name).wstring();
        std::string bytes;
        if (read_file(path, bytes) && bytes.find("Cheeky") != std::string::npos)
            out.push_back(path);
    }
    return out;
}

int toolkit_turbo(const std::wstring& exe) {
    const std::wstring key = toolkit_app_key(exe);
    DWORD value = 0, size = sizeof(value);
    if (key.empty() ||
        RegGetValueW(HKEY_CURRENT_USER, key.c_str(), L"turbo", RRF_RT_REG_DWORD, nullptr, &value, &size) != ERROR_SUCCESS)
        return -1;
    return value ? 1 : 0;
}

bool toolkit_turbo_off(const std::wstring& exe) {
    const std::wstring key = toolkit_app_key(exe);
    const DWORD off = 0;
    return !key.empty() &&
           RegSetKeyValueW(HKEY_CURRENT_USER, key.c_str(), L"turbo", REG_DWORD, &off, sizeof(off)) == ERROR_SUCCESS;
}

}  // namespace aceqv::launcher
