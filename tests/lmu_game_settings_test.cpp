// Offline check of LMU's own graphics settings in its files (launcher/lmu_game_settings.h): values found in the ini's
// [COMPONENTS] and Settings.JSON's "Graphic Options" only, a change rewriting that value alone (every other byte kept,
// CRLF and comments too), unknown values left alone, and a file replaced with a backup kept and put back.
#include "launcher_test_kit.h"

#include "launcher/lmu_game_settings.h"

namespace {

namespace ls = simqvfr::launcher::lmu_settings;

const std::string kIni =
    "//[[gMa1.002f (c)2016    ]] [[            ]]\r\n"
    "[COMPONENTS]\r\n"
    "VideoResW=2560\r\n"
    "EPostProcessingSettings=2\r\n"
    "MSAA=0\r\n"
    "UseFXAA=0\r\n"
    "ViewParams=(0.540, 0.340, 0.600, 15.000, 0.037) // screen width (m), screen height (m)\r\n"
    "VRFOVScaling=(1.000, 0.900)\r\n"
    "\r\n"
    "[VERSION]\r\n"
    "MSAA=99\r\n";

const std::string kJson =
    "{\r\n"
    "  \"CHAT\": {\r\n"
    "    \"Quick Chat #1\": \"Shadows: \\\"off\\\", {not} a key\",\r\n"
    "    \"Shadows\": 9\r\n"
    "  },\r\n"
    "  \"Graphic Options\": {\r\n"
    "    \"Box Outline\": 16711680,\r\n"
    "    \"Rearview Cull\": true,\r\n"
    "    \"Rearview Cull#\": \"Whether to cull objects in the rearview, based on {visgroups}\",\r\n"
    "    \"Rearview_Back_Clip\": 150.0,\r\n"
    "    \"Shadows\": 1,\r\n"
    "    \"Shadows#\": \"Shadow detail: 0=Off 1=Low\",\r\n"
    "    \"Garage Car Detail\": 0.009999999776482582,\r\n"
    "    \"Track Detail\": 2\r\n"
    "  },\r\n"
    "  \"Sound Options\": {\r\n"
    "    \"Track Detail\": 7\r\n"
    "  }\r\n"
    "}";

std::string slurp(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(f), {});
}

void spill(const fs::path& p, const std::string& bytes) {
    fs::create_directories(p.parent_path());
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f << bytes;
}

}  // namespace

int SIM_QVFR_TEST_MAIN() {
    folder = temp_path(L"simqvfr-lmu-game-settings-test");
    std::error_code ec;
    fs::remove_all(folder, ec);
    fs::create_directories(folder);

    // Reading: the ini's [COMPONENTS] only (MSAA in [VERSION] ignored), JSON's "Graphic Options" only (strings with
    // quotes and braces skipped, the same key in other objects ignored).
    {
        std::string v;
        check(ls::read_value(ls::File::Config, kIni, "MSAA", v) && v == "0", "ini: MSAA from [COMPONENTS], not [VERSION]");
        check(ls::read_value(ls::File::Config, kIni, "msaa", v) && v == "0", "ini: keys compared without case");
        check(ls::read_value(ls::File::Config, kIni, "ViewParams", v) && v == "(0.540, 0.340, 0.600, 15.000, 0.037)",
              "ini: a value ends before its // comment");
        check(!ls::read_value(ls::File::Config, kIni, "VideoRes", v), "ini: a key's prefix is not the key");
        check(ls::read_value(ls::File::Player, kJson, "Shadows", v) && v == "1", "json: Shadows from Graphic Options");
        check(ls::read_value(ls::File::Player, kJson, "Track Detail", v) && v == "2",
              "json: the last member, not Sound Options' one");
        check(ls::read_value(ls::File::Player, kJson, "Rearview Cull", v) && v == "true", "json: a boolean");
        check(!ls::read_value(ls::File::Player, kJson, "Rearview", v), "json: no partial key match");
        check(ls::read_value(ls::File::Player, kJson, "Rearview_Back_Clip", v) && v == "150.0", "json: a float");
        check(!ls::read_value(ls::File::Player, "not json", "Shadows", v), "json: garbage has no settings");
        check(ls::read_value(ls::File::Player, "\xEF\xBB\xBF" + kJson, "Shadows", v) && v == "1", "json: a BOM is skipped");
        double x = 0, y = 0;
        check(ls::read_value(ls::File::Config, kIni, "VRFOVScaling", v) && ls::parse_fov_scale(v, x, y) &&
                  std::fabs(x - 1.0) < 1e-9 && std::fabs(y - 0.9) < 1e-9,
              "ini: the VR FOV scale's two numbers");
    }

    // Writing: only the value's bytes change.
    {
        std::string ini = kIni;
        check(ls::write_value(ls::File::Config, ini, "MSAA", "4"), "ini: MSAA written");
        std::string expect = kIni;
        expect.replace(expect.find("MSAA=0"), 6, "MSAA=4");
        check(ini == expect, "ini: one value changed, every other byte (CRLF, [VERSION]'s MSAA) kept");
        std::string fov = kIni;
        check(ls::write_value(ls::File::Config, fov, "ViewParams", "(1, 1, 1, 1, 1)") &&
                  fov.find("ViewParams=(1, 1, 1, 1, 1) // screen width") != std::string::npos,
              "ini: a value's comment is kept");
        std::string json = kJson;
        check(ls::write_value(ls::File::Player, json, "Rearview Cull", "false"), "json: a boolean written");
        check(ls::write_value(ls::File::Player, json, "Shadows", "3"), "json: a number written");
        std::string expectJson = kJson;
        expectJson.replace(expectJson.find("\"Rearview Cull\": true"), 21, "\"Rearview Cull\": false");
        const std::size_t at = expectJson.find("\"Shadows\": 1");
        expectJson.replace(at, 12, "\"Shadows\": 3");
        check(json == expectJson, "json: those two values changed, the rest (CHAT's Shadows, comments) byte for byte");
        std::string none = kJson;
        check(!ls::write_value(ls::File::Player, none, "Max Visible Vehicles", "20") && none == kJson,
              "json: a setting the file lacks isn't added");
    }

    // The table: every choice's values distinct, a value as a number ("2.0" is "2"), unknown values found as none.
    {
        bool distinct = true;
        for (const ls::Def& d : ls::defs())
            for (std::size_t i = 0; i < d.count; ++i)
                for (std::size_t j = i + 1; j < d.count; ++j)
                    distinct = distinct && std::string(d.options[i].value) != d.options[j].value;
        check(distinct && !ls::defs().empty(), "table: each choice's values distinct");
        const ls::Def* shadows = nullptr;
        for (const ls::Def& d : ls::defs())
            if (std::string(d.key) == "Shadows")
                shadows = &d;
        check(shadows && ls::option_index(*shadows, "2") == 2 && ls::option_index(*shadows, "2.0") == 2 &&
                  ls::option_index(*shadows, "7") == -1 && ls::option_index(*shadows, "") == -1,
              "table: values compared as numbers, unknown ones are none");
        const ls::Def* mirror = nullptr;
        for (const ls::Def& d : ls::defs())
            if (std::string(d.key) == "Rearview_Back_Clip")
                mirror = &d;
        check(mirror && mirror->kind == ls::Kind::Choice && ls::option_index(*mirror, "150.0") >= 0 &&
                  ls::option_index(*mirror, "150") == ls::option_index(*mirror, "150.0") &&
                  ls::option_index(*mirror, "0.0") == 0 && ls::option_index(*mirror, "120.0") == -1,
              "table: mirror draw distance finds LMU's float values, 0.0 is the circuit's default");
        bool grouped = true;
        int advanced = 0;
        for (const ls::Def& d : ls::defs()) {
            grouped = grouped && d.group >= 0 && d.group < ls::kGroupCount;
            advanced += d.group == ls::kGroupCount - 1;
        }
        check(grouped && advanced == 3 && std::wstring(ls::kGroups[ls::kVrGroup]) == L"Virtual reality",
              "table: every setting on a card, three on Advanced performance, the FOV check on Virtual reality");
        bool known = false;
        check(ls::toggle_on("true", known) && known && !ls::toggle_on("0", known) && known &&
                  (ls::toggle_on("maybe", known), !known),
              "table: switches from true/false and 1/0, anything else unknown");
        check(ls::toggle_value(ls::File::Config, true) == "1" && ls::toggle_value(ls::File::Player, false) == "false",
              "table: switches written as each file has them");
    }

    // Files: a change writes the backup once (the file as it was), replaces the file, and is put back from it.
    {
        const fs::path user = folder / L"UserData";
        const fs::path ini = user / L"Config_DX11_VR.ini", json = user / L"player" / L"Settings.JSON";
        spill(ini, kIni);
        spill(json, kJson);
        const ls::Snapshot before = ls::read_all(user.wstring());
        check(before.configOk && before.playerOk && before.fovScale == "(1.000, 0.900)", "files: both read");
        check(ls::set_setting(user.wstring(), ls::File::Player, "Shadows", "4").empty(), "files: Shadows set");
        check(ls::set_setting(user.wstring(), ls::File::Player, "Track Detail", "3").empty(), "files: a second change");
        std::string v;
        check(ls::read_value(ls::File::Player, slurp(json), "Shadows", v) && v == "4" &&
                  ls::read_value(ls::File::Player, slurp(json), "Track Detail", v) && v == "3",
              "files: both changes in the file");
        check(slurp(json.wstring() + L".sim-qvfr-backup") == kJson, "files: the backup is the file before the first change");
        check(!fs::exists(json.wstring() + L".sim-qvfr-new"), "files: no temporary file left");
        check(!ls::set_setting(user.wstring(), ls::File::Player, "No Such Key", "1").empty(), "files: an unknown key refused");
        check(ls::restore_backup(json.wstring()).empty() && slurp(json) == kJson, "files: the backup put back");
        check(slurp(ini) == kIni && !ls::has_backup(ini.wstring()), "files: the untouched file has no backup");

        // A read-only file (some players lock their settings this way) is left alone.
        SetFileAttributesW(ini.wstring().c_str(), FILE_ATTRIBUTE_READONLY);
        check(!ls::set_setting(user.wstring(), ls::File::Config, "MSAA", "8").empty() && slurp(ini) == kIni,
              "files: a read-only file is refused and unchanged");
        check(ls::read_all(user.wstring()).configReadOnly, "files: read-only reported");
        SetFileAttributesW(ini.wstring().c_str(), FILE_ATTRIBUTE_NORMAL);
        check(ls::read_all(L"").configOk == false, "files: no folder, nothing read");
        check(ls::user_folder(L"K:\\S\\Le Mans Ultimate\\Le Mans Ultimate.exe") == L"K:\\S\\Le Mans Ultimate\\UserData" &&
                  ls::user_folder(L"").empty(),
              "files: UserData next to the exe");
    }

    fs::remove_all(folder, ec);
    return test_result();
}
