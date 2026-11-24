// Offline check of the launcher's update check (update_check.h) and install (install.h): versions compared, the
// newest release, the hash lists, a staged release installed into a folder and put back on a failure.
#include "launcher_test_kit.h"

int SIM_QVFR_TEST_MAIN() {
    folder = temp_path(L"simqvfr-launcher-install-test");
    std::error_code ec;
    fs::remove_all(folder, ec);
    fs::create_directories(folder);

    // The update check (update_check.h): versions compared by their numbers, the newest release in GitHub's list.
    {
        check(compare_versions(L"v2026.11.3.3-alpha", L"2026.11.3.3-alpha (1a2b3c4)") == 0 &&
                  compare_versions(L"v2026.11.4.1-alpha", L"2026.11.3.3-alpha (1a2b3c4)") > 0 &&
                  compare_versions(L"v2026.11.3.10-alpha", L"v2026.11.3.9-alpha") > 0 &&
                  compare_versions(L"v2026.11.3", L"2026.11.3.1") < 0 && compare_versions(L"v2027.1.1.1", L"dev") == 0,
              "update: versions by their numbers, a suffix or commit ignored, 10 after 9");
        check(known_version(L"2026.11.3.3-alpha (abc)") && !known_version(L"dev") && !known_version(L""),
              "update: a development build has no version to compare");
        check(skipped_release(L"v1.2.0-beta", L"1.2.0-beta") && skipped_release(L"v1.2.0-beta", L"v1.2.0-beta") &&
                  !skipped_release(L"v1.2.1-beta", L"1.2.0-beta") && !skipped_release(L"v1.2.0-beta", L"") &&
                  !skipped_release(L"dev", L"dev"),
              "update: a skipped release is that version only; a newer one, or none skipped, is offered");
        const auto zip = [](const char* name) {
            return std::string(R"("assets": [{"name": ")") + name +
                   R"(", "size": 3, "digest": "sha256:00", "browser_download_url": "u"}])";
        };
        const std::string releases = std::string("[") +
            R"({"tag_name": "v1.1.0-sim-qvfr-beta", "html_url": "draft", "draft": true, )" + zip("sim-qvfr-1.1.0.zip") + "}," +
            R"({"tag_name": "v1.0.0-sim-qvfr-beta", "html_url": "a", "draft": false, "prerelease": true, )" +
            zip("sim-qvfr-1.0.0.zip") + "}," +
            R"({"tag_name": "v1.0.1-sim-qvfr-beta", "html_url": "https://github.com/Williem3/Sim-QVFR/releases/tag/v1.0.1-sim-qvfr-beta",
                "draft": false, "prerelease": true, )" + zip("sim-qvfr-1.0.1.zip") + "}," +
            R"({"tag_name": "v2026.11.4.1-alpha", "html_url": "old", "draft": false, "prerelease": true, )" +
            zip("ace-quadview-2026.11.4.1-alpha.zip") + "}," +
            R"({"tag_name": "v1.2.0", "html_url": "notes only", "draft": false})" + "]";
        Release release;
        check(newest_release(releases, release) && release.tag == L"v1.0.1-sim-qvfr-beta" &&
                  release.url == L"https://github.com/Williem3/Sim-QVFR/releases/tag/v1.0.1-sim-qvfr-beta",
              "update: the highest Sim-QVFR release that isn't a draft, pre-releases counted; the old ace-quadview "
              "alphas and releases without a package ignored");
        check(!newest_release("[]", release) && !newest_release("{\"message\": \"rate limited\"}", release) &&
                  !newest_release("not json", release) && !newest_release(R"([{"tag_name": "v1", "draft": true}])", release),
              "update: no release in an empty list, an error answer, garbage or drafts only");

        // The nightly channel (update_check.h Channel): each launcher looks only at its own channel's packages, and a
        // nightly's date makes a later nightly of the same release newer.
        check(compare_versions(L"v1.3.0-nightly.20261125", L"1.3.0-nightly.20261124 (abc1234)") > 0 &&
                  compare_versions(L"v1.3.0-nightly.20261124", L"1.3.0-nightly.20261124 (abc1234)") == 0 &&
                  compare_versions(L"v1.4.0-nightly.20261101", L"1.3.0-nightly.20261124") > 0 &&
                  compare_versions(L"v1.3.0-beta", L"1.3.0") == 0 && compare_versions(L"1.3.0 (abc)-nightly.5", L"1.3.0") == 0,
              "update: a nightly's date counts after the release's numbers; only right after them");
        check(channel_named("nightly") == Channel::Nightly && channel_named("Nightly") == Channel::Nightly &&
                  channel_named("stable") == Channel::Stable && channel_named("") == Channel::Stable &&
                  channel_named(nullptr) == Channel::Stable,
              "update: the build's channel from SIM_QVFR_CHANNEL; anything but nightly is stable");
        const std::string mixed = std::string("[") +
            R"({"tag_name": "v1.3.0-nightly.20261125", "html_url": "n2", "draft": false, "prerelease": true, )" +
            zip("sqvfr-nightly-1.3.0.20261125.zip") + "}," +
            R"({"tag_name": "v1.3.0-nightly.20261124", "html_url": "n1", "draft": false, "prerelease": true, )" +
            zip("sqvfr-nightly-1.3.0.20261124.zip") + "}," +
            R"({"tag_name": "v1.2.3-beta", "html_url": "s1", "draft": false, "prerelease": true, )" +
            zip("sim-qvfr-1.2.3-beta.zip") + "}]";
        Release stable, nightly;
        check(newest_release(mixed, stable) && stable.tag == L"v1.2.3-beta" &&
                  newest_release(mixed, stable, Channel::Stable) && stable.tag == L"v1.2.3-beta",
              "update: a stable launcher never picks a nightly, however new");
        check(newest_release(mixed, nightly, Channel::Nightly) && nightly.tag == L"v1.3.0-nightly.20261125" &&
                  nightly.url == L"n2",
              "update: a nightly launcher picks the newest nightly and never a stable package");
        check(!newest_release(releases, nightly, Channel::Nightly),
              "update: a nightly launcher finds nothing in a list of stable packages only");
    }

    // Install (install.h): the hash lists, what to replace and keep, the downloads trusted, a staged release
    // installed into a folder (the user's settings and changed files kept, a backup), and put back on a failure.
    {
        const std::string hexA(64, 'a'), hexB(64, 'b'), hexC(64, 'c');
        const auto list = parse_sha256_list(std::string(64, 'A') + "  layer/sim-qvfr.dll\r\n" + hexB + "  README.txt\n" +
                                            hexC + "  ../evil.dll\n" + hexC + "  C:/evil.dll\n" + "nothex  x\n" + hexC +
                                            "  /root.dll\n");
        check(list.size() == 2 && list.at(L"layer\\sim-qvfr.dll") == hexA && list.at(L"README.txt") == hexB,
              "install: SHA256.txt read, lowercase, backslashes; no path out of the folder");

        const std::map<std::wstring, std::string> release = {
            {L"sim-qvfr-launcher.exe", hexA}, {L"sim-qvfr.ini", hexA}, {L"layer\\helmet\\top.png", hexB},
            {L"layer\\helmet\\visor.png", hexB},     {L"README.txt", hexC},       {L"NEW.txt", hexA}};
        const std::map<std::wstring, std::string> oldRelease = {
            {L"sim-qvfr-launcher.exe", hexC}, {L"layer\\helmet\\top.png", hexC}, {L"layer\\helmet\\visor.png", hexC}};
        const std::map<std::wstring, std::string> installed = {
            {L"sim-qvfr-launcher.exe", hexC}, {L"sim-qvfr.ini", hexB},  // the user's settings
            {L"layer\\helmet\\top.png", hexC},                                   // as the old release had it
            {L"layer\\helmet\\visor.png", hexA},                                 // the user's own image
            {L"README.txt", hexC}};                                            // already the new one
        const auto steps = plan_install(release, oldRelease, [&](const std::wstring& p) {
            const auto it = installed.find(p);
            return it == installed.end() ? std::string() : it->second;
        });
        const auto action = [&](const wchar_t* p) {
            for (const InstallStep& s : steps)
                if (s.path == p)
                    return s.action;
            return InstallAction::Same;
        };
        check(action(L"sim-qvfr-launcher.exe") == InstallAction::Replace && action(L"sim-qvfr.ini") == InstallAction::KeepSettings &&
                  action(L"layer\\helmet\\top.png") == InstallAction::Replace && action(L"layer\\helmet\\visor.png") == InstallAction::KeepChanged &&
                  action(L"README.txt") == InstallAction::Same && action(L"NEW.txt") == InstallAction::Add,
              "install: replace the old release's files, keep the settings and the user's own, add new ones");
        const auto noList = plan_install(release, {}, [&](const std::wstring& p) {
            const auto it = installed.find(p);
            return it == installed.end() ? std::string() : it->second;
        });
        bool helmetKept = false, exeReplaced = false;
        for (const InstallStep& s : noList) {
            helmetKept |= s.path == L"layer\\helmet\\top.png" && s.action == InstallAction::KeepChanged;
            exeReplaced |= s.path == L"sim-qvfr-launcher.exe" && s.action == InstallAction::Replace;
        }
        check(helmetKept && exeReplaced, "install: without the old release's list, helmet images stay and program files go");

        check(trusted_download("https://github.com/Williem3/Sim-QVFR/releases/download/v2026.11.4.1-alpha/sim-qvfr-2026.11.4.1-alpha.zip") &&
                  !trusted_download("http://github.com/Williem3/Sim-QVFR/releases/download/v1/a.zip") &&
                  !trusted_download("https://github.com/someone/else/releases/download/v1/a.zip") &&
                  !trusted_download("https://github.com/Williem3/Sim-QVFR/releases/download/../../evil.zip") &&
                  !trusted_download("https://github.com/Williem3/Sim-QVFR/releases/download/"),
              "install: only the repository's release downloads, over HTTPS");
        check(trusted_release_page(L"https://github.com/Williem3/Sim-QVFR/releases/tag/v1.1.1-beta") &&
                  trusted_release_page(L"https://github.com/Williem3/Sim-QVFR/releases/latest") &&
                  !trusted_release_page(L"https://github.com/Williem3/Sim-QVFR/releases/") &&
                  !trusted_release_page(L"https://github.com/someone/else/releases/tag/v1") &&
                  !trusted_release_page(L"http://github.com/Williem3/Sim-QVFR/releases/tag/v1") &&
                  !trusted_release_page(L"https://github.com/Williem3/Sim-QVFR/releases/../../evil") &&
                  !trusted_release_page(L"https://github.com/Williem3/Sim-QVFR/releases/tag/v1\r\nx") &&
                  !trusted_release_page(L"https://github.com/Williem3/Sim-QVFR/releases/tag/v1 \"x\"") &&
                  !trusted_release_page(L"https://github.com/Williem3/Sim-QVFR/releases/tag/v1%00") &&
                  !trusted_release_page(L"file:///C:/Windows/System32/calc.exe"),
              "update: only the repository's release pages are opened");

        const std::string withAsset = R"([{"tag_name": "v2026.11.4.1-alpha", "html_url": "https://github.com/x", "draft": false,
            "assets": [{"name": "notes.txt", "size": 5, "digest": "sha256:00"},
                       {"name": "sim-qvfr-2026.11.4.1-alpha.zip", "size": 12066130,
                        "digest": "sha256:6306eb9b219a7d78685f96fc82da6180c6bb53f88be44fea5b7400f1e482c11e",
                        "browser_download_url": "https://github.com/Williem3/Sim-QVFR/releases/download/v2026.11.4.1-alpha/sim-qvfr-2026.11.4.1-alpha.zip"}]}])";
        Release r;
        check(newest_release(withAsset, r) && r.zipSize == 12066130 &&
                  r.zipSha256 == "6306eb9b219a7d78685f96fc82da6180c6bb53f88be44fea5b7400f1e482c11e" && trusted_download(r.zipUrl),
              "install: the release's zip, its size and SHA-256 from GitHub's list");
        Release noDigest;
        check(!newest_release(R"([{"tag_name": "v1", "assets": [{"name": "sim-qvfr-1.zip", "size": 3, "browser_download_url": "u"}]}])",
                              noDigest),
              "install: a zip without a digest isn't offered to install");

        // A staged release over an installed one, on disk.
        const fs::path root = folder / "install", app = root / "app", staged = root / "staged";
        const auto put = [](const fs::path& p, const std::string& text) {
            fs::create_directories(p.parent_path());
            write_file(p.wstring(), text);
        };
        const auto hash_of = [](const fs::path& p) { return simqvfr::sha256_file(p.c_str()); };
        const auto list_of = [&](const fs::path& dir, std::vector<const char*> files) {
            std::string text;
            for (const char* f : files)
                text += hash_of(dir / f) + "  " + std::string(f) + "\r\n";
            write_file((dir / "SHA256.txt").wstring(), text);
        };
        put(app / "sim-qvfr-launcher.exe", "old launcher");
        put(app / "layer/sim-qvfr.dll", "old layer");
        put(app / "layer/helmet/top.png", "old top");
        put(app / "layer/helmet/visor.png", "old visor");
        put(app / "sim-qvfr.ini", "[shipped]");
        put(app / "VERSION.txt", "Sim-QVFR 1");
        list_of(app, {"sim-qvfr-launcher.exe", "layer/sim-qvfr.dll", "layer/helmet/top.png", "layer/helmet/visor.png",
                      "sim-qvfr.ini", "VERSION.txt"});
        put(app / "sim-qvfr.ini", "[mine]");          // the user's settings since
        put(app / "layer/helmet/visor.png", "my visor");    // and their own image
        put(app / "profiles/Night.ini", "[profile]");      // not in any release
        put(staged / "sim-qvfr-launcher.exe", "new launcher");
        put(staged / "layer/sim-qvfr.dll", "new layer");
        put(staged / "layer/helmet/top.png", "new top");
        put(staged / "layer/helmet/visor.png", "new visor");
        put(staged / "sim-qvfr.ini", "[shipped 2]");
        put(staged / "VERSION.txt", "Sim-QVFR 2");
        put(staged / "README.txt", "new readme");
        list_of(staged, {"sim-qvfr-launcher.exe", "layer/sim-qvfr.dll", "layer/helmet/top.png", "layer/helmet/visor.png",
                         "sim-qvfr.ini", "VERSION.txt", "README.txt"});
        check(packaged_install(app.wstring()) && !packaged_install(staged.parent_path().wstring()),
              "install: a packaged install has VERSION.txt and SHA256.txt");

        // A file in use (opened without sharing) can't be moved aside: nothing changes.
        std::string readBack;
        HANDLE locked = CreateFileW((app / "layer/sim-qvfr.dll").c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        const InstallResult failed = install_staged(app.wstring(), staged.wstring(), L"1 (abc)", [](const InstallProgress&) {});
        CloseHandle(locked);
        check(!failed.ok && failed.error.find(L"sim-qvfr.dll") != std::wstring::npos &&
                  read_file((app / "sim-qvfr-launcher.exe").wstring(), readBack) && readBack == "old launcher" &&
                  read_file((app / "layer/sim-qvfr.dll").wstring(), readBack) && readBack == "old layer" &&
                  !fs::exists(app / "README.txt"),
              "install: a file in use stops it, and the files already replaced go back");

        int steps2 = 0;
        const InstallResult done = install_staged(app.wstring(), staged.wstring(), L"1 (abc)", [&](const InstallProgress&) { ++steps2; });
        std::string launcher, layer, visor, ini, top, backupLauncher, readme;
        read_file((app / "sim-qvfr-launcher.exe").wstring(), launcher);
        read_file((app / "layer/sim-qvfr.dll").wstring(), layer);
        read_file((app / "layer/helmet/visor.png").wstring(), visor);
        read_file((app / "layer/helmet/top.png").wstring(), top);
        read_file((app / "sim-qvfr.ini").wstring(), ini);
        read_file((app / "README.txt").wstring(), readme);
        read_file((app / "update/backup-1/sim-qvfr-launcher.exe").wstring(), backupLauncher);
        check(done.ok && launcher == "new launcher" && layer == "new layer" && top == "new top" && readme == "new readme" &&
                  steps2 > 0,
              "install: the new release's program files in place, new ones added");
        check(ini == "[mine]" && visor == "my visor" && fs::exists(app / "profiles/Night.ini") && done.kept == 2,
              "install: the user's settings, own helmet image and profiles kept");
        check(backupLauncher == "old launcher" && done.backup == (app / "update" / "backup-1").wstring() &&
                  parse_sha256_list((read_file((app / "SHA256.txt").wstring(), readBack), readBack)).count(L"README.txt"),
              "install: the old files in update\\backup-<version>, the new SHA256.txt in place");

        fs::create_directories(app / "update/staged");
        write_file((app / "update/release.zip").wstring(), "zip");
        fs::create_directories(app / "update/backup-0");
        Sleep(20);
        fs::last_write_time(app / "update/backup-1", fs::file_time_type::clock::now());
        clean_update_folder(app.wstring());
        check(!fs::exists(app / "update/staged") && !fs::exists(app / "update/release.zip") && fs::exists(app / "update/backup-1") &&
                  !fs::exists(app / "update/backup-0"),
              "install: the next start clears update\\ but the newest backup");

        put(staged / "layer/sim-qvfr.dll", "tampered");
        const InstallResult tampered = install_staged(app.wstring(), staged.wstring(), L"2", [](const InstallProgress&) {});
        check(!tampered.ok && tampered.error.find(L"SHA256") != std::wstring::npos, "install: a file that doesn't match its list stops it");

        // Go back (restore_backup): what each file does, then backup-1 put back over release 2 and forward again.
        {
            const std::map<std::wstring, std::string> mine = {{L"a.dll", hexA}, {L"b.png", hexA}, {L"new.txt", hexA},
                                                              {L"changed.txt", hexA}, {L"sim-qvfr.ini", hexA}};
            const std::map<std::wstring, std::string> previous = {{L"a.dll", hexB}, {L"b.png", hexB}, {L"gone.txt", hexB}};
            const std::map<std::wstring, std::string> now = {{L"a.dll", hexA}, {L"b.png", hexC}, {L"new.txt", hexA},
                                                             {L"changed.txt", hexC}, {L"sim-qvfr.ini", hexC}, {L"gone.txt", hexB}};
            const auto plan = plan_restore(
                previous, mine, [](const std::wstring& p) { return p == L"a.dll" || p == L"b.png"; },
                [&](const std::wstring& p) {
                    const auto it = now.find(p);
                    return it == now.end() ? std::string() : it->second;
                });
            const auto act = [&](const wchar_t* p) {
                for (const RestoreStep& s : plan)
                    if (s.path == p)
                        return static_cast<int>(s.action);
                return -1;
            };
            check(act(L"a.dll") == static_cast<int>(RestoreAction::Restore) && act(L"b.png") == static_cast<int>(RestoreAction::Keep) &&
                      act(L"new.txt") == static_cast<int>(RestoreAction::Remove) &&
                      act(L"changed.txt") == static_cast<int>(RestoreAction::Keep) && act(L"sim-qvfr.ini") == -1 &&
                      act(L"gone.txt") == -1,
                  "go back: the backup's files back, added ones removed, the user's changes and settings kept");
        }
        Backup backup;
        check(find_backup(app.wstring(), backup) && backup.version == L"1" &&
                  fs::equivalent(backup.folder, app / "update/backup-1"),
              "go back: the newest backup found, its version from its VERSION.txt");
        put(app / "layer/helmet/top.png", "my top");  // changed by the user after the update

        HANDLE inUse = CreateFileW((app / "layer/sim-qvfr.dll").c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        const InstallResult blocked = restore_backup(app.wstring(), backup, L"2 (def)", [](const InstallProgress&) {});
        CloseHandle(inUse);
        check(!blocked.ok && blocked.error.find(L"sim-qvfr.dll") != std::wstring::npos &&
                  read_file((app / "sim-qvfr-launcher.exe").wstring(), readBack) && readBack == "new launcher" &&
                  read_file((app / "update/backup-1/sim-qvfr-launcher.exe").wstring(), readBack) && readBack == "old launcher" &&
                  fs::exists(app / "README.txt") && !fs::exists(app / "update/backup-2"),
              "go back: a file in use stops it, and everything moved goes back");

        const InstallResult back = restore_backup(app.wstring(), backup, L"2 (def)", [](const InstallProgress&) {});
        std::string version;
        read_file((app / "sim-qvfr-launcher.exe").wstring(), launcher);
        read_file((app / "layer/sim-qvfr.dll").wstring(), layer);
        read_file((app / "layer/helmet/top.png").wstring(), top);
        read_file((app / "layer/helmet/visor.png").wstring(), visor);
        read_file((app / "sim-qvfr.ini").wstring(), ini);
        read_file((app / "VERSION.txt").wstring(), version);
        check(back.ok && launcher == "old launcher" && layer == "old layer" && version == "Sim-QVFR 1" &&
                  !fs::exists(app / "README.txt") &&
                  !parse_sha256_list((read_file((app / "SHA256.txt").wstring(), readBack), readBack)).count(L"README.txt"),
              "go back: the old release's files and SHA256.txt back, the file release 2 added gone");
        check(top == "my top" && visor == "my visor" && ini == "[mine]" && fs::exists(app / "profiles/Night.ini") && back.kept == 1,
              "go back: the user's settings, profiles and changed files kept");
        Backup forward;
        check(!fs::exists(app / "update/backup-1") && find_backup(app.wstring(), forward) && forward.version == L"2" &&
                  back.backup == (app / "update" / "backup-2").wstring(),
              "go back: release 2 is now the backup");

        const InstallResult again = restore_backup(app.wstring(), forward, L"1", [](const InstallProgress&) {});
        read_file((app / "sim-qvfr-launcher.exe").wstring(), launcher);
        read_file((app / "VERSION.txt").wstring(), version);
        check(again.ok && launcher == "new launcher" && version == "Sim-QVFR 2" && fs::exists(app / "README.txt") &&
                  find_backup(app.wstring(), backup) && backup.version == L"1",
              "go back: going back is undone the same way");

        put(app / "update/backup-1/layer/sim-qvfr.dll", "tampered");
        const InstallResult damaged = restore_backup(app.wstring(), backup, L"2", [](const InstallProgress&) {});
        check(!damaged.ok && damaged.error.find(L"SHA256") != std::wstring::npos &&
                  read_file((app / "sim-qvfr-launcher.exe").wstring(), readBack) && readBack == "new launcher",
              "go back: a backup file that doesn't match its list stops it");
        check(!restore_backup(app.wstring(), backup, L"1", [](const InstallProgress&) {}).ok,
              "go back: never into the backup it reads from");

        // A backup without its launcher (deleted by hand, quarantined): refused, nothing moved.
        put(app / "update/backup-1/layer/sim-qvfr.dll", "old layer");
        fs::remove(app / "update/backup-1/sim-qvfr-launcher.exe", ec);
        const InstallResult partial = restore_backup(app.wstring(), backup, L"2", [](const InstallProgress&) {});
        check(!partial.ok && partial.error.find(L"launcher") != std::wstring::npos &&
                  read_file((app / "sim-qvfr-launcher.exe").wstring(), readBack) && readBack == "new launcher" &&
                  read_file((app / "VERSION.txt").wstring(), version) && version == "Sim-QVFR 2",
              "go back: a backup missing its launcher or layer is refused");
    }

    fs::remove_all(folder, ec);
    return test_result();
}
