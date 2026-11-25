// Offline check of the launcher's hotkeys (input/hotkeys.h) and profiles (profile/profiles.h; settings.h's
// settings_profile, share_profile, shared_profile).
#include "launcher_test_kit.h"

int SIM_QVFR_TEST_MAIN() {
    folder = temp_path(L"simqvfr-launcher-profiles-test");
    std::error_code ec;
    fs::remove_all(folder, ec);
    fs::create_directories(folder);

    // Hotkeys (input/hotkeys.h): key and wheel control text both ways, hats, the ini, clashes, the launcher's settings.
    {
        KeyCombo k;
        check(parse_key(L"Ctrl+Alt+P", k) && k == KeyCombo{'P', true, true, false} && key_text(k) == L"Ctrl+Alt+P",
              "hotkeys: Ctrl+Alt+P both ways");
        check(parse_key(L"shift+f5", k) && k == KeyCombo{VK_F5, false, false, true} && key_text(k) == L"Shift+F5",
              "hotkeys: names and modifiers in any case");
        check(parse_key(L"Ctrl+Num +", k) && k.vk == VK_ADD && k.ctrl && key_text(k) == L"Ctrl+Num +" &&
                  parse_key(L"Alt+Page Down", k) && k.vk == VK_NEXT && parse_key(L"Num 7", k) && k.vk == VK_NUMPAD7,
              "hotkeys: a key named with a plus, a space, the number pad");
        check(parse_key(key_text(KeyCombo{0xE2, false, true, false}), k) && k.vk == 0xE2,
              "hotkeys: a key without a name round-trips by its number");
        check(!parse_key(L"", k) && !parse_key(L"Ctrl+", k) && !parse_key(L"Ctrl+Alt", k) && !parse_key(L"Hyper+P", k) &&
                  !parse_key(L"F25", k),
              "hotkeys: not keys");
        check(key_text(KeyCombo{}).empty(), "hotkeys: no key, no text");

        PadControl pad;
        check(parse_pad(L"0EB7_0020:button13:Fanatec Wheel", pad) && pad.device == L"0EB7_0020" && pad.control == 12 &&
                  pad.name == L"Fanatec Wheel" && pad_value(pad) == L"0EB7_0020:button13:Fanatec Wheel" &&
                  control_text(pad.control) == L"Button 13",
              "hotkeys: a wheel button both ways, numbered from 1 as Windows shows it");
        check(parse_pad(L"046D_C24F:hat1left:G29", pad) && pad.control == kPadHatBase + kHatLeft && control_text(pad.control) == L"Hat left" &&
                  parse_pad(L"046D_C24F:hat2up:", pad) && pad.control == kPadHatBase + 4 + kHatUp &&
                  control_text(pad.control) == L"Hat 2 up" && pad_value(pad) == L"046D_C24F:hat2up:",
              "hotkeys: hat directions");
        check(!parse_pad(L"", pad) && !parse_pad(L"046D_C24F:button0:", pad) && !parse_pad(L"046D_C24F:button129:", pad) &&
                  !parse_pad(L":button1:", pad) && !parse_pad(L"046D_C24F:hat5up:", pad) && !parse_pad(L"046D_C24F:hat1in:", pad),
              "hotkeys: not wheel controls");
        check(pad_value(PadControl{L"1234_5678", 0, L"Box; v2\r\n"}) == L"1234_5678:button1:Box  v2  ",
              "hotkeys: a name keeps no ini comment or line break");
        check(hat_directions(0xFFFFFFFF) == 0 && hat_directions(0) == (1u << kHatUp) && hat_directions(9000) == (1u << kHatRight) &&
                  hat_directions(4500) == ((1u << kHatUp) | (1u << kHatRight)) && hat_directions(27000) == (1u << kHatLeft) &&
                  hat_directions(31500) == ((1u << kHatLeft) | (1u << kHatUp)),
              "hotkeys: a hat's angle, diagonals holding two directions");

        const std::wstring hotkeysIni = (folder / "hotkeys.ini").wstring();
        const Hotkeys defaults = default_hotkeys();
        check(read_hotkeys(hotkeysIni.c_str()) == defaults &&
                  key_text(defaults[static_cast<int>(HotkeyAction::Overlay)].key) == L"Ctrl+Alt+P" &&
                  key_text(defaults[static_cast<int>(HotkeyAction::FrameCapture)].key) == L"Ctrl+Alt+F" &&
                  !defaults[static_cast<int>(HotkeyAction::MenuToggle)].key.set(),
              "hotkeys: unset, the defaults (no open/close key)");
        Hotkeys changedKeys = defaults;
        changedKeys[static_cast<int>(HotkeyAction::Visor)].key = {};
        changedKeys[static_cast<int>(HotkeyAction::MenuToggle)].pad = {L"0EB7_0020", 4, L"Wheel"};
        changedKeys[static_cast<int>(HotkeyAction::MenuUp)].pad = {L"0EB7_0020", kPadHatBase + kHatUp, L"Wheel"};
        check(write_hotkeys(hotkeysIni.c_str(), changedKeys) && read_hotkeys(hotkeysIni.c_str()) == changedKeys,
              "hotkeys: written and read back, a cleared key staying cleared");
        changedKeys[static_cast<int>(HotkeyAction::Overlay)].key = changedKeys[static_cast<int>(HotkeyAction::CpuCapture)].key;
        changedKeys[static_cast<int>(HotkeyAction::MenuDown)].pad = changedKeys[static_cast<int>(HotkeyAction::MenuToggle)].pad;
        check(hotkey_clash(changedKeys, HotkeyAction::Overlay, false) == static_cast<int>(HotkeyAction::CpuCapture) &&
                  hotkey_clash(changedKeys, HotkeyAction::MenuToggle, true) == static_cast<int>(HotkeyAction::MenuDown) &&
                  hotkey_clash(changedKeys, HotkeyAction::Visor, false) == -1 && hotkey_clash(changedKeys, HotkeyAction::Visor, true) == -1,
              "hotkeys: clashes, but none between two unset ones");

        Settings withKeys;
        withKeys.hotkeys[static_cast<int>(HotkeyAction::Overlay)].key = {VK_F9, false, false, false};
        withKeys.frameCapture = true;
        check(save_settings(hotkeysIni, withKeys) &&
                  load_settings(hotkeysIni).hotkeys[static_cast<int>(HotkeyAction::Overlay)].key.vk == VK_F9 &&
                  load_settings(hotkeysIni).frameCapture,
              "hotkeys: saved and loaded with the launcher's settings (the live keys)");
    }

    // Profiles (profile/profiles.h; settings.h's settings_profile, share_profile, shared_profile).
    {
        const fs::path profileFolder = folder / "profiles";
        const std::vector<Profile> presets = builtin_profiles(true), fixedPresets = builtin_profiles(false);
        check(presets.size() == 3 && presets[0].name == L"Performance" && presets[2].name == L"Quality" &&
                  fixedPresets.size() == 3 && fixedPresets[1].name == L"Balanced" &&
                  preset_summary(presets[1]) == L"Periphery 25% \u00b7 focus density 160% \u00b7 focus 28 \u00d7 28%" &&
                  preset_summary(fixedPresets[1]) == L"Periphery 25% \u00b7 focus density 140% \u00b7 focus 40 \u00d7 35%",
              "profiles: three presets for each foveated mode, by the same names, each summed up");
        // Every preset fits the 8192-pixel limit, keeps the outer views at most 25% and the centre denser than native;
        // the fixed set's area is larger than the eye tracked one's, and less dense.
        bool fit = true, centred = true, fixedLarger = true;
        const auto value = [](const Profile& p, const wchar_t* key) {
            for (const ProfileKey& k : p.keys)
                if (k.key == key)
                    return _wtoi(k.value.c_str());
            return 0;
        };
        for (size_t i = 0; i < 3; ++i)
            for (const Profile* p : {&presets[i], &fixedPresets[i]}) {
                fit &= value(*p, L"periphery_scale") * 100 + value(*p, L"focus_density") * value(*p, L"focus_percent") < 100 * 100;
                centred &= value(*p, L"periphery_scale") <= 25 && value(*p, L"focus_density") > 100 &&
                           value(*p, L"eye_scale") == value(*p, L"periphery_scale");
                fixedLarger &= p == &presets[i] ||
                               (value(*p, L"focus_percent") > value(presets[i], L"focus_percent") &&
                                value(*p, L"focus_height_percent") > value(presets[i], L"focus_height_percent") &&
                                value(*p, L"focus_density") < value(presets[i], L"focus_density"));
            }
        check(fit, "profiles: each preset keeps periphery + density x focus under 1 (the 8192-pixel limit)");
        check(centred && value(presets[2], L"focus_density") == 180, "profiles: presets weigh the centre: outer at most 25%, centre up to 180%");
        check(fixedLarger, "profiles: fixed presets: a larger focus area, less dense, than eye tracked ones");

        check(valid_profile_name(L"Wet night 90 Hz") && !valid_profile_name(L"") && !valid_profile_name(L"a/b") &&
                  !valid_profile_name(L"balanced") && !valid_profile_name(L" padded") && !valid_profile_name(L"dot.") &&
                  !valid_profile_name(std::wstring(41, L'x')),
              "profiles: names a file can hold, none a preset's");
        check(profile_names(profileFolder.wstring()) == std::vector<std::wstring>{L"Performance", L"Balanced", L"Quality"},
              "profiles: without a folder, the presets");

        Settings mine;
        mine.outer = 42;
        mine.width = 33;
        mine.helmet.on = true;
        mine.osd.enabled = true;  // a panel's place: not in a profile
        mine.hotkeys[static_cast<int>(HotkeyAction::Overlay)].key = {VK_F9, false, false, false};
        const Profile saved = settings_profile(mine, L"Night");
        bool panels = false, hotkeysIn = false, launcherKeys = false, outerIn = false;
        for (const ProfileKey& k : saved.keys) {
            panels |= k.section == L"osd" || k.section == L"menu";
            hotkeysIn |= k.section == L"hotkeys";
            launcherKeys |= k.section == L"launcher" || k.key == L"cpu_profile";
            outerIn |= k.key == L"periphery_scale" && k.value == L"42";
        }
        check(!saved.keys.empty() && outerIn && !panels && !hotkeysIn && !launcherKeys,
              "profiles: a profile of the settings holds the picture's keys, not the panels, hotkeys or launcher's");
        check(save_profile(profileFolder.wstring(), saved) &&
                  profile_names(profileFolder.wstring()) == std::vector<std::wstring>{L"Performance", L"Balanced", L"Quality", L"Night"},
              "profiles: saved, listed after the presets");
        Profile loaded;
        check(load_profile(profileFolder.wstring(), L"Night", loaded, true) && loaded.keys == saved.keys && !loaded.builtIn,
              "profiles: loaded back as saved");
        Profile preset, fixedPreset;
        check(load_profile(profileFolder.wstring(), L"balanced", preset, true) && preset.builtIn && preset.keys == presets[1].keys &&
                  load_profile(profileFolder.wstring(), L"Balanced", fixedPreset, false) && fixedPreset.keys == fixedPresets[1].keys,
              "profiles: a preset by name, any case, the one for the foveated mode");
        Profile badName = saved;
        badName.name = L"Quality";
        check(!save_profile(profileFolder.wstring(), badName), "profiles: no saved profile takes a preset's name");

        const std::wstring profileIni = (folder / "profile-settings.ini").wstring();
        Settings other;
        other.outer = 70;
        save_settings(profileIni, other);
        check(!profile_matches(profileIni.c_str(), saved) && apply_profile(profileIni.c_str(), saved) &&
                  profile_matches(profileIni.c_str(), saved) && load_settings(profileIni).outer == 42 &&
                  load_settings(profileIni).helmet.on,
              "profiles: used, the ini matches it and the launcher reads its settings");
        check(apply_profile(profileIni.c_str(), presets[0]) && load_settings(profileIni).outer == 20 &&
                  load_settings(profileIni).inner == 140 &&
                  load_settings(profileIni).helmet.on && !profile_matches(profileIni.c_str(), saved) &&
                  profile_matches(profileIni.c_str(), presets[0]),
              "profiles: a preset changes the resolution only");
        // The headset menu changes periphery_scale and leaves eye_scale (QVFR's fallback alone): not a change in QVFR.
        apply_profile(profileIni.c_str(), saved);
        WritePrivateProfileStringW(L"experiment", L"eye_scale", L"10", profileIni.c_str());
        const bool qvfrMatch = profile_matches(profileIni.c_str(), saved);
        WritePrivateProfileStringW(L"experiment", L"mode", L"focus_only", profileIni.c_str());
        check(qvfrMatch && !profile_matches(profileIni.c_str(), saved),
              "profiles: eye_scale doesn't count in QVFR; in a research mode (its resolution) it does");
        check(set_current_profile(profileIni.c_str(), L"Night") && current_profile(profileIni.c_str()) == L"Night",
              "profiles: the current one named");

        const std::string sharedText = share_profile(saved);
        Profile roundTrip;
        check(sharedText.rfind("QVFR1:", 0) == 0 && sharedText.size() < export_profile(saved).size() &&
                  shared_profile(sharedText, roundTrip) && roundTrip.name == L"Night" && roundTrip.keys == saved.keys,
              "profiles: shared as only what differs, and back whole");
        check(shared_profile("  " + sharedText + "\r\n", roundTrip), "profiles: pasted with spaces around");
        Profile sneaky;
        sneaky.name = L"Sneaky";
        sneaky.keys = {{L"cheeky", L"runtime", L"C:\\evil.dll"}, {L"launcher", L"game_dir", L"C:\\x"},
                       {L"experiment", L"mode", L"double_render"}, {L"experiment", L"periphery_scale", L"20"}};
        Profile safe;
        bool leaked = false, outer20 = false, modeKept = false;
        check(shared_profile(export_profile(sneaky), safe), "profiles: a shared text with other keys still reads");
        for (const ProfileKey& k : safe.keys) {
            leaked |= k.key == L"runtime" || k.key == L"game_dir" || k.value == L"double_render";
            outer20 |= k.key == L"periphery_scale" && k.value == L"20";
            modeKept |= k.key == L"mode" && k.value == L"views4";
        }
        check(!leaked && outer20 && modeKept, "profiles: a shared text sets only a profile's keys, and the method only to a method");
        Profile legacy, fromLegacy;
        legacy.name = L"Legacy";
        legacy.keys = {{L"experiment", L"mode", L"quad"}};
        bool legacyKept = true;
        check(shared_profile(export_profile(legacy), fromLegacy), "profiles: a shared text of the removed Two-Loop still reads");
        for (const ProfileKey& k : fromLegacy.keys)
            legacyKept &= !(k.key == L"mode" && k.value != L"views4");
        check(legacyKept, "profiles: a shared text can't switch the method away from QVFR");
        Profile junk;
        check(!shared_profile("hello", junk) && !shared_profile("QVFR1:!!!", junk) && !import_profile("QVFR1:", junk) &&
                  !import_profile(export_profile(Profile{L"", {{L"quad", L"gaze", L"1"}}}), junk) &&
                  !import_profile(export_profile(Profile{L"Empty", {}}), junk),
              "profiles: not shared profiles");
        check(delete_profile(profileFolder.wstring(), L"Night") &&
                  profile_names(profileFolder.wstring()).size() == 3 && !delete_profile(profileFolder.wstring(), L"Balanced"),
              "profiles: deleted; a preset can't be");
    }

    fs::remove_all(folder, ec);
    return test_result();
}
