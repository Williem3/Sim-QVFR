// Offline check of the game's graphics settings copied at a launch (launcher/game_settings_snapshot.h): LMU's listed
// settings only, with their names, nothing else of its files (driver name, livery, chat), a missing setting and an
// unreadable file said so, the summary line; and the report taking each zipped session's copy and the newest summary.
#include "launcher_test_kit.h"

#include "launcher/game_settings_snapshot.h"
#include "launcher/report.h"

namespace {

namespace sl = simqvfr::launcher;

const std::string kIni =
    "//[[gMa1.002f (c)2016    ]] [[            ]]\r\n"
    "[COMPONENTS]\r\n"
    "VideoGUID=00002C05-0000-0000-0000-000000000000\r\n"
    "RenderScale=1.000\r\n"
    "UpscaleMethod=0\r\n"
    "EPostProcessingSettings=2\r\n"
    "MSAA=4\r\n"
    "UseFXAA=0\r\n"
    "VRFOVScaling=(1.000, 1.000)\r\n"
    "\r\n"
    "[INSTALL]\r\n"
    "InstallTo=C:\\Users\\someone\\Games\r\n";

const std::string kJson =
    "{\r\n"
    "  \"CHAT\": {\r\n"
    "    \"Quick Chat #1\": \"hello from Jane\"\r\n"
    "  },\r\n"
    "  \"DRIVER\": {\r\n"
    "    \"Player Name\": \"Jane Racer\"\r\n"
    "  },\r\n"
    "  \"Graphic Options\": {\r\n"
    "    \"Player Livery\": \"Jane's livery\",\r\n"
    "    \"Shadows\": 3,\r\n"
    "    \"Shadows#\": \"Shadow detail\",\r\n"
    "    \"Track Detail\": 7,\r\n"
    "    \"Rearview Cull\": true,\r\n"
    "    \"Max Framerate\": 0.0\r\n"
    "  }\r\n"
    "}";

void spill(const fs::path& p, const std::string& bytes) {
    fs::create_directories(p.parent_path());
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f << bytes;
}

bool has(const std::string& text, const char* part) { return text.find(part) != std::string::npos; }

}  // namespace

int SIM_QVFR_TEST_MAIN() {
    folder = temp_path(L"simqvfr-game-settings-snapshot-test");
    std::error_code ec;
    fs::remove_all(folder, ec);
    fs::create_directories(folder);

    // LMU's copy: the listed settings, named; nothing else of the files.
    {
        const std::string t = sl::lmu_settings_text(true, kIni, true, kJson);
        check(has(t, "[Config_DX11_VR.ini]\n") && has(t, "[Settings.JSON Graphic Options]\n"), "lmu: both sections");
        check(has(t, "EPostProcessingSettings=2 ; Post effects: High\n"), "lmu: a choice with its name");
        check(has(t, "MSAA=4 ; MSAA: 4x\n"), "lmu: MSAA's sample count");
        check(has(t, "UseFXAA=0 ; FXAA: Off\n") && has(t, "Rearview Cull=true ; Rear-view cull: On\n"),
              "lmu: switches as On/Off, ini and JSON");
        check(has(t, "Shadows=3 ; Shadows: High\n"), "lmu: a JSON choice");
        check(has(t, "Track Detail=7 ; Circuit detail: other\n"), "lmu: a value the table doesn't know kept as it is");
        check(has(t, "RenderScale=1.000\n") && has(t, "VRFOVScaling=(1.000, 1.000)\n") && has(t, "Max Framerate=0.0\n"),
              "lmu: the extras, raw");
        check(has(t, "Opponent Detail= ; not in the file\n"), "lmu: a setting the file lacks");
        check(!has(t, "Jane") && !has(t, "someone") && !has(t, "VideoGUID") && !has(t, "Shadows#"),
              "lmu: no names, livery, chat, paths, GPU id or comments");
        const std::wstring s = sl::game_settings_summary(t);
        check(s.find(L"Post effects High, MSAA 4x, FXAA Off, Render scale 1.000, Upscaler 0") == 0,
              "lmu: the summary starts with image quality, in order");
        check(s.find(L"Shadows High") != std::wstring::npos && s.find(L"Circuit detail 7") != std::wstring::npos,
              "lmu: the summary names known values, raw unknown ones");
        check(s.find(L"Opponent") == std::wstring::npos, "lmu: the summary leaves out what the file lacks");
    }

    // A file that couldn't be read.
    {
        const std::string t = sl::lmu_settings_text(true, kIni, false, "");
        check(has(t, "[Settings.JSON Graphic Options]\n; could not read the file\n"), "lmu: an unreadable file said so");
        check(sl::game_settings_summary(t).find(L"; Settings.JSON could not be read") != std::wstring::npos,
              "lmu: and in the summary");
        check(sl::game_settings_summary("no summary here\n").empty(), "summary: none in a text without one");
    }

    // From the game's folder; other games get no copy.
    {
        const fs::path game = folder / L"Le Mans Ultimate";
        spill(game / L"UserData" / L"Config_DX11_VR.ini", kIni);
        spill(game / L"UserData" / L"player" / L"Settings.JSON", kJson);
        const std::wstring exe = (game / L"Le Mans Ultimate.exe").wstring();
        check(sl::game_settings_text("lmu", exe) == sl::lmu_settings_text(true, kIni, true, kJson),
              "files: LMU's two files read from UserData");
        check(sl::game_settings_text("ace", exe).empty() && sl::game_settings_text(nullptr, exe).empty(),
              "files: no copy for a game without a reader");
    }

    // The report: each zipped session's copy, by its run folder, and the newest launch's summary.
    {
        const fs::path logs = folder / L"logs";
        const std::string older = sl::lmu_settings_text(true, kIni, false, "");
        const std::string newer = sl::lmu_settings_text(true, kIni, true, kJson);
        spill(logs / L"session-20261120-100000.log", "a\n");
        spill(logs / L"run-20261120-100000" / sl::kGameSettingsName, older);
        spill(logs / L"session-20261121-110000.log", "b\n");
        spill(logs / L"run-20261121-110000" / sl::kGameSettingsName, newer);
        spill(logs / L"session-20261122-120000.log", "c\n");  // a launch of a game with no reader: no copy
        fs::create_directories(logs / L"run-20261122-120000");
        FILETIME ft;
        GetSystemTimeAsFileTime(&ft);
        const uint64_t now = (static_cast<uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
        const std::vector<sl::ReportFile> files = sl::report_files({logs.wstring(), L"", L"", L""}, now);
        const auto zipped = [&](const wchar_t* name) {
            return std::any_of(files.begin(), files.end(), [&](const sl::ReportFile& f) { return f.name == name; });
        };
        check(zipped(L"game-settings-20261120-100000.txt") && zipped(L"game-settings-20261121-110000.txt"),
              "report: each session's copy, named by its time");
        check(!zipped(L"game-settings-20261122-120000.txt"), "report: none for a launch that made none");
        check(sl::newest_game_settings(logs.wstring()).empty(), "report: the newest launch made none, no summary");
        fs::remove_all(logs / L"run-20261122-120000", ec);
        const std::wstring summary = sl::newest_game_settings(logs.wstring());
        check(summary == sl::game_settings_summary(newer), "report: the newest launch's summary");
        sl::IssueReport r;
        r.gameSettings = summary;
        check(sl::issue_details(r).find(L"- **Game graphics (last launch):** Post effects High") != std::wstring::npos,
              "report: details.md carries it");
        r.gameSettings.clear();
        check(sl::issue_details(r).find(L"Game graphics") == std::wstring::npos, "report: and no line without one");
    }

    fs::remove_all(folder, ec);
    return test_result();
}
