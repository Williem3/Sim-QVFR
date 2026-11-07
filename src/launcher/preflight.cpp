#include "preflight.h"

#include <cstring>
#include <filesystem>

#include <windows.h>
#include <tlhelp32.h>

#include "../games/game.h"

namespace simqvfr::launcher {
namespace {

namespace fs = std::filesystem;

constexpr wchar_t kToolkitKey[] = L"SOFTWARE\\OpenXR_Toolkit";

// A layer's place counted from ACE: every layer above the Sim-QVFR layer 0 (the loader orders those), then
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

bool launcher_elevated() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
        return false;
    alignas(TOKEN_MANDATORY_LABEL) BYTE buffer[sizeof(TOKEN_MANDATORY_LABEL) + SECURITY_MAX_SID_SIZE] = {};
    DWORD size = 0;
    bool high = false;
    if (GetTokenInformation(token, TokenIntegrityLevel, buffer, sizeof(buffer), &size)) {
        const PSID sid = reinterpret_cast<TOKEN_MANDATORY_LABEL*>(buffer)->Label.Sid;
        const DWORD level = *GetSidSubAuthority(sid, *GetSidSubAuthorityCount(sid) - 1);
        high = level > SECURITY_MANDATORY_MEDIUM_RID;  // as the OpenXR loader's IsHighIntegrityLevel
    }
    CloseHandle(token);
    return high;
}

std::wstring parent_process_name() {
    const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE)
        return {};
    std::vector<PROCESSENTRY32W> processes;
    PROCESSENTRY32W entry{sizeof(entry)};
    for (BOOL more = Process32FirstW(snapshot, &entry); more; more = Process32NextW(snapshot, &entry))
        processes.push_back(entry);
    CloseHandle(snapshot);
    const DWORD self = GetCurrentProcessId();
    for (const PROCESSENTRY32W& p : processes)
        if (p.th32ProcessID == self)
            for (const PROCESSENTRY32W& q : processes)
                if (q.th32ProcessID == p.th32ParentProcessID)
                    return q.szExeFile;
    return {};
}

BuildState build_state(const std::string& sha, bool hashing) {
    if (sha.empty())
        return hashing ? BuildState::Hashing : BuildState::Unknown;
    return games::supported_build(games::launcher_game(), sha) ? BuildState::Supported : BuildState::Unsupported;
}

std::wstring game_folder_from(const std::wstring& picked, const games::GameInfo& game) {
    if (picked.empty())
        return {};
    std::error_code ec;
    const fs::path exeFolder = game.exeFolder, exe = game.exeName;
    const auto holds_game = [&](const fs::path& folder) { return fs::is_regular_file(folder / exeFolder / exe, ec); };
    fs::path folder = fs::path(picked).lexically_normal();
    if (!folder.has_filename())  // a trailing separator: "D:\Games\" -> "D:\Games"
        folder = folder.parent_path();
    if (holds_game(folder))
        return folder.wstring();
    // The exe's own folder (ACC's AC2\Binaries\Win64): as many folders up as exeFolder is deep.
    if (fs::is_regular_file(folder / exe, ec)) {
        fs::path up = folder;
        for (const fs::path& part : exeFolder)
            if (!part.empty())
                up = up.parent_path();
        if (holds_game(up))
            return up.wstring();
    }
    // Steam's steamapps\common (or the library above it): the game's folder inside.
    for (const fs::path& inside : {folder / game.steamFolder, folder / L"steamapps" / L"common" / game.steamFolder})
        if (holds_game(inside))
            return inside.wstring();
    return {};
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
        Check& c = add("game", CheckTone::Bad, name + L" not found",
                       L"Steam doesn’t list it in any of its libraries. Choose its folder: the one Steam’s Browse local "
                       L"files opens.");
        c.fix = CheckFix::ChooseGameFolder;
        c.fixLabel = L"Choose folder…";
    } else {
        switch (in.build) {
        case BuildState::Supported:
            add("build", CheckTone::Ok, L"Game build supported",
                exe + L" is the build this version of Sim-QVFR was made for.");
            break;
        case BuildState::Hashing:
            add("build", CheckTone::Pending, L"Checking the game build…",
                L"Reading " + exe + L" to compare it with the build Sim-QVFR supports.");
            break;
        case BuildState::Unknown:
            add("build", CheckTone::Warn, L"Game build not checked",
                exe + L" could not be read: " + in.gameExe);
            break;
        case BuildState::Unsupported: {
            Check& c = add("build", method ? CheckTone::Bad : CheckTone::Warn, name + L" was updated",
                           L"Its " + exe + L" isn’t the build this version of Sim-QVFR supports, so Sim-QVFR "
                           L"stays off and the game runs as normal, without foveated rendering. Look for an Sim-QVFR "
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
    // Administrator rights: the game inherits them, and the OpenXR loader then ignores the launch's layer folder.
    if (in.elevated) {
        const bool byShell = in.parent.empty() || _wcsicmp(in.parent.c_str(), L"explorer.exe") == 0;
        Check& c = add("elevated", method ? CheckTone::Bad : CheckTone::Warn, L"Sim-QVFR is running as administrator",
                       shortName + L" would start with administrator rights too, and OpenXR then ignores Sim-QVFR: "
                       L"the game starts on the desktop instead of in VR. " +
                           (byShell ? std::wstring(L"Close the launcher and open it with a normal double-click, not "
                                                   L"Run as administrator. If it still shows this, untick Run this "
                                                   L"program as an administrator on the Compatibility tab of "
                                                   L"sim-qvfr-launcher.exe’s Properties, under Change settings for all "
                                                   L"users too.")
                                    : L"It was started by " + in.parent + L", which runs as administrator. Close the "
                                      L"launcher and open it with a double-click instead."));
        c.confirm = method;
    } else {
        add("elevated", CheckTone::Ok, L"Not running as administrator",
            shortName + L" starts with normal rights, as OpenXR needs to load Sim-QVFR.");
    }

    // Running with Sim-QVFR (this launcher's launch, or one before it) is how it should be; running without it,
    // a launch would start a second copy.
    if (in.gameRunning && in.gameHasLayer)
        add("game_running", CheckTone::Ok, name + L" is running with Sim-QVFR",
            L"Changes here reach it as you make them; resolution beyond its launch size waits for the next launch.");
    else if (in.gameRunning && in.gameStarting)
        add("game_running", CheckTone::Ok, name + L" is starting",
            L"Launched from here. Sim-QVFR loads when the game starts VR.");
    else if (in.gameRunning)
        add("game_running", CheckTone::Bad, name + L" is already running",
            L"It runs without Sim-QVFR. Close it first: the launch starts a new copy of the game with Sim-QVFR.");

    // OpenComposite, for a game that reaches OpenXR through it.
    if (game.openComposite && in.openComposite != OpenComposite::Unknown) {
        if (in.openComposite == OpenComposite::Installed) {
            add("opencomposite", CheckTone::Ok, L"OpenComposite installed",
                shortName + L" reaches your headset’s OpenXR runtime through it.");
        } else {
            Check& c = add("opencomposite", CheckTone::Bad, L"OpenComposite isn’t installed",
                           shortName + L" uses SteamVR’s OpenVR; Sim-QVFR needs it on OpenXR, through OpenComposite. "
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
                           L"It’s Cheeky Foveated DLSS’s own loader. The game can fail to start with it there, and "
                           L"Sim-QVFR loads Cheeky itself. Move it out of the game folder.");
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
                           L"The game can crash at start in this order. Move OpenXR Toolkit above Cheeky’s layer.");
            c.fix = CheckFix::OpenLayers;
            c.fixLabel = L"Open OpenXR layers";
            c.confirm = true;
        }
        if (in.cheekyFg && toolkit >= 0 && in.toolkitTurbo == 1) {
            Check& c = add("toolkit_turbo", CheckTone::Bad, L"OpenXR Toolkit’s Turbo mode is on",
                           shortName + L" crashes with Turbo mode and Cheeky’s frame generation together. Turning it off changes "
                           L"Toolkit’s setting for " + shortName + L" only.");
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
        c.fix = CheckFix::OpenFoveation;
        c.fixLabel = L"Open Foveation";
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

}  // namespace simqvfr::launcher
