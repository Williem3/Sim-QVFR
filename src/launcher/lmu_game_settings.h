// Le Mans Ultimate's own graphics settings, as its files hold them: UserData\Config_DX11_VR.ini ([COMPONENTS]: post
// effects, MSAA, FXAA, the monitor mirror, the VR FOV scale) and UserData\player\Settings.JSON ("Graphic Options": circuit,
// car and opponent detail, shadows, reflections, rear-view mirrors and the rest). The launcher's In-game settings page
// (ui/pane_lmu_game.cpp) shows and edits them while the game is closed.
//
// Edits are non-destructive: only the value of a setting changed is rewritten, in place, the rest of the file (its other
// settings, comments, order, line ends) byte for byte; a copy of each file as it was before the first edit is kept next to
// it (<file>.sim-qvfr-backup), and a file is replaced whole (written beside it, then moved over it), never left half
// written. A value the table doesn't know is shown as it is and left alone unless the player picks another.
#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "../i18n.h"

namespace simqvfr::launcher::lmu_settings {

enum class File { Config, Player };  // UserData\Config_DX11_VR.ini, UserData\player\Settings.JSON
enum class Kind { Choice, Toggle };

// One of a choice's values: its text in the file and its name (English, the language file's key).
struct Option {
    const char* value;
    const wchar_t* name;
};

// A setting the page edits: the file and key, how it is shown, the card it is on (kGroups' index).
struct Def {
    File file;
    const char* key;
    Kind kind;
    const wchar_t* label;  // English (tr() shows it)
    const wchar_t* help;
    int group;
    const Option* options = nullptr;  // Choice only
    std::size_t count = 0;
};

// The cards, in order.
inline constexpr const wchar_t* kGroups[] = {I18N(L"Image quality"),     I18N(L"Detail"),
                                             I18N(L"Lighting and effects"), I18N(L"Rear-view mirrors"),
                                             I18N(L"Virtual reality"),     I18N(L"Advanced performance")};
inline constexpr int kGroupCount = static_cast<int>(sizeof(kGroups) / sizeof(kGroups[0]));
inline constexpr int kVrGroup = 4;  // the card that also carries the VR FOV scale check

// Every setting, in the order shown.
const std::vector<Def>& defs();

// The game's UserData folder from its exe ("…\Le Mans Ultimate\Le Mans Ultimate.exe"), and each file's path in it.
std::wstring user_folder(const std::wstring& exe);
std::wstring file_path(const std::wstring& userFolder, File file);

// ---- Text (pure) ----------------------------------------------------------------------------

// A setting's value in a file's text: true and its text ("2", "true", "1.000"); false if the setting isn't there.
bool read_value(File file, const std::string& text, const char* key, std::string& value);
// The text with the setting's value replaced by `value`, everything else unchanged. False (text unchanged): the setting
// isn't there.
bool write_value(File file, std::string& text, const char* key, const std::string& value);

// A choice's option for a value (numbers compared as numbers: "1" is "1.0"); -1 when none is.
int option_index(const Def& d, const std::string& value);
// A switch's state from its value: JSON true/false, the ini's 0/1. `known` false for anything else.
bool toggle_on(const std::string& value, bool& known);
// A switch's value for a state in a file: JSON true/false, the ini 1/0.
std::string toggle_value(File file, bool on);

// The VR FOV scale ([COMPONENTS] VRFOVScaling "(x, y)"): false if it isn't two numbers.
bool parse_fov_scale(const std::string& value, double& x, double& y);

// ---- Files ----------------------------------------------------------------------------------

// The values as last read from the files (file_text: each file's bytes; ok false if it couldn't be read).
struct Snapshot {
    std::wstring userFolder;
    bool configOk = false, playerOk = false;
    bool configReadOnly = false, playerReadOnly = false;
    std::string configText, playerText;
    std::vector<std::string> values;  // per defs(), "" when the file lacks it
    std::vector<bool> present;
    std::string fovScale;  // VRFOVScaling's text, "" if missing
};
Snapshot read_all(const std::wstring& userFolder);

// A file's bytes; false if it can't be read.
bool read_file(const std::wstring& path, std::string& bytes);
// Writes `bytes` over `path`: the first time, a copy of the file as it is to <path>.sim-qvfr-backup; then the new
// bytes beside it and moved over it. Empty on success, else why (English, for the page to show through tr()).
std::wstring replace_file(const std::wstring& path, const std::string& bytes);
// The backup's path, and whether one is there.
std::wstring backup_path(const std::wstring& path);
bool has_backup(const std::wstring& path);
// Puts the backup back over the file (the backup is kept). Empty on success, else why.
std::wstring restore_backup(const std::wstring& path);

// Sets one setting in its file (read again first, so a change made meanwhile by the game is kept). Empty on success,
// else why.
std::wstring set_setting(const std::wstring& userFolder, File file, const char* key, const std::string& value);

}  // namespace simqvfr::launcher::lmu_settings
