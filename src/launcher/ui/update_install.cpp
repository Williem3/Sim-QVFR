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
            result->ok = newest_release(body, result->release);
            if (!result->ok)
                result->error = L"GitHub lists no release";
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
                                                                    : Update::Available;
}

// The first start: whether to check for updates when the launcher starts, asked once ([launcher] update_asked keeps the
// answer; About's switch changes it).
void ask_update_check() {
    Sheet s;
    s.title = L"Check for updates automatically?";
    s.text = L"When it starts, the launcher can ask GitHub for the newest Sim-QVFR release and tell you when there is one. "
             L"Nothing else is sent, and nothing downloads by itself. You can change this in About.";
    s.buttons = {L"Don’t Check", L"Check Automatically"};
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
    app.installProgress = {L"Starting", 0};
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
        notify(L"Close " + game_name() + L" first", L"Sim-QVFR’s layer is in use while the game runs, so it can’t be replaced yet.");
        return;
    }
    const std::wstring mb = std::to_wstring((app.release.zipSize + (1 << 19)) >> 20);
    Sheet s;
    s.title = L"Install Sim-QVFR " + app.release.tag + L"?";
    s.text = L"The launcher downloads it from GitHub (" + mb + L" MB), checks it, replaces its program files and restarts. "
             L"Your settings, profiles, logs and any file you changed (your own helmet images) stay. The version you have now "
             L"is kept in the update folder.";
    s.buttons = {L"Cancel", L"Install"};
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
    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, path, MAX_PATH);  // the new launcher now: the old one moved into the backup
    Sheet s;
    s.title = L"Sim-QVFR " + app.release.tag + L" is installed";
    s.text = std::to_wstring(result->replaced + result->added) + L" files installed, " + std::to_wstring(result->kept) +
             L" of yours kept. The version you had is in " + result->backup + L".";
    s.buttons = {L"Restart Now"};
    s.done = [exe = std::wstring(path)](int, const std::wstring&) { restart_launcher(exe); };
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
        app.installError = L"installed; start the launcher again to use it";
    }
}

}  // namespace

}  // namespace simqvfr::launcher::ui::win
