// The update check (update_check.h), installing a release (install.h) and restarting into it.
#include <filesystem>
#include <memory>
#include <string>
#include <thread>

#include <windows.h>

#include "../launch_plan.h"
#include "../preflight.h"
#include "../install.h"
#include "../update_check.h"
#include "launcher_version.h"
#include "window_state.h"
#include "controls.h"

namespace simqvfr::launcher::ui::win {

// Asks GitHub for the newest release on a worker (update_check.h); the window gets kUpdateMessage.
void start_update_check() {
    if (app.update == Update::Checking)
        return;
    app.update = Update::Checking;
    std::thread([hwnd = app.hwnd] {
        auto* result = new UpdateResult;
        std::string body;
        if (fetch_releases(body, result->error)) {
            result->ok = newest_release(body, result->release, channel_named(SIM_QVFR_CHANNEL));
            if (!result->ok)
                result->error = tr(L"GitHub lists no release");  // About shows it
        }
        if (!PostMessageW(hwnd, kUpdateMessage, 0, reinterpret_cast<LPARAM>(result)))
            delete result;
    }).detach();
}

void update_done(UpdateResult* r) {
    const std::unique_ptr<UpdateResult> result(r);
    if (!result->ok) {
        app.update = Update::Failed;
        app.updateError = result->error;
        return;
    }
    app.release = result->release;
    const std::wstring mine = widen(SIM_QVFR_VERSION);
    app.update = !known_version(mine)                              ? Update::Dev
                 : compare_versions(result->release.tag, mine) <= 0 ? Update::UpToDate
                 : skipped_release(result->release.tag, app.s.updateSkip) ? Update::Skipped
                                                                    : Update::Available;
}

// Skip this version / Don't skip: the newest release found, skipped or offered again.
void skip_release(bool skip) {
    if (app.update != (skip ? Update::Available : Update::Skipped))
        return;
    app.s.updateSkip = skip ? app.release.tag : std::wstring();
    app.update = skip ? Update::Skipped : Update::Available;
    changed();
}

// The first start: whether to check for updates when the launcher starts, asked once ([launcher] update_asked keeps the
// answer; About's switch changes it).
void ask_update_check() {
    Sheet s;
    s.title = tr(L"Check for updates automatically?");
    s.text = tr(L"When it starts, the launcher can ask GitHub for the newest Sim-QVFR release and tell you when there "
                L"is one. Nothing else is sent, and nothing downloads by itself. You can change this in About.");
    s.buttons = {tr(L"Don’t Check"), tr(L"Check Automatically")};
    s.cancel = 0;
    s.done = [](int button, const std::wstring&) {
        app.s.updateAsked = true;
        app.s.updateCheck = button == 1;
        changed();
        if (app.s.updateCheck)
            start_update_check();
    };
    open_sheet(std::move(s));
}

namespace {

std::wstring launcher_folder() { return std::filesystem::path(app.ini).parent_path().wstring(); }

}  // namespace

// Install is offered for a newer release with a zip GitHub gives a SHA-256 for, in a packaged install.
bool installable() {
    return app.update == Update::Available && !app.release.zipSha256.empty() && trusted_download(app.release.zipUrl) &&
           packaged_install(launcher_folder());
}

bool game_running() {
    for (const Check& c : app.checks)
        if (c.id == "game_running")
            return true;
    return false;
}

namespace {

// The install itself, on a worker (install.h): its progress and outcome come back as messages.
void run_install() {
    app.installing = true;
    app.installError.clear();
    app.installProgress = {tr(L"Starting"), 0};  // About shows its step
    std::thread([hwnd = app.hwnd, folder = launcher_folder(), release = app.release, version = widen(SIM_QVFR_VERSION)] {
        // Progress as messages, at most one per percent or step.
        std::wstring lastStep;
        int lastPercent = -1;
        auto* result = new InstallResult(install_release(folder, release.zipUrl, release.zipSha256, release.zipSize, version,
                                                         [&](const InstallProgress& p) {
                                                             const int percent = static_cast<int>(p.share * 100);
                                                             if (p.step == lastStep && percent == lastPercent)
                                                                 return;
                                                             lastStep = p.step;
                                                             lastPercent = percent;
                                                             auto* copy = new InstallProgress(p);
                                                             if (!PostMessageW(hwnd, kInstallProgressMessage, 0,
                                                                               reinterpret_cast<LPARAM>(copy)))
                                                                 delete copy;
                                                         }));
        if (!PostMessageW(hwnd, kInstallDoneMessage, 0, reinterpret_cast<LPARAM>(result)))
            delete result;
    }).detach();
}

}  // namespace

// Install: asked first, in a sheet.
void start_install() {
    if (app.installing || !installable())
        return;
    refresh_checks();
    if (game_running()) {
        notify(trf(L"Close {game} first", {{L"game", game_name()}}),
               tr(L"Sim-QVFR’s layer is in use while the game runs, so it can’t be replaced yet."));
        return;
    }
    const std::wstring mb = std::to_wstring((app.release.zipSize + (1 << 19)) >> 20);
    Sheet s;
    s.title = trf(L"Install Sim-QVFR {version}?", {{L"version", app.release.tag}});
    s.text = trf(L"The launcher downloads it from GitHub ({size} MB), checks it, replaces its program files and "
                 L"restarts. Your settings, profiles, logs and any file you changed (your own helmet images) stay. The "
                 L"version you have now is kept in the update folder.",
                 {{L"size", mb}});
    s.buttons = {tr(L"Cancel"), tr(L"Install")};
    s.done = [](int button, const std::wstring&) {
        if (button == 1)
            run_install();
    };
    open_sheet(std::move(s));
}

namespace {

void restart_launcher(const std::wstring& exe);

}  // namespace

// The install's outcome: on success a sheet says so, then the new launcher starts and this one closes.
void install_done(InstallResult* r) {
    const std::unique_ptr<InstallResult> result(r);
    app.installing = false;
    if (!result->ok) {
        app.installError = result->error;
        return;
    }
    const std::wstring path = module_path();  // the new launcher now: the old one moved into the backup
    refresh_backup();
    Sheet s;
    s.title = trf(L"Sim-QVFR {version} is installed", {{L"version", app.release.tag}});
    s.text = trf(L"{installed} files installed, {kept} of yours kept. The version you had is in {folder}.",
                 {{L"installed", std::to_wstring(result->replaced + result->added)},
                  {L"kept", std::to_wstring(result->kept)},
                  {L"folder", result->backup}});
    s.buttons = {tr(L"Restart Now")};
    s.done = [exe = path](int, const std::wstring&) { restart_launcher(exe); };
    open_sheet(std::move(s));
}

namespace {

// The new launcher started, this one closed.
void restart_launcher(const std::wstring& exe) {
    STARTUPINFOW si{sizeof(si)};
    PROCESS_INFORMATION pi{};
    std::wstring command = L"\"" + exe + L"\"";
    if (CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, FALSE, 0, nullptr, launcher_folder().c_str(), &si, &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        DestroyWindow(app.hwnd);
    } else {
        app.installError = tr(L"installed; start the launcher again to use it");
    }
}

}  // namespace

// Go back is offered for the backup an install left (install.h's find_backup), in a packaged install.
void refresh_backup() {
    Backup backup;
    if (packaged_install(launcher_folder()) && find_backup(launcher_folder(), backup))
        app.backup = backup;
    else
        app.backup.reset();
}

// Go back: asked first, in a sheet; then on a worker, as an install is.
void start_restore() {
    if (app.installing || !app.backup)
        return;
    refresh_checks();
    if (game_running()) {
        notify(trf(L"Close {game} first", {{L"game", game_name()}}),
               tr(L"Sim-QVFR’s layer is in use while the game runs, so it can’t be replaced yet."));
        return;
    }
    // Back to the version the last update replaced, or (after going back) forward to the one it was.
    const bool newer = compare_versions(app.backup->version, widen(SIM_QVFR_VERSION)) > 0;
    Sheet s;
    s.title = newer ? trf(L"Return to Sim-QVFR {version}?", {{L"version", app.backup->version}})
                    : trf(L"Go back to Sim-QVFR {version}?", {{L"version", app.backup->version}});
    const std::wstring mine = widen(SIM_QVFR_VERSION);
    // A launcher before 1.2.2-beta has no Go back and no skipped version: it offers this one again as an update.
    const bool olderLauncher = !newer && compare_versions(app.backup->version, L"1.2.2") < 0;
    s.text = olderLauncher
                 ? trf(L"The launcher puts back the version you had before the last update and restarts. Your settings, "
                       L"profiles, logs and any file you changed stay. {old} has no Go back of its own and offers "
                       L"{version} again as an update: to come back, install it from About.",
                       {{L"old", app.backup->version}, {L"version", mine.substr(0, mine.find(L' '))}})
           : newer ? trf(L"The launcher puts back the version you went back from and restarts. Your settings, profiles, "
                         L"logs and any file you changed stay. The version you have now ({version}) is kept in the "
                         L"update folder, so you can switch back the same way.",
                         {{L"version", mine}})
                   : trf(L"The launcher puts back the version you had before the last update and restarts. Your settings, "
                         L"profiles, logs and any file you changed stay. The version you have now ({version}) is kept in "
                         L"the update folder, so you can switch back the same way. The update check won’t offer it "
                         L"again, only a newer release.",
                         {{L"version", mine}});
    s.buttons = {tr(L"Cancel"), newer ? tr(L"Return") : tr(L"Go Back")};
    s.done = [](int button, const std::wstring&) {
        if (button != 1 || !app.backup)
            return;
        app.installing = app.restoring = true;
        app.restoreError.clear();
        app.installProgress = {tr(L"Starting"), 0};  // About shows its step
        std::thread([hwnd = app.hwnd, folder = launcher_folder(), backup = *app.backup, version = widen(SIM_QVFR_VERSION)] {
            std::wstring lastStep;
            int lastPercent = -1;
            auto* result = new InstallResult(restore_backup(folder, backup, version, [&](const InstallProgress& p) {
                const int percent = static_cast<int>(p.share * 100);
                if (p.step == lastStep && percent == lastPercent)
                    return;
                lastStep = p.step;
                lastPercent = percent;
                auto* copy = new InstallProgress(p);
                if (!PostMessageW(hwnd, kInstallProgressMessage, 0, reinterpret_cast<LPARAM>(copy)))
                    delete copy;
            }));
            if (!PostMessageW(hwnd, kRestoreDoneMessage, 0, reinterpret_cast<LPARAM>(result)))
                delete result;
        }).detach();
    };
    open_sheet(std::move(s));
}

// Going back's outcome: on success a sheet says so, then the old launcher starts and this one closes.
void restore_done(InstallResult* r) {
    const std::unique_ptr<InstallResult> result(r);
    app.installing = app.restoring = false;
    const std::wstring version = app.backup ? app.backup->version : std::wstring();
    refresh_backup();
    if (!result->ok) {
        app.restoreError = result->error;
        return;
    }
    // Going back from this version: the update check doesn't offer it again (a newer one it does). Returning to it:
    // offered again.
    const std::wstring mine = widen(SIM_QVFR_VERSION);
    if (compare_versions(version, mine) < 0)
        app.s.updateSkip = mine.substr(0, mine.find(L' '));
    else if (skipped_release(version, app.s.updateSkip))
        app.s.updateSkip.clear();
    changed();
    const std::wstring path = module_path();  // the old launcher now: this one moved into the update folder
    Sheet s;
    s.title = trf(L"Sim-QVFR {version} is back", {{L"version", version}});
    s.text = trf(L"{restored} files put back, {kept} of yours kept. The version you had is in {folder}.",
                 {{L"restored", std::to_wstring(result->replaced)},
                  {L"kept", std::to_wstring(result->kept)},
                  {L"folder", result->backup}});
    s.buttons = {tr(L"Restart Now")};
    s.done = [exe = path](int, const std::wstring&) { restart_launcher(exe); };
    open_sheet(std::move(s));
}

}  // namespace simqvfr::launcher::ui::win
