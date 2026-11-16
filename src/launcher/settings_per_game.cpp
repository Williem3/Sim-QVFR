#include "settings.h"

#include <algorithm>
#include <cwchar>
#include <filesystem>
#include <string>
#include <vector>

#include <windows.h>

#include "../games/game.h"
#include "../ini.h"

// Per-game copies of the settings: store, restore, seed and switch (settings.h "Per-game settings").
namespace simqvfr::launcher {

namespace {

// The key's value, kIniAbsent when it isn't there.
std::wstring ini_value(const std::wstring& ini, const std::wstring& section, const std::wstring& key) {
    return ini_get(ini.c_str(), section.c_str(), key.c_str(), kIniAbsent);
}

// A game's copy written in one go (each WritePrivateProfileString rewrites the whole file): its keys by section, in
// the ANSI code page as WritePrivateProfileString writes a new file, through a temporary file so a copy is never half
// written.
bool write_game_copy(const std::wstring& file, const std::vector<ProfileKey>& keys) {
    std::vector<std::wstring> sections;
    for (const ProfileKey& k : keys)
        if (std::find(sections.begin(), sections.end(), k.section) == sections.end())
            sections.push_back(k.section);
    std::wstring text;
    for (const std::wstring& section : sections) {
        text += L"[" + section + L"]\r\n";
        for (const ProfileKey& k : keys)
            if (k.section == section)
                text += k.key + L"=" + k.value + L"\r\n";
    }
    const int bytes = WideCharToMultiByte(CP_ACP, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    std::string ansi(static_cast<std::size_t>(std::max(bytes, 0)), '\0');
    if (bytes > 0)
        WideCharToMultiByte(CP_ACP, 0, text.c_str(), static_cast<int>(text.size()), ansi.data(), bytes, nullptr, nullptr);
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(file).parent_path(), ec);
    const std::wstring temp = file + L".new";
    HANDLE h = CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return false;
    DWORD written = 0;
    const bool ok = WriteFile(h, ansi.data(), static_cast<DWORD>(ansi.size()), &written, nullptr) && written == ansi.size();
    CloseHandle(h);
    if (!ok || !MoveFileExW(temp.c_str(), file.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        DeleteFileW(temp.c_str());
        return false;
    }
    return true;
}

// The ini's per-game keys (as a profile of its settings) and its [profiles] current, for a game's copy.
std::vector<ProfileKey> game_copy_keys(const std::wstring& ini) {
    std::vector<ProfileKey> keys = settings_profile(load_settings(ini), L"game").keys;
    const std::wstring current = ini_value(ini, L"profiles", L"current");
    if (current != kIniAbsent)
        keys.push_back({L"profiles", L"current", current});
    return keys;
}

}  // namespace

// A game's first copy: the settings in use, with the sizes the game starts a fresh install with (GameInfo::defaults).
std::vector<ProfileKey> first_game_copy(std::vector<ProfileKey> keys, const std::wstring& gameId) {
    std::size_t count = 0;
    const games::GameInfo* const* all = games::all_games(count);
    for (std::size_t i = 0; i < count; ++i) {
        if (_wcsicmp(games::wide_id(*all[i]).c_str(), gameId.c_str()) != 0)
            continue;
        const games::GameInfo::Defaults& d = all[i]->defaults;
        const std::pair<const wchar_t*, int> sizes[] = {{L"periphery_scale", d.periphery}, {L"focus_density", d.density},
                                                        {L"focus_percent", d.focusWidth}, {L"focus_height_percent", d.focusHeight}};
        for (const auto& [key, value] : sizes) {
            if (value <= 0)
                continue;
            const std::wstring text = std::to_wstring(value);
            bool found = false;
            for (ProfileKey& k : keys)
                if (_wcsicmp(k.section.c_str(), L"experiment") == 0 && _wcsicmp(k.key.c_str(), key) == 0) {
                    k.value = text;
                    found = true;
                }
            if (!found)
                keys.push_back({L"experiment", key, text});
        }
    }
    return keys;
}

bool store_game_settings(const std::wstring& ini, const std::wstring& gameId) {
    if (gameId.empty() || is_preset_name(gameId) || GetFileAttributesW(ini.c_str()) == INVALID_FILE_ATTRIBUTES)
        return false;
    return write_game_copy(game_settings_file(ini, gameId), game_copy_keys(ini));
}

bool restore_game_settings(const std::wstring& ini, const std::wstring& gameId) {
    Profile stored;
    if (gameId.empty() || is_preset_name(gameId) || !load_profile(game_settings_folder(ini), gameId, stored, true))
        return false;
    // Only the keys whose value differs are written: each write rewrites the whole ini, and between two games
    // usually only a few differ.
    bool ok = true;
    for (const ProfileKey& k : stored.keys)
        if ((per_game_key(k.section, k.key) || (k.section == L"profiles" && k.key == L"current")) &&
            ini_value(ini, k.section, k.key) != k.value)
            ok &= WritePrivateProfileStringW(k.section.c_str(), k.key.c_str(), k.value.c_str(), ini.c_str()) != 0;
    // A per-game key the copy doesn't hold (a setting newer than the copy, such as [quad] sharpen_percent): its default,
    // not the value the last game left in the ini, which would otherwise carry over into this game.
    static const std::vector<ProfileKey> defaults = settings_profile(Settings{}, L"defaults").keys;
    for (const ProfileKey& d : defaults) {
        const bool held = std::any_of(stored.keys.begin(), stored.keys.end(), [&](const ProfileKey& k) {
            return _wcsicmp(k.section.c_str(), d.section.c_str()) == 0 && _wcsicmp(k.key.c_str(), d.key.c_str()) == 0;
        });
        if (!held && per_game_key(d.section, d.key) && ini_value(ini, d.section, d.key) != d.value)
            ok &= WritePrivateProfileStringW(d.section.c_str(), d.key.c_str(), d.value.c_str(), ini.c_str()) != 0;
    }
    return ok;
}

int seed_game_settings(const std::wstring& ini) {
    if (GetFileAttributesW(ini.c_str()) == INVALID_FILE_ATTRIBUTES)
        return 0;
    std::size_t count = 0;
    const games::GameInfo* const* all = games::all_games(count);
    int made = 0;
    std::vector<ProfileKey> keys;  // the settings in use, read once for every game that needs them
    for (std::size_t i = 0; i < count; ++i) {
        const std::wstring file = game_settings_file(ini, games::wide_id(*all[i]));
        if (GetFileAttributesW(file.c_str()) != INVALID_FILE_ATTRIBUTES)
            continue;
        if (keys.empty())
            keys = game_copy_keys(ini);
        made += write_game_copy(file, first_game_copy(keys, games::wide_id(*all[i]))) ? 1 : 0;
    }
    return made;
}

bool switch_game_settings(const std::wstring& ini, const std::wstring& fromId, const std::wstring& toId) {
    bool ok = store_game_settings(ini, fromId);
    if (GetFileAttributesW(game_settings_file(ini, toId).c_str()) == INVALID_FILE_ATTRIBUTES)  // none yet: it starts from
        ok &= !toId.empty() && !is_preset_name(toId) &&                                         // the settings in use and
              write_game_copy(game_settings_file(ini, toId), first_game_copy(game_copy_keys(ini), toId));  // its own sizes
    ok &= restore_game_settings(ini, toId);
    return ok;
}

}  // namespace simqvfr::launcher
