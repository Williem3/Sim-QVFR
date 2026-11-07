// Profiles: named sets of the ini keys that shape the picture and what it costs, to switch between in the
// launcher's Profiles pane or from the headset menu (live, as any other change). Shared by the layer and the
// launcher; pure ini file work.
//
// - The presets (Performance, Balanced, Quality) are built in, a set for each foveated mode, and set only the
//   resolution: the outer views, the focus density and the focus area's size. They put the pixels in the centre:
//   low outer views, a dense focus area. Eye tracked, the area follows the eyes and can be small; fixed, it is
//   larger (the eyes move off the centre) and a little less dense. The rest of the settings stay as they are.
// - A saved profile is every key the launcher writes for the picture (the launcher's settings_profile), in
//   profiles\<name>.ini next to the launcher's ini. Not in one: the panels' places, the hotkeys, the OpenXR
//   layers and the launcher's own keys.
// - Using one writes its keys into the ini; [profiles] current names it, and it shows as changed once a key
//   differs (profile_matches).
// - Shared as text: "QVFR1:" and the profile in base64 (the launcher shares only what differs from the
//   defaults and takes in only the keys a profile of its own has: settings.h's share_profile, shared_profile).
#pragma once

#include <string>
#include <vector>

namespace simqvfr {

struct ProfileKey {
    std::wstring section, key, value;
    bool operator==(const ProfileKey&) const = default;
};

struct Profile {
    std::wstring name;
    std::vector<ProfileKey> keys;
    bool builtIn = false;
};

// The presets for eye tracked foveation, or for fixed.
std::vector<Profile> builtin_profiles(bool eyeTracked);
// What a preset sets, for the launcher ("Outer 45% · inner 110% · focus 35 × 35%"); empty for a saved profile.
std::wstring preset_summary(const Profile& profile);

// <the launcher's ini folder>\profiles.
std::wstring profiles_folder(const std::wstring& settingsIni);

// Per-game settings: each game keeps its own copy of the keys a profile holds (the launcher's settings_profile), in
// <the launcher's ini folder>\games\<game id>.ini, with its [profiles] current. The launcher's ini holds the selected
// game's ([launcher] game); switching games stores it into that game's copy and loads the other's (launcher/settings.h).
std::wstring game_settings_folder(const std::wstring& settingsIni);
std::wstring game_settings_file(const std::wstring& settingsIni, const std::wstring& gameId);
// Where a game's per-game keys are written (the headset menu, a running game): the launcher's ini while that game is
// the one it shows, else the game's own copy, so a change never lands in another game's settings.
std::wstring game_settings_target(const std::wstring& settingsIni, const std::wstring& gameId);
// The presets, then the saved profiles by name.
std::vector<std::wstring> profile_names(const std::wstring& folder);
// A preset by name is the one for the mode (`eyeTracked`: [quad] gaze).
bool load_profile(const std::wstring& folder, const std::wstring& name, Profile& profile, bool eyeTracked);
// Writes <folder>\<name>.ini afresh (the folder made if needed). False for a preset's name or a bad one.
bool save_profile(const std::wstring& folder, const Profile& profile);
bool delete_profile(const std::wstring& folder, const std::wstring& name);
// A name a saved profile can have: 1..40 characters, none a file name can't hold, not a preset's.
bool valid_profile_name(const std::wstring& name);
bool is_preset_name(const std::wstring& name);

// Writes the profile's keys into `ini`. False if a write failed.
bool apply_profile(const wchar_t* ini, const Profile& profile);
// Every key of the profile has its value in `ini` (in QVFR but eye_scale, its fallback alone).
bool profile_matches(const wchar_t* ini, const Profile& profile);
// [profiles] current: the profile last used (empty: none).
std::wstring current_profile(const wchar_t* ini);
bool set_current_profile(const wchar_t* ini, const std::wstring& name);

// "QVFR1:" + base64 of the profile as UTF-8 lines ("name", then "[section]" and "key=value" lines), and back.
// import_profile is false for anything else, or a profile without a valid name or keys.
std::string export_profile(const Profile& profile);
bool import_profile(const std::string& text, Profile& profile);

}  // namespace simqvfr
