// The Game tab's Mods card: the installed game mods (mod_catalog.h), who made each, what the core's trust check will do
// with it (mod_api/mod_trust.h), the user's approval of a third-party mod and its Allow online, and the settings its
// settings.json declares. The launcher only reads the mods' files and hashes their DLLs; it never loads one.
#include <algorithm>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <windows.h>
#include <shellapi.h>
#include <commdlg.h>

#include "../launcher.h"
#include "../mod_catalog.h"
#include "../mod_update.h"
#include "../update_check.h"
#include "launcher_version.h"
#include "../../games/game.h"
#include "../../mod_api/mod_trust.h"
#include "window_state.h"
#include "card.h"
#include "controls.h"

namespace simqvfr::launcher::ui::win {

namespace fs = std::filesystem;
using mod_api::ModTrust;

// A worker's outcome (kModWorkMessage): the update check's releases, or an install's or Go back's result.
struct ModWork {
    enum class Kind { Check, Install, Restore, Stage } kind = Kind::Check;
    std::wstring id, error;
    std::string body;  // Check: the releases JSON
    InstallResult result;
    StagedMod staged;  // Stage: the mod unpacked from a zip
};

namespace {

// The package's mods folder, as the core finds it (mod_api/mod_loader.h mods_folder): beside the layer folder, or in
// the development build's folder.
std::wstring mods_folder_path() {
    const fs::path layer = layer_folder();
    const bool inLayer = _wcsicmp(layer.filename().c_str(), L"layer") == 0;
    return ((inLayer ? layer.parent_path() : layer) / L"mods").wstring();
}

std::wstring short_hash(const std::string& sha) { return std::wstring(sha.begin(), sha.begin() + std::min<size_t>(12, sha.size())); }

std::wstring widen_reason(const std::string& why) { return std::wstring(why.begin(), why.end()); }

void write_mod_approval_now(const std::wstring& id, const mod_api::ModApproval& approval) {
    if (!mod_api::write_mod_approval(app.ini, id, approval))
        notify(tr(L"Not saved"), tr(L"The approval couldn’t be written to sim-qvfr.ini."));
    mods_refresh();
}

void start_mods_check() {
    if (app.modsBusy)
        return;
    app.modsBusy = true;
    app.modsMessage.clear();
    std::thread([hwnd = app.hwnd] {
        auto* w = new ModWork;
        w->kind = ModWork::Kind::Check;
        fetch_releases(w->body, w->error, kModReleasesApiPath);
        if (!PostMessageW(hwnd, kModWorkMessage, 0, reinterpret_cast<LPARAM>(w)))
            delete w;
    }).detach();
}

// Installs the newest release of mod `i` (as last checked), or (`back`) goes back to the version it replaced.
void start_mod_install(size_t i, bool back) {
    if (app.modsBusy)
        return;
    if (game_running()) {
        app.modsMessage = tr(L"Close the game first: the mod’s DLL is in use.");
        app.modsTone = 1;
        return;
    }
    const ModEntry& m = app.mods[i];
    const auto it = app.modReleases.find(m.id);
    if (!back && it == app.modReleases.end())
        return;
    app.modsBusy = true;
    app.modsMessage.clear();
    const ModRelease release = back ? ModRelease{} : it->second;
    std::thread([hwnd = app.hwnd, folder = mods_folder_path(), id = m.id, version = m.version, release, back] {
        auto* w = new ModWork;
        w->kind = back ? ModWork::Kind::Restore : ModWork::Kind::Install;
        w->id = id;
        w->result = back ? restore_mod_backup(folder, id, version)
                         : install_mod_release(folder, id, release, version, [](const InstallProgress&) {});
        if (!PostMessageW(hwnd, kModWorkMessage, 0, reinterpret_cast<LPARAM>(w)))
            delete w;
    }).detach();
}

// A mod unpacked from a zip (start_mod_add): what it is, and whether to add it. Nothing is approved by adding it.
void confirm_add(const StagedMod& staged) {
    if (!staged.error.empty()) {
        app.modsMessage = trf(L"Not added: {why}", {{L"why", staged.error}});
        app.modsTone = 2;
        return;
    }
    Sheet s;
    s.title = trf(L"Add {mod}?", {{L"mod", staged.name}});
    std::wstring text = trf(L"By {author}{version}.", {{L"author", staged.author.empty() ? std::wstring(tr(L"an unknown author")) : staged.author},
                                                       {L"version", staged.version.empty() ? std::wstring() : L", version " + staged.version}});
    if (!staged.source.empty())
        text += L" " + trf(L"Source code: {link} ({license}).", {{L"link", staged.source}, {L"license", staged.license}});
    if (staged.replaces)
        text += L" " + std::wstring(tr(L"It replaces the one installed, which is kept so you can go back."));
    if (!mod_api::first_party_mod(staged.id))
        text += L"\n\n" + std::wstring(tr(L"It wasn’t made by Sim-QVFR: it won’t run until you approve it in Settings > Mods."));
    s.text = text;
    s.buttons = {tr(L"Cancel"), tr(L"Add")};
    s.focus = 1;
    s.done = [staged](int button, const std::wstring&) {
        if (button != 1)
            return;
        if (game_running()) {
            app.modsMessage = tr(L"Close the game first: a mod can’t change while it runs.");
            app.modsTone = 1;
            return;
        }
        std::wstring oldVersion;
        for (const ModEntry& m : app.mods)
            if (_wcsicmp(m.id.c_str(), staged.id.c_str()) == 0)
                oldVersion = m.version;
        const InstallResult r = add_staged_mod(mods_folder_path(), staged, oldVersion);
        app.modsMessage = r.ok ? trf(L"{mod} added.", {{L"mod", staged.name}}) : r.error;
        app.modsTone = r.ok ? 0 : 2;
        mods_refresh();
        // A game it adds joins the rail now (games/game.h mod games).
        if (r.ok && games::rescan_mod_games() > 0)
            refresh_installed();
    };
    open_sheet(std::move(s));
}

void ensure_read() {
    if (!app.modsRead)
        mods_refresh();
}

// The chip's tone and text for what the core will do with it here (offline).
std::pair<int, std::wstring> chip_of(const ModStatus& st) {
    switch (st.offline.trust) {
    case ModTrust::Official: return {0, tr(L"Official")};
    case ModTrust::FirstPartyUnsignedBuild: return {3, tr(L"Built in (unsigned build)")};
    case ModTrust::Community: return {1, tr(L"Third-party, approved")};
    case ModTrust::Refused: break;
    }
    return {2, st.approvable ? tr(L"Third-party, not approved") : tr(L"Won’t load")};
}

std::wstring signed_line(const ModStatus& st) {
    if (st.facts.sha256.empty())
        return tr(L"Its DLL can’t be read.");
    if (!st.facts.signatureTrusted)
        return tr(L"Not signed.");
    return trf(L"Signed by {signer}.", {{L"signer", st.facts.signer}});
}

// A newer release of one of Sim-QVFR's own mods, as last checked.
bool update_available(size_t i) {
    const ModEntry& m = app.mods[i];
    if (!mod_api::first_party_mod(m.id) || !m.error.empty())
        return false;
    const auto it = app.modReleases.find(m.id);
    return it != app.modReleases.end() && compare_mod_versions(m.version, it->second.version) < 0;
}

// A mod's line in the list: its author, version and the SDK it was built with.
std::wstring byline(const ModEntry& m) {
    std::wstring line = m.author.empty() ? std::wstring(tr(L"Unknown author")) : m.author;
    if (!m.version.empty())
        line += L" · " + trf(L"v{version}", {{L"version", m.version}});
    if (!m.sdk.empty())
        line += L" · " + trf(L"SDK {version}", {{L"version", m.sdk}});
    else if (m.abi > 0)
        line += L" · " + trf(L"ABI {abi}", {{L"abi", std::to_wstring(m.abi)}});
    return line;
}

// A mod in the Settings panel's list: name, status, author, version, SDK, and its quick buttons (Approve, Allow online,
// Update); everything else is in its Details window (show_details).
void mod_item(Body& b, size_t i) {
    const ModEntry& m = app.mods[i];
    const ModStatus& st = app.modStatus[i];
    const int n = static_cast<int>(i);
    std::vector<Body::ButtonSpec> buttons;
    if (!m.error.empty()) {
        buttons.push_back({static_cast<Ctl>(kModDetails0 + n), tr(L"Details…")});
        b.item(m.name.empty() ? m.id : m.name, 2, tr(L"Won’t load"), m.id, buttons);
        return;
    }
    const auto [tone, text] = chip_of(st);
    const bool approved = st.offline.trust == ModTrust::Community;
    if (update_available(i))
        buttons.push_back({static_cast<Ctl>(kModInstall0 + n), tr(L"Update"), Btn::Primary});
    if (st.approvable && approved)
        buttons.push_back({static_cast<Ctl>(kModOnline0 + n), st.approval.online ? tr(L"Online: on") : tr(L"Allow online…")});
    if (st.approvable)
        buttons.push_back({static_cast<Ctl>(kModApprove0 + n), approved ? tr(L"Take back") : tr(L"Approve…"),
                           approved ? Btn::Secondary : Btn::Primary});
    buttons.push_back({static_cast<Ctl>(kModDetails0 + n), tr(L"Details…")});
    b.item(m.name, tone, text, byline(m), buttons);
}

// What the core will do with the mod, in a sentence for players (its status badge says it in short).
std::wstring status_sentence(const ModEntry& m, const ModStatus& st) {
    switch (st.offline.trust) {
    case ModTrust::Official: return tr(L"Made and signed by Sim-QVFR. It runs in every launch.");
    case ModTrust::FirstPartyUnsignedBuild:
        return tr(L"Comes with this test build of Sim-QVFR. This build isn’t signed, so the mod stays off while a game’s "
                  L"anti-cheat runs.");
    case ModTrust::Community:
        return st.online.trust == ModTrust::Community ? tr(L"You approved it. It runs offline and online.")
                                                      : tr(L"You approved it. It runs offline.");
    case ModTrust::Refused: break;
    }
    if (st.approvable)
        return st.approval.sha256.empty() ? tr(L"Not approved yet: it won’t run until you approve it.")
                                          : tr(L"Its file changed since you approved it: approve it again to use it.");
    (void)m;
    return trf(L"It won’t load: {why}.", {{L"why", widen_reason(st.offline.why)}});
}

// A mod's Details window, over the Settings panel: what it is, what loads and why, its approval, online, games, source,
// updates, settings; and Remove (not Sim-QVFR's own), Go back, Open folder, View source.
void show_details(size_t i) {
    const ModEntry& m = app.mods[i];
    const ModStatus& st = app.modStatus[i];
    const bool own = mod_api::first_party_mod(m.id);
    Sheet s;
    s.title = m.name.empty() ? m.id : m.name;
    std::wstring t;
    auto line = [&t](const std::wstring& label, const std::wstring& value) {
        if (!value.empty())
            t += (t.empty() ? L"" : L"\n") + label + L":  " + value;
    };
    std::wstring folder, backupVersion;
    const bool backup = m.error.empty() && own && find_mod_backup(mods_folder_path(), m.id, folder, backupVersion);
    if (!m.error.empty()) {
        line(tr(L"Won’t load"), m.error);
    } else {
        std::wstring made = m.author.empty() ? std::wstring(tr(L"Unknown author")) : m.author;
        if (!m.version.empty())
            made += L", " + trf(L"version {version}", {{L"version", m.version}});
        line(tr(L"Made by"), made);
        std::wstring built;
        if (!m.sdk.empty())
            built = trf(L"SDK {version}", {{L"version", m.sdk}});
        if (m.abi > 0)
            built += (built.empty() ? L"" : L", ") + trf(L"mod ABI {abi}", {{L"abi", std::to_wstring(m.abi)}});
        line(tr(L"Built with"), built);
        line(tr(L"Status"), status_sentence(m, st));
        if (st.offline.trust == ModTrust::Community)
            line(tr(L"Approval"), trf(L"This exact file ({hash}…). If it changes, it stops loading until you approve it again.",
                                      {{L"hash", short_hash(st.facts.sha256)}}));
        if (st.approvable)
            line(tr(L"Online"),
                 !st.facts.signatureTrusted ? tr(L"Not signed by its author, so it never runs while a game’s anti-cheat runs.")
                 : st.approval.online
                     ? trf(L"On. Signed by {signer}. It also runs while a game’s anti-cheat runs, which may kick or ban you for it.",
                           {{L"signer", st.facts.signer}})
                     : trf(L"Off. Signed by {signer}, so you can allow it.", {{L"signer", st.facts.signer}}));
        std::wstring games;
        for (const std::wstring& g : m.games)
            games += (games.empty() ? L"" : L", ") + g;
        line(tr(L"Games"), games);
        if (own) {
            const auto it = app.modReleases.find(m.id);
            line(tr(L"Updates"), update_available(i) ? trf(L"Version {version} is available.", {{L"version", it->second.version}})
                                 : !app.modsChecked ? std::wstring()
                                 : it != app.modReleases.end() ? std::wstring(tr(L"Up to date."))
                                                               : std::wstring(tr(L"None released yet.")));
            if (backup)
                line(tr(L"Previous version"), trf(L"{version}, kept by the last update.", {{L"version", backupVersion}}));
        }
        if (!m.source.empty())
            line(tr(L"Source code"), m.source + (m.license.empty() ? L"" : L" (" + m.license + L")"));
        if (!m.settings.empty())
            line(tr(L"Settings"), tr(L"On the Game tab, with each game it covers."));
    }
    s.text = t;
    s.width = 640;
    // Buttons, left to right; the last (Close) is the main one and Escape's.
    std::vector<int> kinds;  // 0 Remove, 1 Go back, 2 Open folder, 3 View source, 4 Close
    if (!own) {  // Sim-QVFR's own: removing it takes the game's support away until the package is reinstalled
        s.buttons.push_back(tr(L"Remove…"));
        kinds.push_back(0);
    }
    if (backup) {
        s.buttons.push_back(tr(L"Go back"));
        kinds.push_back(1);
    }
    s.buttons.push_back(tr(L"Open folder"));
    kinds.push_back(2);
    if (!m.source.empty()) {
        s.buttons.push_back(tr(L"View source"));
        kinds.push_back(3);
    }
    s.buttons.push_back(tr(L"Close"));
    kinds.push_back(4);
    s.cancel = static_cast<int>(s.buttons.size()) - 1;
    s.focus = s.cancel;
    const int n = static_cast<int>(i);
    const std::wstring modFolder = m.folder;
    s.done = [kinds, n, modFolder](int button, const std::wstring&) {
        if (button < 0 || button >= static_cast<int>(kinds.size()))
            return;
        switch (kinds[static_cast<size_t>(button)]) {
        case 0: mods_press(static_cast<Ctl>(kModRemove0 + n), -1); break;
        case 1: mods_press(static_cast<Ctl>(kModRestore0 + n), -1); break;
        case 2: ShellExecuteW(app.hwnd, L"explore", modFolder.c_str(), nullptr, nullptr, SW_SHOWNORMAL); break;
        case 3: mods_press(static_cast<Ctl>(kModSource0 + n), -1); break;
        default: break;
        }
    };
    open_sheet(std::move(s));
}

// A mod's settings (its settings.json), on the Game tab of a game it covers.
void settings_rows(Body& b, size_t i) {
    const ModEntry& m = app.mods[i];
    if (!m.settingsError.empty())
        b.note(m.settingsError);
    for (size_t k = 0; k < m.settings.size() && k < static_cast<size_t>(kModSettingsMax); ++k) {
        const ModSetting& s = m.settings[k];
        const Ctl id = static_cast<Ctl>(kModSetting0 + static_cast<int>(i) * kModSettingsMax + static_cast<int>(k));
        const std::string value = mod_setting_value(app.ini, m.id, s);
        if (s.kind == ModSetting::Kind::Toggle) {
            b.toggle(id, s.label, value == "1", s.help, false, Applies::NextLaunch);
            continue;
        }
        std::vector<std::wstring> items;
        int selected = 0;
        for (size_t o = 0; o < s.options.size(); ++o) {
            items.push_back(s.options[o].label);
            if (s.options[o].value == value)
                selected = static_cast<int>(o);
        }
        b.segments(id, s.label, items, selected);
        if (!s.help.empty())
            b.note(s.help);
    }
}

// The mod's folder to the Recycle Bin, after asking (Windows asks too on a drive without one).
void remove_mod(size_t i) {
    const ModEntry& m = app.mods[i];
    Sheet s;
    s.title = trf(L"Remove {mod}?", {{L"mod", m.name}});
    s.text = trf(L"Its folder ({folder}) goes to the Recycle Bin, so you can get it back from there. Its settings and "
                 L"approval stay in sim-qvfr.ini.",
                 {{L"folder", m.folder}});
    s.buttons = {tr(L"Cancel"), tr(L"Remove")};
    s.focus = 0;
    s.danger = true;
    const std::wstring folder = m.folder;
    s.done = [folder](int button, const std::wstring&) {
        if (button != 1)
            return;
        std::wstring list = folder;  // double-null terminated
        list += L'\0';
        list += L'\0';
        SHFILEOPSTRUCTW op{};
        op.hwnd = app.hwnd;
        op.wFunc = FO_DELETE;
        op.pFrom = list.c_str();
        op.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_WANTNUKEWARNING | FOF_SILENT;
        if (SHFileOperationW(&op) != 0 || op.fAnyOperationsAborted)
            notify(tr(L"Not removed"), tr(L"The mod’s folder couldn’t be moved to the Recycle Bin (is the game running?)."));
        mods_refresh();
    };
    open_sheet(std::move(s));
}

void approve(size_t i) {
    const ModEntry& m = app.mods[i];
    const ModStatus& st = app.modStatus[i];
    if (st.offline.trust == ModTrust::Community) {  // take it back: at once
        write_mod_approval_now(m.id, {});
        return;
    }
    Sheet s;
    s.title = trf(L"Approve {mod}?", {{L"mod", m.name}});
    s.text = trf(L"{mod} wasn’t made by Sim-QVFR. {signed} It runs inside the game with the same access as the game "
                 L"itself.\n\nApprove it only if you trust who made it. Sim-QVFR and its developer aren’t responsible "
                 L"for what it does. You approve this exact file: an update to it needs your approval again.",
                 {{L"mod", m.name}, {L"signed", signed_line(st)}});
    s.buttons = {tr(L"Cancel"), tr(L"Approve")};
    s.focus = 0;
    s.danger = true;
    const std::wstring id = m.id;
    const std::string sha = st.facts.sha256;
    s.done = [id, sha](int button, const std::wstring&) {
        if (button != 1)
            return;
        mod_api::ModApproval a;
        a.sha256 = sha;
        write_mod_approval_now(id, a);
    };
    open_sheet(std::move(s));
}

void allow_online(size_t i) {
    const ModEntry& m = app.mods[i];
    const ModStatus& st = app.modStatus[i];
    if (st.offline.trust != ModTrust::Community)
        return;
    if (!st.facts.signatureTrusted) {
        notify(tr(L"Offline only"), trf(L"{mod} isn’t signed by its author, so it can’t run while a game’s anti-cheat runs. "
                                        L"Its author can sign it to make that possible.",
                                        {{L"mod", m.name}}));
        return;
    }
    mod_api::ModApproval a = st.approval;
    if (a.online) {  // off: at once
        a.online = false;
        write_mod_approval_now(m.id, a);
        return;
    }
    Sheet s;
    s.title = trf(L"Let {mod} run online?", {{L"mod", m.name}});
    s.text = trf(L"{mod} will also run while a game’s anti-cheat runs. The anti-cheat sees its author’s signature "
                 L"({signer}), not Sim-QVFR’s, and may kick you or ban your account for it.\n\nSim-QVFR and its "
                 L"developer aren’t responsible for any kick or ban. By turning this on, you accept that risk.",
                 {{L"mod", m.name}, {L"signer", st.facts.signer}});
    s.buttons = {tr(L"Cancel"), tr(L"I accept the risk")};
    s.focus = 0;
    s.danger = true;
    const std::wstring id = m.id;
    s.done = [id, a](int button, const std::wstring&) mutable {
        if (button != 1)
            return;
        a.online = true;
        write_mod_approval_now(id, a);
    };
    open_sheet(std::move(s));
}

}  // namespace

void mods_work_done(ModWork* raw) {
    const std::unique_ptr<ModWork> w(raw);
    app.modsBusy = false;
    if (w->kind == ModWork::Kind::Stage) {
        confirm_add(w->staged);
        return;
    }
    if (w->kind == ModWork::Kind::Check) {
        if (!w->error.empty()) {
            app.modsMessage = w->error;
            app.modsTone = 2;
            return;
        }
        app.modReleases.clear();
        const bool nightly = channel_named(SIM_QVFR_CHANNEL) == Channel::Nightly;
        int available = 0;
        for (const ModEntry& m : app.mods) {
            ModRelease r;
            if (!mod_api::first_party_mod(m.id) || !newest_mod_release(w->body, m.id, nightly, r))
                continue;
            if (compare_mod_versions(m.version, r.version) < 0)
                ++available;
            app.modReleases[m.id] = r;
        }
        app.modsChecked = true;
        app.modsMessage = available ? trn(available, L"{count} update available", L"{count} updates available",
                                          {{L"count", std::to_wstring(available)}})
                                    : std::wstring(tr(L"Sim-QVFR’s mods are up to date"));
        app.modsTone = available ? 1 : 0;
        return;
    }
    if (w->result.ok) {
        app.modsMessage = w->kind == ModWork::Kind::Install ? tr(L"Installed. It’s used from the next launch.")
                                                             : tr(L"Went back. It’s used from the next launch.");
        app.modsTone = 0;
    } else {
        app.modsMessage = w->result.error;
        app.modsTone = 2;
    }
    mods_refresh();
}

void start_mod_add(const std::wstring& zip) {
    if (app.panel != PanelKind::Settings)
        open_panel(PanelKind::Settings);
    if (app.modsBusy)
        return;
    app.modsBusy = true;
    app.modsMessage.clear();
    std::thread([hwnd = app.hwnd, folder = mods_folder_path(), zip] {
        auto* w = new ModWork;
        w->kind = ModWork::Kind::Stage;
        std::error_code ec;
        std::filesystem::create_directories(folder, ec);
        w->staged = stage_mod_zip(folder, zip);
        if (!PostMessageW(hwnd, kModWorkMessage, 0, reinterpret_cast<LPARAM>(w)))
            delete w;
    }).detach();
    InvalidateRect(app.hwnd, nullptr, FALSE);
}

void mods_refresh() {
    app.modsRead = true;
    const mod_api::ModFacts core = mod_api::read_mod_facts((fs::path(layer_folder()) / L"sim-qvfr.dll").wstring());
    app.coreOfficial = core.signatureTrusted && mod_api::official_thumbprint(core.thumbprint);
    app.mods = scan_mods(mods_folder_path());
    app.modStatus.clear();
    for (const ModEntry& m : app.mods)
        app.modStatus.push_back(mod_status(m, app.ini, app.coreOfficial));
    InvalidateRect(app.hwnd, nullptr, FALSE);
}

bool mods_press(Ctl id, int option) {
    if (id == kModsReload) {
        mods_refresh();
        return true;
    }
    if (id == kModsOpenFolder) {
        const std::wstring folder = mods_folder_path();
        std::error_code ec;
        fs::create_directories(folder, ec);
        ShellExecuteW(app.hwnd, L"explore", folder.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        return true;
    }
    if (id == kModsAdd) {
        wchar_t path[MAX_PATH] = {};
        OPENFILENAMEW ofn{sizeof(ofn)};
        ofn.hwndOwner = app.hwnd;
        ofn.lpstrFilter = L"Sim-QVFR mod (*.zip)\0*.zip\0";
        ofn.lpstrFile = path;
        ofn.nMaxFile = MAX_PATH;
        ofn.lpstrTitle = tr(L"Choose a mod’s zip");
        ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
        if (GetOpenFileNameW(&ofn))
            start_mod_add(path);
        return true;
    }
    if (id == kModsCheck) {
        start_mods_check();
        return true;
    }
    if (id >= kModInstall0 && id <= kModInstallLast) {
        const size_t i = static_cast<size_t>(id - kModInstall0);
        if (i < app.mods.size())
            start_mod_install(i, false);
        return true;
    }
    if (id >= kModRestore0 && id <= kModRestoreLast) {
        const size_t i = static_cast<size_t>(id - kModRestore0);
        if (i < app.mods.size())
            start_mod_install(i, true);
        return true;
    }
    if (id >= kModSource0 && id <= kModSourceLast) {
        const size_t i = static_cast<size_t>(id - kModSource0);
        std::string link;
        if (i < app.mods.size())
            for (wchar_t c : app.mods[i].source)
                link += c < 0x80 ? static_cast<char>(c) : ' ';
        if (!link.empty() && valid_source_link(link))
            ShellExecuteW(app.hwnd, L"open", app.mods[i].source.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        return true;
    }
    if (id >= kModDetails0 && id <= kModDetailsLast) {
        const size_t i = static_cast<size_t>(id - kModDetails0);
        if (i < app.mods.size() && i < app.modStatus.size())
            show_details(i);
        return true;
    }
    if (id >= kModRemove0 && id <= kModRemoveLast) {
        const size_t i = static_cast<size_t>(id - kModRemove0);
        if (i < app.mods.size())
            remove_mod(i);
        return true;
    }
    if (id >= kModApprove0 && id <= kModApproveLast) {
        const size_t i = static_cast<size_t>(id - kModApprove0);
        if (i < app.mods.size() && i < app.modStatus.size())
            approve(i);
        return true;
    }
    if (id >= kModOnline0 && id <= kModOnlineLast) {
        const size_t i = static_cast<size_t>(id - kModOnline0);
        if (i < app.mods.size() && i < app.modStatus.size())
            allow_online(i);
        return true;
    }
    if (id >= kModSetting0 && id <= kModSettingLast) {
        const int n = id - kModSetting0;
        const size_t i = static_cast<size_t>(n / kModSettingsMax), k = static_cast<size_t>(n % kModSettingsMax);
        if (i >= app.mods.size() || k >= app.mods[i].settings.size())
            return true;
        const ModEntry& m = app.mods[i];
        const ModSetting& s = m.settings[k];
        std::string value;
        if (s.kind == ModSetting::Kind::Toggle)
            value = mod_setting_value(app.ini, m.id, s) == "1" ? "0" : "1";
        else if (option >= 0 && option < static_cast<int>(s.options.size()))
            value = s.options[static_cast<size_t>(option)].value;
        else
            return true;
        write_mod_setting(app.ini, m.id, s, value);
        InvalidateRect(app.hwnd, nullptr, FALSE);
        return true;
    }
    return false;
}

// The Settings panel (the rail's Settings button): the app's own settings, not a game's. Its Mods section lists every
// installed mod, whichever games it covers.
float settings_pane(float x, float y, float w, bool draw) {
    ensure_read();
    Body b(x, y, w, draw);
    b.group(tr(L"Mods"));
    b.note(app.mods.empty() ? tr(L"No game mods are installed. Add one with Add a mod…, or drop its zip on this window.")
                            : tr(L"Add a mod with Add a mod…, or drop its zip on this window. A mod not made by Sim-QVFR "
                                 L"runs only once you approve it."));
    if (app.modsBusy)
        b.note(tr(L"Working…"));
    else if (!app.modsMessage.empty())
        b.chip(app.modsTone, app.modsMessage);
    b.actions({{kModsAdd, tr(L"Add a mod…"), Btn::Primary}, {kModsCheck, tr(L"Check for updates")}, {kModsOpenFolder, tr(L"Open the mods folder")},
               {kModsReload, tr(L"Look again")}});
    for (size_t i = 0; i < app.mods.size() && i < app.modStatus.size(); ++i) {
        b.divider();
        mod_item(b, i);
    }
    return b.height();
}

// The Game tab's card: the settings of the mods that cover the shown game (none: no rows, a pointer to the Mods tab).
Card mods_card() {
    ensure_read();
    Card c{Tab::Game, tr(L"Mod settings")};
    c.body = [](Body& b) {
        const std::wstring game = games::wide_id(games::launcher_game());
        bool any = false;
        for (size_t i = 0; i < app.mods.size(); ++i) {
            const ModEntry& m = app.mods[i];
            if (m.settings.empty() && m.settingsError.empty())
                continue;
            if (std::none_of(m.games.begin(), m.games.end(),
                             [&](const std::wstring& g) { return _wcsicmp(g.c_str(), game.c_str()) == 0; }))
                continue;
            if (any)
                b.divider();
            any = true;
            b.group(m.name);
            settings_rows(b, i);
        }
        if (!any)
            b.note(tr(L"No installed mod has settings for this game. Settings (in the rail) lists every mod."));
    };
    return c;
}

}  // namespace simqvfr::launcher::ui::win
