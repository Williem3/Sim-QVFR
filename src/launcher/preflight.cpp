#include "preflight.h"

#include <cstring>
#include <filesystem>

#include <windows.h>
#include <tlhelp32.h>

#include "../games/game.h"
#include "../lang.h"

namespace simqvfr::launcher {
namespace {

namespace fs = std::filesystem;
using lang::tr;
using lang::trf;

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
        Check& c = add("game", CheckTone::Bad, trf(L"{game} not found", {{L"game", name}}),
                       tr(L"Steam doesn’t list it in any of its libraries. Choose its folder: the one Steam’s "
                          L"Browse local files opens."));
        c.fix = CheckFix::ChooseGameFolder;
        c.fixLabel = tr(L"Choose folder…");
    } else {
        switch (in.build) {
        case BuildState::Supported:
            add("build", CheckTone::Ok, tr(L"Game build supported"),
                trf(L"{exe} is the build this version of Sim-QVFR was made for.", {{L"exe", exe}}));
            break;
        case BuildState::Hashing:
            add("build", CheckTone::Pending, tr(L"Checking the game build…"),
                trf(L"Reading {exe} to compare it with the build Sim-QVFR supports.", {{L"exe", exe}}));
            break;
        case BuildState::Unknown:
            add("build", CheckTone::Warn, tr(L"Game build not checked"),
                trf(L"{exe} could not be read: {path}", {{L"exe", exe}, {L"path", in.gameExe}}));
            break;
        case BuildState::Unsupported: {
            if (game.anyBuild) {  // QVFR runs on it, without what needs the game's addresses
                Check& c = add("build", CheckTone::Warn, trf(L"{game} build not tested yet", {{L"game", name}}),
                               trf(L"Its {exe} is newer than this version of Sim-QVFR, or from a beta branch. "
                                   L"Foveated rendering still runs. Sim-QVFR’s DLSS and FSR stay off until a "
                                   L"Sim-QVFR release supports this build.",
                                   {{L"exe", exe}}));
                c.fix = CheckFix::OpenReleases;
                c.fixLabel = tr(L"Check for an update");
                break;
            }
            Check& c = add("build", method ? CheckTone::Bad : CheckTone::Warn,
                           trf(L"{game} was updated", {{L"game", name}}),
                           trf(L"Its {exe} isn’t the build this version of Sim-QVFR supports, so Sim-QVFR "
                               L"stays off and the game runs as normal, without foveated rendering. Look for an "
                               L"Sim-QVFR release for the new build.",
                               {{L"exe", exe}}));
            c.fix = CheckFix::OpenReleases;
            c.fixLabel = tr(L"Check for an update");
            c.confirm = method;
            break;
        }
        }
    }

    // Steam and the game: the launch refuses without the one or with the other.
    if (in.steamRunning) {
        add("steam", CheckTone::Ok, tr(L"Steam is running"), trf(L"{game} needs it to start.", {{L"game", shortName}}));
    } else {
        Check& c = add("steam", CheckTone::Bad, tr(L"Steam isn’t running"),
                       trf(L"{game} needs Steam. Start it, then launch.", {{L"game", shortName}}));
        c.fix = CheckFix::StartSteam;
        c.fixLabel = tr(L"Start Steam");
    }
    // Administrator rights: the game inherits them, and the OpenXR loader then ignores the launch's layer folder. The
    // launch registers its layers where that loader looks (explicit_layers.h), unless that is turned off.
    const bool byShell = in.parent.empty() || _wcsicmp(in.parent.c_str(), L"explorer.exe") == 0;
    if (in.elevated && in.elevatedRegistration) {
        add("elevated", CheckTone::Warn, tr(L"Sim-QVFR is running as administrator"),
            trf(L"{game} starts with administrator rights too, and OpenXR then looks for layers only in Windows’ "
                L"machine-wide list. Each launch adds Sim-QVFR’s own entries to it and takes them out when {game} "
                L"closes; nothing else in the list changes.",
                {{L"game", shortName}}) +
                L" " +
                (byShell ? std::wstring(tr(L"To launch with normal rights instead, open the launcher with a normal "
                                           L"double-click, not Run as administrator (and untick Run this program as an "
                                           L"administrator on the Compatibility tab of sim-qvfr-launcher.exe’s "
                                           L"Properties, if it is ticked)."))
                         : trf(L"To launch with normal rights instead, open the launcher with a double-click, not from "
                               L"{program}.",
                               {{L"program", in.parent}})));
    } else if (in.elevated) {
        Check& c = add("elevated", method ? CheckTone::Bad : CheckTone::Warn,
                       tr(L"Sim-QVFR is running as administrator"),
                       trf(L"{game} would start with administrator rights too, and OpenXR then ignores Sim-QVFR: "
                           L"the game starts on the desktop instead of in VR.",
                           {{L"game", shortName}}) +
                           L" " +
                           (byShell ? std::wstring(tr(L"Close the launcher and open it with a normal double-click, not "
                                                      L"Run as administrator. If it still shows this, untick Run this "
                                                      L"program as an administrator on the Compatibility tab of "
                                                      L"sim-qvfr-launcher.exe’s Properties, under Change settings for "
                                                      L"all users too."))
                                    : trf(L"It was started by {program}, which runs as administrator. Close the "
                                          L"launcher and open it with a double-click instead.",
                                          {{L"program", in.parent}})) +
                           L" " +
                           tr(L"(Registering Sim-QVFR for administrator launches is off: elevated_registration=0 in "
                              L"sim-qvfr.ini.)"));
        c.confirm = method;
    } else {
        add("elevated", CheckTone::Ok, tr(L"Not running as administrator"),
            trf(L"{game} starts with normal rights, as OpenXR needs to load Sim-QVFR.", {{L"game", shortName}}));
    }

    // Running with Sim-QVFR (this launcher's launch, or one before it) is how it should be; running without it,
    // a launch would start a second copy.
    if (in.gameRunning && in.gameHasLayer)
        add("game_running", CheckTone::Ok, trf(L"{game} is running with Sim-QVFR", {{L"game", name}}),
            tr(L"Changes here reach it as you make them; resolution beyond its launch size waits for the next "
               L"launch."));
    else if (in.gameRunning && in.gameStarting)
        add("game_running", CheckTone::Ok, trf(L"{game} is starting", {{L"game", name}}),
            tr(L"Launched from here. Sim-QVFR loads when the game starts VR."));
    else if (in.gameRunning)
        add("game_running", CheckTone::Bad, trf(L"{game} is already running", {{L"game", name}}),
            tr(L"It runs without Sim-QVFR. Close it first: the launch starts a new copy of the game with Sim-QVFR."));

    // OpenComposite, for a game that reaches OpenXR through it.
    if (game.openComposite && in.openComposite != OpenComposite::Unknown) {
        if (in.openComposite == OpenComposite::Installed) {
            add("opencomposite", CheckTone::Ok, tr(L"OpenComposite installed"),
                trf(L"{game} reaches your headset’s OpenXR runtime through it.", {{L"game", shortName}}));
        } else {
            Check& c = add("opencomposite", CheckTone::Bad, tr(L"OpenComposite isn’t installed"),
                           trf(L"{game} uses SteamVR’s OpenVR; Sim-QVFR needs it on OpenXR, through OpenComposite. "
                               L"Without it the game starts without foveated rendering. The guide shows the one file "
                               L"to copy.",
                               {{L"game", shortName}}));
            c.fix = CheckFix::OpenGuide;
            c.fixLabel = tr(L"Open the guide");
            c.fixTarget = kOpenCompositeGuideUrl;
            c.confirm = method;
        }
    }

    // Cheeky's loader in the game folder.
    if (!in.cheekyLoaders.empty()) {
        for (const std::wstring& loader : in.cheekyLoaders) {
            Check& c = add("cheeky_loader", CheckTone::Bad,
                           trf(L"Cheeky’s {file} is in the game folder",
                               {{L"file", fs::path(loader).filename().wstring()}}),
                           tr(L"It’s Cheeky Foveated DLSS’s own loader. The game can fail to start with it there, and "
                              L"Sim-QVFR loads Cheeky itself. Move it out of the game folder."));
            c.fix = CheckFix::ShowFile;
            c.fixLabel = tr(L"Show file");
            c.fixTarget = loader;
            c.confirm = true;
        }
    } else if (!in.gameExe.empty()) {
        add("cheeky_loader", CheckTone::Ok, tr(L"No Cheeky loader in the game folder"),
            tr(L"Neither dxgi.dll nor version.dll there is Cheeky Foveated DLSS’s loader."));
    }

    // Sim-QVFR's own DLSS, when it is on: its nvngx_dlss.dll (the one Sim-QVFR includes, or
    // the player's choice) and its version (made for the 310 series, DLSS 4).
    if (in.ownDlss) {
        const std::wstring where = in.ownDlssPath.empty() ? std::wstring(tr(L"the folder chosen")) : in.ownDlssPath;
        if (!in.ownDlssFound && in.ownDlssIncluded) {
            Check& c = add("own_dlss", CheckTone::Warn, tr(L"DLSS is on, but NVIDIA’s DLSS file is missing"),
                           trf(L"This copy of Sim-QVFR has no nvngx_dlss.dll (it should be at {path}). {game} runs "
                               L"without DLSS until you install Sim-QVFR again from its release zip, or turn DLSS off.",
                               {{L"path", where}, {L"game", shortName}}));
            c.fix = CheckFix::OpenDlss;
            c.fixLabel = tr(L"Open DLSS");
        } else if (!in.ownDlssFound) {
            Check& c = add("own_dlss", CheckTone::Warn, tr(L"The DLSS file you chose isn’t there"),
                           trf(L"There is no nvngx_dlss.dll at {path}, so DLSS uses the one Sim-QVFR includes. Choose "
                               L"another, or Use included.",
                               {{L"path", where}}));
            c.fix = CheckFix::OpenDlss;
            c.fixLabel = tr(L"Open DLSS");
        } else if (!in.ownDlssVersion.empty() && version_major(in.ownDlssVersion) < kCheekyDlssMajor) {
            Check& c = add("own_dlss", CheckTone::Warn,
                           trf(L"DLSS {version} is older than 310", {{L"version", in.ownDlssVersion}}),
                           tr(L"Sim-QVFR is made for DLSS 310 or newer (DLSS 4): with an older nvngx_dlss.dll the "
                              L"presets may not apply, or DLSS may not start. Choose a newer one, or Use included."));
            c.fix = CheckFix::OpenDlss;
            c.fixLabel = tr(L"Open DLSS");
        } else {
            add("own_dlss", CheckTone::Ok,
                in.ownDlssVersion.empty() ? std::wstring(tr(L"DLSS ready"))
                                          : trf(L"DLSS ready ({version})", {{L"version", in.ownDlssVersion}}),
                in.ownDlssIncluded
                    ? trf(L"The nvngx_dlss.dll Sim-QVFR includes, at {path}.", {{L"path", in.ownDlssPath}})
                    : trf(L"Your nvngx_dlss.dll, at {path}.", {{L"path", in.ownDlssPath}}));
        }
    }

    // Cheeky Foveated DLSS, when it is on.
    if (in.cheekyLoad) {
        if (!in.cheekyRuntimeFound) {
            Check& c = add("cheeky_runtime", CheckTone::Bad, tr(L"Foveated DLSS is on, but Cheeky isn’t installed"),
                           tr(L"Cheeky Foveated DLSS is a separate download, and its files aren’t in Sim-QVFR’s "
                              L"folder, so the game would run without it. Turn it off, or install it."));
            c.fix = CheckFix::OpenDlss;
            c.fixLabel = tr(L"Open Foveated DLSS");
        }
        if (in.cheekyLayer != CheekyLayer::Enabled) {
            Check& c = add("cheeky_layer", CheckTone::Bad,
                           in.cheekyLayer == CheekyLayer::Disabled
                               ? tr(L"Foveated DLSS is on, but Cheeky’s VR add-on is turned off")
                               : tr(L"Foveated DLSS is on, but Cheeky’s VR add-on isn’t installed"),
                           tr(L"Cheeky needs its own small VR add-on, which its installer (CheekyOpenXRSetup.exe, in "
                              L"Cheeky’s official download) sets up. Turn Foveated DLSS off, or install it."));
            c.fix = CheckFix::OpenDlss;
            c.fixLabel = tr(L"Open Foveated DLSS");
        }
        if (in.cheekyRuntimeFound && in.cheekyLayer == CheekyLayer::Enabled)
            add("cheeky", CheckTone::Ok, tr(L"Cheeky Foveated DLSS ready"),
                tr(L"Its runtime and its OpenXR layer were found."));

        // The game's DLSS: Cheeky's own passes (its periphery pass, its presets) need the 310 series.
        if (!in.dlssVersion.empty()) {
            if (version_major(in.dlssVersion) >= kCheekyDlssMajor) {
                add("dlss_version", CheckTone::Ok,
                    trf(L"{game}’s DLSS is {version}", {{L"game", shortName}, {L"version", in.dlssVersion}}),
                    tr(L"New enough for Foveated DLSS."));
            } else {
                Check& c = add("dlss_version", CheckTone::Warn,
                               trf(L"{game}’s DLSS is too old for Foveated DLSS", {{L"game", shortName}}),
                               trf(L"{game} has DLSS {version}. Foveated DLSS needs DLSS 310 or newer "
                                   L"(DLSS 4): with an older one Cheeky can’t make its own periphery pass, and the "
                                   L"game’s DLSS runs as it is. Replace nvngx_dlss.dll with a newer one (a tool such "
                                   L"as DLSS Swapper does it, or the NVIDIA app’s DLSS override); Steam’s file check "
                                   L"puts the original back.",
                                   {{L"game", shortName}, {L"version", in.dlssVersion}}));
                c.fix = CheckFix::ShowFile;
                c.fixLabel = tr(L"Show the file");
                c.fixTarget = in.dlssPath;
            }
        }

        const int toolkit = rank(in.layers, kToolkitLayer), cheeky = rank(in.layers, kCheekyLayer);
        if (toolkit >= 0 && cheeky >= 0 && toolkit > cheeky) {
            Check& c = add("toolkit_order", CheckTone::Bad, tr(L"OpenXR Toolkit is below Cheeky’s layer"),
                           tr(L"The game can crash at start in this order. Move OpenXR Toolkit above Cheeky’s layer."));
            c.fix = CheckFix::OpenLayers;
            c.fixLabel = tr(L"Open OpenXR layers");
            c.confirm = true;
        }
        if (in.cheekyFg && toolkit >= 0 && in.toolkitTurbo == 1) {
            Check& c = add("toolkit_turbo", CheckTone::Bad, tr(L"OpenXR Toolkit’s Turbo mode is on"),
                           trf(L"{game} crashes with Turbo mode and Cheeky’s frame generation together. Turning it off "
                               L"changes Toolkit’s setting for {game} only.",
                               {{L"game", shortName}}));
            c.fix = CheckFix::TurboOff;
            c.fixLabel = tr(L"Turn off");
            c.confirm = true;
        }
    }

    // Eye tracking at the last launch.
    if (method && in.eyeTracked && in.eyeTracker == EyeTracker::Missing) {
        Check& c = add("eye_tracking", CheckTone::Warn, tr(L"No eye tracking at the last launch"),
                       tr(L"The focus area stayed in the centre. Check that the headset’s eye tracker is on and "
                          L"calibrated, or choose Fixed foveation."));
        c.fix = CheckFix::OpenFoveation;
        c.fixLabel = tr(L"Open Foveation");
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
    if (!read_file(path, bytes, 0))  // a DLL: no text-file cap
        return OpenComposite::Missing;
    return bytes.find("OpenComposite") != std::string::npos ? OpenComposite::Installed : OpenComposite::Valve;
}

std::vector<std::wstring> cheeky_loaders(const std::wstring& gameFolder) {
    std::vector<std::wstring> out;
    for (const wchar_t* name : {L"dxgi.dll", L"version.dll"}) {
        const std::wstring path = (fs::path(gameFolder) / name).wstring();
        std::string bytes;
        if (read_file(path, bytes, 0) && bytes.find("Cheeky") != std::string::npos)  // a DLL: no text-file cap
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
