// Offline check of the launcher's Report an issue (report.h): the files zipped, the zip, the issue's body and link.
#include "launcher_test_kit.h"

int SIM_QVFR_TEST_MAIN() {
    folder = temp_path(L"simqvfr-launcher-report-test");
    std::error_code ec;
    fs::remove_all(folder, ec);
    fs::create_directories(folder);

    // Report an issue (report.h): the files zipped, the zip, the issue's body and link.
    {
        const fs::path logs = folder / L"report" / L"logs";
        fs::create_directories(logs);
        const auto now = fs::file_time_type::clock::now();
        const auto day = std::chrono::hours(24);
        const auto make = [&](const std::wstring& name, fs::file_time_type::duration age, size_t size = 10) {
            write_file((logs / name).wstring(), std::string(size, 'x'));
            fs::last_write_time(logs / name, now - age);
        };
        make(L"session-20261101-100000.log", 30 * day);
        make(L"session-20261103-100000.log", std::chrono::hours(4));
        make(L"session-20261103-110000.log", std::chrono::hours(3));
        make(L"session-20261103-120000.log", std::chrono::hours(2));
        make(L"session-20261103-130000.log", std::chrono::hours(1));
        make(L"sim-qvfr.log", std::chrono::minutes(1));  // the old single log: not a session log
        for (int i = 0; i < 4; ++i)
            make(L"cpu-profile-2026110" + std::to_wstring(i) + L".txt", std::chrono::hours(5 - i));
        make(L"cpu-profile-old.txt", 9 * day);
        make(L"cheeky-snapshot.json", std::chrono::hours(1));
        make(L"hang-101010.dmp", std::chrono::hours(1));
        make(L"hang-090909.dmp", 9 * day);
        make(L"hang-111111.dmp", std::chrono::minutes(5), static_cast<size_t>(kReportMaxDumpBytes) + 1);
        make(L"other.txt", std::chrono::hours(1));
        const std::wstring settingsIni = (folder / L"report" / L"sim-qvfr.ini").wstring();
        const std::wstring reportRuntime = (folder / L"report" / L"runtime.ini").wstring();
        write_file(settingsIni, "[experiment]\r\nmode=views4\r\n");
        write_file(reportRuntime, "[runtime]\r\nrecommended_width=2584\r\n");
        const uint64_t nowTicks = static_cast<uint64_t>(now.time_since_epoch().count());
        const std::vector<ReportFile> files = report_files({logs.wstring(), reportRuntime, settingsIni}, nowTicks);
        std::vector<std::wstring> zipped;
        for (const ReportFile& f : files)
            zipped.push_back(f.name);
        const std::vector<std::wstring> want = {L"sim-qvfr.ini",         L"runtime.ini",
                                                L"session-20261103-130000.log",  L"session-20261103-120000.log",
                                                L"session-20261103-110000.log",  L"cpu-profile-20261103.txt",
                                                L"cpu-profile-20261102.txt", L"cpu-profile-20261101.txt",
                                                L"cheeky-snapshot.json",     L"hang-101010.dmp"};
        check(zipped == want, "report: the inis, the newest three session logs and CPU captures, Cheeky's snapshot, "
                              "a recent dump under the cap; no old, oversized or other files");
        for (const wchar_t* name : {L"session-20261103-100000.log", L"session-20261103-110000.log", L"session-20261103-120000.log",
                                    L"session-20261103-130000.log"})
            fs::remove(logs / name);
        const std::vector<ReportFile> old = report_files({logs.wstring(), reportRuntime, settingsIni}, nowTicks);
        check(std::count_if(old.begin(), old.end(), [](const ReportFile& f) { return f.name == L"session-20261101-100000.log"; }) == 1,
              "report: only an old session log: the newest one still goes in");

        // A launch the loader failed has no session log: the newest run's plan and loader output go in instead,
        // only the end of a long output.
        fs::create_directories(logs / L"run-20261103-120000");
        fs::create_directories(logs / L"run-20261103-130000");
        write_file((logs / L"run-20261103-120000" / L"launch.txt").wstring(), "older launch");
        write_file((logs / L"run-20261103-130000" / L"launch.txt").wstring(), "newest launch");
        write_file((logs / L"run-20261103-130000" / kLoaderOutputName).wstring(),
                   std::string(static_cast<size_t>(kReportLoaderOutputBytes), 'x') + "Error [GENERAL | xrCreateInstance]");
        const std::vector<ReportFile> runs = report_files({logs.wstring(), reportRuntime, settingsIni}, nowTicks);
        const auto entry = [&](const wchar_t* name) {
            const auto it = std::find_if(runs.begin(), runs.end(), [&](const ReportFile& f) { return f.name == name; });
            return it == runs.end() ? nullptr : &*it;
        };
        const ReportFile* runPlan = entry(L"launch.txt");
        const ReportFile* loader = entry(kLoaderOutputName);
        check(runPlan && runPlan->path.find(L"run-20261103-130000") != std::wstring::npos,
              "report: the newest run's launch.txt goes in");
        check(loader && loader->path.empty() && loader->data.size() == kReportLoaderOutputBytes &&
                  loader->data.ends_with("xrCreateInstance]"),
              "report: a long loader output goes in as its last part");

        // The game's crash reports (GameInfo::crashReports): only those during a zipped session, the newest
        // kReportGameCrashes, each as game-crash-<local time>/ with its CrashContext and log redacted.
        {
            const fs::path base = folder / L"report-crashes", crashLogs = base / L"logs", crashes = base / L"Crashes";
            fs::create_directories(crashLogs);
            fs::create_directories(crashes);
            const auto stamp = [](fs::file_time_type t) {
                const uint64_t ticks = static_cast<uint64_t>(t.time_since_epoch().count());
                const FILETIME ft{static_cast<DWORD>(ticks), static_cast<DWORD>(ticks >> 32)};
                SYSTEMTIME utc{}, local{};
                FileTimeToSystemTime(&ft, &utc);
                SystemTimeToTzSpecificLocalTime(nullptr, &utc, &local);
                wchar_t text[32];
                swprintf_s(text, L"%04u%02u%02u-%02u%02u%02u", local.wYear, local.wMonth, local.wDay, local.wHour,
                           local.wMinute, local.wSecond);
                return std::wstring(text);
            };
            using std::chrono::hours, std::chrono::minutes;
            const auto session = [&](fs::file_time_type::duration startAge, fs::file_time_type::duration endAge) {
                const fs::path p = crashLogs / (L"session-" + stamp(now - startAge) + L".log");
                write_file(p.wstring(), "log");
                fs::last_write_time(p, now - endAge);
            };
            session(hours(5), hours(4));
            session(hours(3), hours(2));
            session(minutes(50), minutes(40));
            const std::string gameLog =
                "LogInit: Computer: GAMING-PC\r\nLogInit: User: al\r\nFile [C:/Users/al/AppData/Local/AC2/Saved]\r\n"
                "LogKsGamePlatform: UGamePlatformServices::Init() loaded local account: S76561198000000001 Alice Example "
                "ally#0\r\n"
                "LogOnline: Warning: STEAM: Failed to obtain steam user stats, user: Ally Cat [0x21873D004C0] has no "
                "stats entries\r\n"
                "Check ok with S76561198000000001, Alice Example on GAMING-PC; build 97656119000000001234; also al stays\r\n";
            // The dump holds the context's ids as Unreal copies them: ASCII in another case, and UTF-16.
            const auto utf16 = [](const std::string& s) {
                std::string out;
                for (char ch : s)
                    out += std::string(1, ch) + '\0';
                return out;
            };
            const std::string dump = std::string("MDMP\0\0", 6) + "head EBB98300AA11 mid " + utf16("3b18b8ffee22") + " tail";
            const auto crash = [&](const wchar_t* name, fs::file_time_type::duration age) {
                const fs::path d = crashes / name;
                fs::create_directories(d);
                write_file((d / L"UE4Minidump.dmp").wstring(), dump);
                write_file((d / kCrashContextName).wstring(),
                           "<LoginId>ebb98300aa11</LoginId><MachineId>EBB98300AA11</MachineId><ErrorMessage>boom"
                           "</ErrorMessage><EpicAccountId>3b18b8ffee22</EpicAccountId><UserName></UserName>");
                write_file((d / L"AC2.log").wstring(), gameLog);
                write_file((d / L"CrashReportClient.ini").wstring(), "[x]");
                for (const fs::directory_entry& f : fs::directory_iterator(d))
                    fs::last_write_time(f.path(), now - age);
            };
            crash(L"UE4CC-A", hours(4) - minutes(1));   // just after session 1's last line: in, but the oldest
            crash(L"UE4CC-B", hours(2) + minutes(30));  // during session 2
            crash(L"UE4CC-C", minutes(39));             // just after session 3's last line
            crash(L"UE4CC-D", minutes(20));             // 20 minutes after session 3: the game without Sim-QVFR
            crash(L"UE4CC-E", hours(3) + minutes(30));  // between sessions 1 and 2
            const std::vector<ReportFile> got =
                report_files({crashLogs.wstring(), reportRuntime, settingsIni, crashes.wstring()}, nowTicks);
            std::vector<std::wstring> chosen;  // the crashes' folders in the zip (by their time), in zip order
            int crashEntries = 0;
            const ReportFile* context = nullptr;
            const ReportFile* gameLogEntry = nullptr;
            const ReportFile* dumpEntry = nullptr;
            for (const ReportFile& f : got) {
                if (!f.name.starts_with(L"game-crash-"))
                    continue;
                ++crashEntries;
                if (f.name.ends_with(L"/UE4Minidump.dmp")) {
                    chosen.push_back(f.name.substr(0, f.name.find(L'/')));
                    dumpEntry = dumpEntry ? dumpEntry : &f;
                }
                if (f.name.ends_with(kCrashContextName))
                    context = &f;
                if (f.name.ends_with(L"/AC2.log"))
                    gameLogEntry = &f;
            }
            const std::vector<std::wstring> wantCrashes = {L"game-crash-" + stamp(now - minutes(39)),
                                                           L"game-crash-" + stamp(now - hours(2) - minutes(30))};
            check(chosen == wantCrashes && crashEntries == 6,
                  "report: the game's two newest crashes during zipped sessions (dump, context, log each), named by "
                  "their local time; none between or after sessions, no crash reporter ini");
            const auto has = [](const ReportFile* f, const std::string& text) {
                return f && f->data.find(text) != std::string::npos;
            };
            check(has(context, "<LoginId>redacted</LoginId>") && has(context, "<MachineId>redacted</MachineId>") &&
                      has(context, "<EpicAccountId>redacted</EpicAccountId>") && has(context, "<UserName></UserName>") &&
                      has(context, "<ErrorMessage>boom</ErrorMessage>") && !has(context, "ebb983") &&
                      !has(context, "EBB983") && !has(context, "3b18"),
                  "report: CrashContext without the login, machine and Epic account ids; the error kept");
            check(dumpEntry && dumpEntry->path.empty() && dumpEntry->data.size() == dump.size() &&
                      dumpEntry->data.starts_with(std::string("MDMP\0\0", 6)) && dumpEntry->data.ends_with(" tail") &&
                      has(dumpEntry, "head 000000000000 mid ") && !has(dumpEntry, "EBB98300AA11") &&
                      !has(dumpEntry, utf16("3b18b8ffee22")) && has(dumpEntry, utf16("000000000000")),
                  "report: the dump with the context's ids zeroed, ASCII in any case and UTF-16, its size kept");
            check(crash_context_ids("<LoginId></LoginId><MachineId>short</MachineId>").empty(),
                  "report: no ids from empty or short values");
            check(gameLogEntry && !has(gameLogEntry, "GAMING-PC") && !has(gameLogEntry, "Users/al/") &&
                      has(gameLogEntry, "C:/Users/<user>/AppData") && has(gameLogEntry, "LogInit: User: <user>") &&
                      !has(gameLogEntry, "Alice Example") && !has(gameLogEntry, "ally#0") &&
                      !has(gameLogEntry, "Ally Cat [") && has(gameLogEntry, "user: <steam name> [0x21873D004C0]") &&
                      !has(gameLogEntry, "76561198000000001") && has(gameLogEntry, "with S<steam id>, <account> on <computer>") &&
                      has(gameLogEntry, "build 97656119000000001234") && has(gameLogEntry, "also al stays"),
                  "report: the game's log without the computer, user, account and Steam names or SteamID64s, "
                  "wherever they appear; longer numbers and a short user name outside paths kept");
            check(redact_game_log("no names here\r\n") == "no names here\r\n", "report: a log with no names is unchanged");
        }

        check(crc32("123456789", 9) == 0xcbf43926u, "report: CRC-32 check value");
        const std::vector<ReportFile> zipEntries = {{L"runtime.ini", reportRuntime}, {L"issue.md", L"", "# Problem \xc3\xa9"}};
        const std::wstring zip = (folder / L"report" / L"report.zip").wstring();
        const std::wstring zipError = write_zip(zip, zipEntries);
        std::string bytes;
        if (FILE* f = nullptr; _wfopen_s(&f, zip.c_str(), L"rb") == 0 && f) {
            char buffer[4096];
            for (size_t n; (n = std::fread(buffer, 1, sizeof(buffer), f)) > 0;)
                bytes.append(buffer, n);
            std::fclose(f);
        }
        const auto u16 = [&](size_t at) {
            return static_cast<unsigned char>(bytes[at]) | static_cast<unsigned char>(bytes[at + 1]) << 8;
        };
        const size_t eocd = bytes.size() >= 22 ? bytes.size() - 22 : 0;
        check(zipError.empty() && bytes.compare(0, 4, "PK\x03\x04") == 0 && bytes.compare(eocd, 4, "PK\x05\x06") == 0 &&
                  u16(eocd + 10) == 2 && bytes.find("# Problem \xc3\xa9") != std::string::npos &&
                  bytes.find("recommended_width=2584") != std::string::npos,
              "report: a stored zip of both entries, its end record counting them");
        check(!write_zip((folder / L"missing" / L"x.zip").wstring(), zipEntries).empty(), "report: an unwritable zip says why");

        // The user's and computer's names out of every zipped file (launch.txt, logs, the ini, hang dumps).
        {
            const Identity who{L"C:\\Users\\al", L"al", L"GAMING-PC"};
            const auto utf16 = [](const std::string& s) {
                std::string out;
                for (char ch : s)
                    out += std::string(1, ch) + '\0';
                return out;
            };
            const std::string idText =
                "game: C:\\Users\\al\\Games\\x.exe; c:/users/AL/y; {\"p\": \"C:\\\\Users\\\\al\\\\z\"}; D:\\Users\\al\\w; "
                "USERNAME=al COMPUTERNAME=GAMING-PC on gaming-pc; \\\\Gaming-PC\\share; also al stays; C:\\Users\\alice\\ "
                "kept; GAMING-PCS kept";
            const std::string idWant =
                "game: C:\\Users\\<user>\\Games\\x.exe; c:/users/<user>/y; {\"p\": \"C:\\\\Users\\\\<user>\\\\z\"}; "
                "D:\\Users\\<user>\\w; USERNAME=<user> COMPUTERNAME=<computer> on <computer>; \\\\<computer>\\share; also al "
                "stays; C:\\Users\\alice\\ kept; GAMING-PCS kept";
            check(redact_identity(idText, who, false) == idWant, "report: user and computer names out of text, in any case");
            check(redact_identity(utf16("log C:\\USERS\\al\\x and GAMING-PC"), who, false) ==
                      utf16("log C:\\USERS\\<user>\\x and <computer>"),
                  "report: and out of UTF-16 text");
            const Identity longName{L"C:\\Users\\alice_000", L"alice", L"PC"};
            check(redact_identity("C:\\Users\\alice_000\\x by alice, malice kept, on PC", longName, false) ==
                      "C:\\Users\\<user>\\x by <user>, malice kept, on PC",
                  "report: the profile folder's own name; a longer user name anywhere as a word, a short computer name kept");
            const std::string dump = std::string("MDMP\0\0", 6) + "C:\\Users\\al\\d " + utf16("c:\\users\\AL\\e") +
                                     " COMPUTERNAME=GAMING-PC " + utf16("USERNAME=al");
            const std::string redacted = redact_identity(dump, who, true);
            check(redacted.size() == dump.size() && redacted.starts_with(std::string("MDMP\0\0", 6)) &&
                      redacted.find("C:\\Users\\00\\d ") != std::string::npos &&
                      redacted.find(utf16("c:\\users\\00\\e")) != std::string::npos &&
                      redacted.find("COMPUTERNAME=000000000 ") != std::string::npos &&
                      redacted.find(utf16("USERNAME=00")) != std::string::npos,
                  "report: a dump's names zeroed, ASCII and UTF-16, its size kept");
            check(redact_identity("nothing here", who, false) == "nothing here" &&
                      redact_identity("C:\\Users\\al\\x", Identity{}, false) == "C:\\Users\\al\\x",
                  "report: text without the names, or no names known, unchanged");
            const std::wstring idPlan = (folder / L"report" / L"plan.txt").wstring();
            write_file(idPlan, "layer log: C:\\Users\\al\\Sim-QVFR\\logs\\session.log on GAMING-PC");
            const std::wstring redactedZip = (folder / L"report" / L"redacted.zip").wstring();
            const std::vector<ReportFile> idEntries = {
                {L"launch.txt", idPlan}, {L"hang-1.dmp", L"", std::string("MDMP") + utf16("C:\\Users\\al\\x"), 0}};
            std::string idZipped;
            const bool wrote = write_zip(redactedZip, idEntries, who).empty() && read_file(redactedZip, idZipped);
            check(wrote && idZipped.find("C:\\Users\\<user>\\Sim-QVFR\\logs\\session.log on <computer>") != std::string::npos &&
                      idZipped.find(utf16("C:\\Users\\00\\x")) != std::string::npos && idZipped.find("GAMING-PC") == std::string::npos &&
                      idZipped.find("Users\\al\\") == std::string::npos && idZipped.find(utf16("Users\\al")) == std::string::npos,
                  "report: the zip's files and dumps go in without the names");
        }

        // The launcher's text reads are capped (a registration can name any file); its DLL checks are not.
        {
            const std::wstring big = (folder / L"report" / L"big.json").wstring();
            write_file(big, std::string(static_cast<size_t>(kMaxReadBytes) + 1, ' '));
            const std::wstring fits = (folder / L"report" / L"fits.json").wstring();
            write_file(fits, std::string(static_cast<size_t>(kMaxReadBytes), ' '));
            std::string got;
            const bool refused = !read_file(big, got) && got.empty();
            const bool uncapped = read_file(big, got, 0) && got.size() == kMaxReadBytes + 1;
            check(refused && uncapped && read_file(fits, got) && got.size() == kMaxReadBytes,
                  "read_file: a file over 1 MB refused, unless uncapped; one of 1 MB read");
            const std::wstring self = module_path();
            check(!self.empty() && fs::exists(self) && _wcsicmp(fs::path(self).extension().c_str(), L".exe") == 0,
                  "module_path: this exe's full path");
        }

        check(windows_name(26100, L"24H2", 2314) == L"Windows 11 24H2 (build 26100.2314)" &&
                  windows_name(19045, L"22H2", 0) == L"Windows 10 22H2 (build 19045)",
              "report: Windows 11 from the build number");
        const uint64_t umd = (32ull << 48) | (15ull << 16) | 6094;
        check(gpu_driver_text(0x10de, umd) == L"560.94 (32.0.15.6094)" && gpu_driver_text(0x1002, umd) == L"32.0.15.6094",
              "report: NVIDIA's driver number from the Windows version");
        check(openxr_runtime_name(R"({"runtime": {"name": "SteamVR", "library_path": "x.dll"}})", L"C:\\a\\steamxr.json") ==
                      L"SteamVR" &&
                  openxr_runtime_name(R"({"runtime": {"library_path": "x.dll"}})", L"C:\\a\\pimax.json") == L"pimax.json",
              "report: the OpenXR runtime's name, else its manifest's file");
        check(url_encode(L"a b&\u00e9\n~") == "a%20b%26%C3%A9%0A~", "report: URL encoding of UTF-8");

        IssueReport report;
        report.version = L"1.2.3";
        report.zipName = L"sim-qvfr-report-20261103-120000.zip";
        report.system = {L"AMD Ryzen 7 9800X3D, 16 threads", L"NVIDIA GeForce RTX 5090, 32 GB", L"581.57 (32.0.15.8157)",
                         L"Windows 11 24H2 (build 26100.2314)", L"Pimax OpenXR", 64};
        report.runtime.width = 2584;
        report.runtime.height = 2160;
        report.runtime.eyeTracker = EyeTracker::Detected;
        SessionInfo& session = report.runtime.session;
        session.fps = 89.9;
        session.frameP50 = 11.1;
        session.frameP90 = 12.4;
        session.limited = L"GPU";
        session.log = L"C:\\Users\\someone\\Sim-QVFR\\logs\\session-20261103-120000.log";
        session.threads = {{L"GameThread", 71}, {L"RHIThread", 40}};
        const std::wstring details = issue_details(report);
        const auto has = [&](const wchar_t* text) { return details.find(text) != std::wstring::npos; };
        check(has(L"RTX 5090") && has(L"581.57") && has(L"Pimax OpenXR") && has(L"64 GB") && has(L"2584×2160") &&
                  has(L"eye tracked") && has(L"89.9 fps") && has(L"GameThread 71%") && has(L"Limited by: GPU") &&
                  has(L"detected") && has(L"Sim-QVFR 1.2.3"),
              "report: the details list version, hardware, headset, settings and the last session");
        check(!has(L"someone") && has(L"Log: session-20261103-120000.log"),
              "report: the session log by name, not by a path with the user's name");

        // The likely causes: none for a healthy session, each known one when its sign shows.
        Settings healthy;
        RuntimeInfo fine = report.runtime;
        fine.session.limited = L"Neither (headroom)";
        check(likely_causes(healthy, fine).empty() && likely_causes(healthy, RuntimeInfo{}).empty(),
              "causes: none for a session with headroom, or before any session");
        const auto titles = [](const std::vector<Cause>& causes) {
            std::wstring all;
            for (const Cause& c : causes)
                all += c.title + L"|";
            return all;
        };
        RuntimeInfo troubled = fine;
        troubled.eyeTracker = EyeTracker::Missing;
        troubled.vramUsedMb = 11000;
        troubled.vramBudgetMb = 11600;
        troubled.cheekyStatus = L"missing";
        troubled.session.layersBelow = L"XR_APILAYER_MBUCCHIA_toolkit";
        troubled.session.limited = L"Pacing (frames late, neither saturated)";
        troubled.session.latePercent = 25;
        Settings dlss;
        dlss.cheeky.load = true;
        const std::wstring found = titles(likely_causes(dlss, troubled));
        check(found.find(L"tracker sent nothing") != std::wstring::npos && found.find(L"Cheeky missing") != std::wstring::npos &&
                  found.find(L"VRAM was nearly full (95%") != std::wstring::npos &&
                  found.find(L"OpenXR Toolkit") != std::wstring::npos && found.find(L"25% of frames were late") != std::wstring::npos,
              "causes: no eye tracker samples, Cheeky missing, VRAM near full, OpenXR Toolkit, late frames");
        Settings fixed;
        fixed.eyeTracking = false;
        check(titles(likely_causes(fixed, troubled)).find(L"tracker") == std::wstring::npos &&
                  titles(likely_causes(healthy, troubled)).find(L"Cheeky") == std::wstring::npos,
              "causes: no tracker warning in Fixed, no Cheeky one with Foveated DLSS off");
        RuntimeInfo gpu = fine, cpu = fine;
        gpu.session.limited = L"GPU";
        gpu.session.gpuBusy = 98;
        cpu.session.threads = {{L"GameThread", 97}};
        check(titles(likely_causes(healthy, gpu)).find(L"GPU was busy all the time (98%)") != std::wstring::npos &&
                  titles(likely_causes(healthy, cpu)).find(L"CPU work limited") != std::wstring::npos,
              "causes: a busy GPU, and ACE's game thread near a whole core");
        RuntimeInfo halfRate = fine;
        halfRate.session.limited = L"Pacing (the runtime ran the game at half rate)";
        halfRate.session.halfRatePercent = 8;
        halfRate.session.latePercent = 9;
        const std::wstring halfFound = titles(likely_causes(healthy, halfRate));
        check(halfFound.find(L"half rate for 8% of the session") != std::wstring::npos &&
                  halfFound.find(L"were late") == std::wstring::npos &&
                  session_summary_text(halfRate).find(L"Runtime at half rate: 8.0%") != std::wstring::npos,
              "causes: the runtime's half rate (Varjo), in place of the plain late-frames one, and in the summary");

        // The runtime composing with a focus region smaller than the focus area: named, with the two sizes; not when
        // it is as large (within a point), nor when the runtime didn't compose.
        RuntimeInfo pimax = fine;
        pimax.quadViewsUsed = true;
        pimax.quadFocusWidth = 20;
        pimax.quadFocusHeight = 20;
        Settings wide;
        wide.width = 30;
        wide.height = 21;
        Settings matched;
        matched.width = 21;
        matched.height = 20;
        RuntimeInfo stereo = pimax;
        stereo.quadViewsUsed = false;
        check(titles(likely_causes(wide, pimax)).find(L"focus area (30% × 21%) is larger than the runtime's own focus "
                                                      L"region (20% × 20%)") != std::wstring::npos &&
                  titles(likely_causes(matched, pimax)).find(L"focus region") == std::wstring::npos &&
                  titles(likely_causes(wide, stereo)).find(L"focus region") == std::wstring::npos,
              "causes: the focus area larger than the runtime's own focus region, when the runtime composed");

        // The title: the kind, then the description's first line, cut at a word.
        report.kind = 2;  // Performance
        report.description = L"  Stutters every few seconds on Monza  \nSecond line";
        check(issue_title(report) == L"[Performance] Stutters every few seconds on Monza", "title: kind and first line");
        report.description = L"The focus area jumps around whenever I look at the left mirror in the hairpin at Monza";
        check(issue_title(report) == L"[Performance] The focus area jumps around whenever I look at the left mirror in the…",
              "title: a long line cut at a word");
        report.kind = 0;
        report.description.clear();
        check(issue_title(report) == L"[Problem] Sim-QVFR 1.2.3: ", "title: Not sure and no description: Problem, the version");

        report.description = L"The focus area jumps & flickers.";
        report.causes = likely_causes(dlss, troubled);
        const std::wstring body = issue_body(report);
        const auto at = [&](const wchar_t* text) { return body.find(text); };
        check(body.starts_with(L"### What happened?\n\nThe focus area jumps & flickers.") &&
                  at(L"### Steps to reproduce") < at(L"### When does it happen?") &&
                  at(L"### When does it happen?") < at(L"### Screenshots and logs") &&
                  at(report.zipName.c_str()) < at(L"### Launcher checks") &&
                  at(L"### Launcher checks") < at(L"OpenXR Toolkit") && at(L"OpenXR Toolkit") < at(L"### Collected by the launcher") &&
                  at(L"### Collected by the launcher") < at(L"RTX 5090") && at(L"RTX 5090") != std::wstring::npos,
              "body: the description, the form's prompts, the zip, the checks, then the collected details");
        IssueReport bare = report;
        bare.description.clear();
        bare.causes.clear();
        const std::wstring bareBody = issue_body(bare);
        check(bareBody.starts_with(L"### What happened?\n\n<!--") && bareBody.find(L"None found.") != std::wstring::npos,
              "body: a prompt without a description, and no checks found");

        const std::vector<IssueField> fields = issue_fields(report);
        check(fields.size() == 4 && fields[0].id == "what" && fields[0].value == report.description && fields[1].id == "files" &&
                  fields[1].value.find(report.zipName) != std::wstring::npos && fields[2].id == "checks" &&
                  fields[2].value.find(L"OpenXR Toolkit") != std::wstring::npos && fields[3].id == "details" &&
                  fields[3].value == issue_details(report),
              "fields: what, files (the zip), checks and details, by the forms' ids");
        check(std::string(kIssueTemplates[0]) == "bug.yml" && std::string(kIssueTemplates[2]) == "performance.yml" &&
                  std::wstring(kIssueKinds[2]) == L"Performance",
              "forms: Not sure is bug.yml, each kind its own form");
        // Every form in .github/ISSUE_TEMPLATE exists and has the fields the launcher fills.
        const fs::path forms = fs::path(__FILE__).parent_path().parent_path() / L".github" / L"ISSUE_TEMPLATE";
        bool formsOk = true;
        for (const char* form : kIssueTemplates) {
            std::string yml;
            if (FILE* f = nullptr; _wfopen_s(&f, (forms / form).c_str(), L"rb") == 0 && f) {
                char buffer[4096];
                for (size_t n; (n = std::fread(buffer, 1, sizeof(buffer), f)) > 0;)
                    yml.append(buffer, n);
                std::fclose(f);
            }
            for (const IssueField& field : fields) {  // "id: what" ending its line (LF or CRLF)
                const size_t idAt = yml.find("id: " + field.id);
                const size_t end = idAt == std::string::npos ? yml.size() : idAt + 4 + field.id.size();
                formsOk = formsOk && end < yml.size() && (yml[end] == '\n' || yml[end] == '\r');
            }
        }
        check(formsOk, "forms: each kind's form exists with what, files, checks and details");

        const std::string url = issue_url(kIssueRepo, kIssueTemplates[0], L"[Problem] Sim-QVFR 1.2.3: ", fields);
        check(url.starts_with("https://github.com/Williem3/Sim-QVFR/issues/new?template=bug.yml&title=%5BProblem%5D%20Sim-QVFR%201.2.3%3A%20&what=The%20focus%20area%20jumps%20%26%20flickers.&files=") &&
                  url.find("&checks=") != std::string::npos && url.find("&details=") != std::string::npos &&
                  url.size() <= kIssueUrlMax && url.find("Trimmed") == std::string::npos,
              "report: the form's link with each field, whole when it fits");
        std::vector<IssueField> empty = fields;
        empty[0].value.clear();
        check(issue_url(L"someone/test", "crash.yml", L"t", empty).starts_with("https://github.com/someone/test/issues/new?template=crash.yml&") &&
                  issue_url(kIssueRepo, "bug.yml", L"t", empty).find("&what=") == std::string::npos,
              "report: another repository and form; empty fields left out");
        std::vector<IssueField> longFields = fields;
        for (int i = 0; i < 400; ++i)  // inside the session's ``` block, before its closing fence
            longFields[3].value.insert(longFields[3].value.size() - 4, L"Busiest thread line " + std::to_wstring(i) + L"\n");
        const std::string trimmed = issue_url(kIssueRepo, "bug.yml", L"t", longFields, 4000);
        size_t fences = 0;
        for (size_t f = trimmed.find("%60%60%60"); f != std::string::npos; f = trimmed.find("%60%60%60", f + 9))
            ++fences;
        check(trimmed.size() <= 4000 && trimmed.find("&what=The%20focus") != std::string::npos &&
                  trimmed.find("OpenXR%20Toolkit") != std::string::npos && trimmed.find("Trimmed") != std::string::npos &&
                  fences % 2 == 0,
              "report: long details are cut from their end to fit, their code block closed, with a note");
        const std::string tiny = issue_url(kIssueRepo, "bug.yml", L"t", longFields, 1500);
        check(tiny.size() <= 1500 && tiny.find("&what=The%20focus") != std::string::npos && tiny.find("&details=") == std::string::npos,
              "report: with less room the details go, then the checks, the description last");
    }

    fs::remove_all(folder, ec);
    return test_result();
}
