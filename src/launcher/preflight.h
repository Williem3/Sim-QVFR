// Launch checks: what would stop ace-quadview working at the next launch, found before it, each with
// what to do about it (the launcher's Launch checks pane and its action bar badge). The checks are
// pure (preflight_checks); the scans that gather their input are the small Windows functions below
// and launcher_preflight (main.cpp).
//
// - The game build: ace-quadview patches the game builds its addresses were taken from (games/<id>/game.h).
//   After a game update the layer refuses to patch and ACE runs unchanged; the launcher says so first.
// - Steam running and ACE closed (the launch refuses otherwise).
// - Cheeky Foveated DLSS's loader (dxgi.dll, version.dll) in the game folder: ACE fails to start with it.
// - With Cheeky on: its runtime and OpenXR layer found; OpenXR Toolkit nearer ACE than Cheeky's layer
//   (ACE crashes at start otherwise); with its frame generation, Toolkit's Turbo mode off (ACE crashes).
// - Eye tracked foveation without eye tracking at the last launch.
// - A game that reaches OpenXR through OpenComposite (ACC): OpenComposite's openvr_api.dll in its folder.
#pragma once

#include <string>
#include <vector>

#include "launch_plan.h"
#include "settings.h"

namespace aceqv::launcher {

inline constexpr char kToolkitLayer[] = "XR_APILAYER_MBUCCHIA_toolkit";
inline constexpr wchar_t kReleasesUrl[] = L"https://github.com/Williem3/Sim-QVFR/releases";

// Pending: still being found out (the exe's hash). Ok and Pending never hold a launch back.
enum class CheckTone { Ok, Warn, Bad, Pending };
// What a check's button does (the window carries it out).
enum class CheckFix { None, OpenReleases, StartSteam, ShowFile, OpenLayers, TurboOff, OpenDlss, OpenRendering, OpenGuide };
inline constexpr wchar_t kOpenCompositeGuideUrl[] =
    L"https://github.com/Williem3/Sim-QVFR/blob/main/docs/acc-opencomposite.md";

struct Check {
    std::string id;  // stable: the tests' and the log's name for it
    CheckTone tone = CheckTone::Ok;
    std::wstring title, detail;
    CheckFix fix = CheckFix::None;
    std::wstring fixLabel, fixTarget;  // the button's label; ShowFile's file
    bool confirm = false;  // Launch asks before starting the game (it would crash, or run without ace-quadview)
};

enum class BuildState { Unknown, Hashing, Supported, Unsupported };
// The game's openvr_api.dll (GameInfo::openComposite): OpenComposite's, Valve's, missing, or not looked at.
enum class OpenComposite { Unknown, Installed, Valve, Missing };
enum class LaunchMode { Off, Qvfr, TwoLoop };

struct PreflightInput {
    std::wstring gameExe;  // empty: not found
    BuildState build = BuildState::Unknown;
    LaunchMode mode = LaunchMode::Qvfr;
    bool steamRunning = true, gameRunning = false;
    std::vector<std::wstring> cheekyLoaders;  // Cheeky's loader DLLs in the game folder
    bool cheekyLoad = false, cheekyFg = false, cheekyRuntimeFound = false;
    CheekyLayer cheekyLayer = CheekyLayer::Missing;
    LayerPlan layers;      // the launch's layers (plan_layers)
    int toolkitTurbo = -1; // OpenXR Toolkit's Turbo mode for ACE: 1 on, 0 off, -1 not set (or no Toolkit)
    bool eyeTracked = true;
    EyeTracker eyeTracker = EyeTracker::Unknown;
    OpenComposite openComposite = OpenComposite::Unknown;  // only for a game that needs it
};

std::vector<Check> preflight_checks(const PreflightInput& in);

// The worst tone among `checks` (Pending counts as Ok), and the first check with it.
CheckTone worst_tone(const std::vector<Check>& checks);
const Check* first_with(const std::vector<Check>& checks, CheckTone tone);

// The build an exe hash is: Hashing while `sha` is empty and `hashing`, Unknown if it is empty otherwise.
BuildState build_state(const std::string& sha, bool hashing);

// Whose openvr_api.dll `path` is: Valve's has its version information, OpenComposite's names itself.
OpenComposite open_composite(const std::wstring& path);
// dxgi.dll and version.dll in `gameFolder` that are Cheeky Foveated DLSS's loader (they name Cheeky).
std::vector<std::wstring> cheeky_loaders(const std::wstring& gameFolder);
// OpenXR Toolkit's per-app settings (HKCU\SOFTWARE\OpenXR_Toolkit\<app>) for the app whose "module" is
// `exe`: its "turbo" value, or -1. TurboOff writes 0 there; false if it cannot.
int toolkit_turbo(const std::wstring& exe);
bool toolkit_turbo_off(const std::wstring& exe);

}  // namespace aceqv::launcher
