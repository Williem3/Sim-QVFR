// Live diagnostic switches for a game's render passes, for developers ([experiment] <prefix>_diag=1 at launch;
// manifest/sim-qvfr.ini): switch k (0 based) toggles with Ctrl+Alt+<k + 1>, and the ini's <prefix>_diag_<name>_off = 1
// sets it (re-read every 45 frames, from the launcher's own ini so it can be edited while the game runs). Each game
// decides what a switch turns off (X-Plane: shader edits; ACC: console variables).
#pragma once

#include <windows.h>

#include <string>
#include <vector>

namespace simqvfr::games {

class DiagSwitches {
public:
    // At launch: on with [experiment] <prefix>_diag=1 in `ini`; `names` the switches, in hotkey order (at most 9).
    // The keys are then read from SIM_QVFR_SETTINGS (the launcher's ini), else `ini`. False: off.
    bool start(const wchar_t* ini, const wchar_t* prefix, std::vector<std::wstring> names) {
        if (!ini || GetPrivateProfileIntW(L"experiment", (std::wstring(prefix) + L"_diag").c_str(), 0, ini) != 1)
            return false;
        wchar_t settings[MAX_PATH] = {};
        const DWORD n = GetEnvironmentVariableW(L"SIM_QVFR_SETTINGS", settings, MAX_PATH);
        ini_ = n > 0 && n < MAX_PATH ? settings : ini;
        prefix_ = prefix;
        names_ = std::move(names);
        if (names_.size() > 9)
            names_.resize(9);
        off_.assign(names_.size(), false);
        held_.assign(names_.size(), false);
        iniOff_.assign(names_.size(), -1);
        return true;
    }
    bool on() const { return !ini_.empty(); }
    const std::wstring& ini() const { return ini_; }
    std::size_t count() const { return names_.size(); }
    const std::wstring& name(std::size_t k) const { return names_[k]; }
    bool off(std::size_t k) const { return k < off_.size() && off_[k]; }

    // Every frame: the hotkeys, and the ini now and then. True when a switch changed.
    bool poll() {
        if (!on())
            return false;
        auto down = [](int key) { return (GetAsyncKeyState(key) & 0x8000) != 0; };
        const bool chord = down(VK_CONTROL) && down(VK_MENU);
        bool changed = false;
        for (std::size_t k = 0; k < names_.size(); ++k) {
            const bool key = chord && down('1' + static_cast<int>(k));
            if (key && !held_[k]) {
                off_[k] = !off_[k];
                changed = true;
            }
            held_[k] = key;
        }
        if (++frames_ % 45 != 0)
            return changed;
        for (std::size_t k = 0; k < names_.size(); ++k) {
            const std::wstring key = prefix_ + L"_diag_" + names_[k] + L"_off";
            const int value = GetPrivateProfileIntW(L"experiment", key.c_str(), 0, ini_.c_str()) == 1 ? 1 : 0;
            if (value != iniOff_[k]) {
                // The first read only applies a switch the ini has on; later, any change in the ini wins.
                if (iniOff_[k] >= 0 || value) {
                    off_[k] = value == 1;
                    changed = true;
                }
                iniOff_[k] = value;
            }
        }
        return changed;
    }

private:
    std::wstring ini_, prefix_;
    std::vector<std::wstring> names_;
    std::vector<bool> off_, held_;
    std::vector<int> iniOff_;
    int frames_ = 0;
};

}  // namespace simqvfr::games
