// Mods in the launcher: the Settings panel's list (Built in, Third-party: a row per mod, its status in pills), a mod's
// Details over it (what loads and why, approval, Allow online, games, updates, Technical details), and the Game tab's
// card with the settings each mod's settings.json declares. The core's trust check decides what loads
// (mod_api/mod_trust.h). The launcher only reads the mods' files and hashes their DLLs; it never loads one.
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

namespace {

// ---- The Settings panel's mods list and a mod's Details (drawn to Willie's mockups, 2026-11-24) ----------------------

constexpr const char* kIconCube = "M8 1.75l5.5 3.1v6.3L8 14.25l-5.5-3.1v-6.3zM2.5 4.85L8 8l5.5-3.15M8 8v6.25";
constexpr const char* kIconChevronRight = "M6 3.5L10.5 8 6 12.5";
constexpr const char* kIconChevronDown = "M3.5 6L8 10.5 12.5 6";
constexpr const char* kIconRescan = "M13.25 8A5.25 5.25 0 1 1 11.7 4.3M12.5 1.75v3h-3";
constexpr const char* kIconFolder = "M1.75 4.25v8h12.5v-6.5H7.5L6 4.25z";
constexpr const char* kIconUpload = "M8 11V2.75M4.75 6L8 2.75 11.25 6M2.75 10.5v2.75h10.5V10.5";
constexpr const char* kIconCheck = "M3.5 8.25l3 3 6-6.5";
constexpr const char* kIconWarn = "M8 2l6.5 11.5h-13zM8 6.5v3";
constexpr const char* kIconBlock = "M3.6 3.6l8.8 8.8";
constexpr const char* kIconClose = "M4 4L12 12M12 4L4 12";

// A game's full name from its id (a built-in game or one a mod adds), else the id.
std::wstring mod_game_name(const std::wstring& id) {
    std::string narrow;
    for (wchar_t c : id)
        narrow += c < 0x80 ? static_cast<char>(c) : '?';
    if (const games::GameInfo* g = games::game_by_id(narrow.c_str()))
        return g->name;
    return id;
}

std::wstring games_line(const ModEntry& m) {
    std::wstring line;
    for (const std::wstring& g : m.games)
        line += (line.empty() ? L"" : L", ") + mod_game_name(g);
    return line;
}

// A pill: its text in a tone's colour on that tone's tint. Tones: 0 green, 1 amber, 2 red, 3 grey; `bare`: no fill
// (the green "Approved"). Returns its width; draws only with `draw`.
float pill(float x, float y, int tone, const std::wstring& text, const char* icon, bool bare, bool draw) {
    const float iconW = icon ? 16.0f : 0.0f;
    const float w = std::ceil(app.canvas.measure(text, t::kBadge)) + 16 + iconW;
    if (draw) {
        const uint32_t bg[] = {app.c->success_bg, app.c->warning_bg, app.c->danger_bg, app.c->segment_track};
        const uint32_t fg[] = {app.c->success, app.c->warning, app.c->danger, app.c->label_secondary};
        if (!bare)
            app.canvas.fill({x, y, w, 22}, col(bg[tone]), t::kRadiusFull);
        if (icon)
            app.canvas.path(icon, {x + 8, y + 5, 12, 12}, 16, col(fg[tone]), 1.6f);
        app.canvas.text(text, t::kBadge, {x + 8 + iconW, y, w - 16 - iconW, 22}, col(fg[tone]));
    }
    return w;
}

void icon_tile(const Rect& r) {
    app.canvas.fill(r, col(app.c->control_bg), 6);
    app.canvas.stroke(r.inset(0.5f, 0.5f), col(app.c->control_border), 1, 6);
    const float s = std::round(r.w * 0.5f);
    app.canvas.path(kIconCube, {r.x + (r.w - s) / 2, r.y + (r.h - s) / 2, s, s}, 16, col(app.c->accent), 1.4f);
}

// Text cut from its start ("…\mods\hello") to fit `w`.
std::wstring fit_left(const std::wstring& text, const t::TypeStyle& style, float w) {
    if (app.canvas.measure(text, style) <= w)
        return text;
    std::wstring s = text;
    while (s.size() > 1 && app.canvas.measure(L"…" + s, style) > w)
        s.erase(0, 1);
    return L"…" + s;
}

// "owner/repo › path" for a GitHub link (its tree or blob and branch left out), else the link without https://.
std::wstring source_shown(const std::wstring& link) {
    std::wstring rest = link.rfind(L"https://", 0) == 0 ? link.substr(8) : link;
    const std::wstring gh = L"github.com/";
    if (rest.rfind(gh, 0) != 0)
        return rest;
    rest = rest.substr(gh.size());
    std::vector<std::wstring> parts;
    for (size_t at = 0; at <= rest.size();) {
        const size_t slash = rest.find(L'/', at);
        const std::wstring part = rest.substr(at, slash == std::wstring::npos ? std::wstring::npos : slash - at);
        if (!part.empty())
            parts.push_back(part);
        if (slash == std::wstring::npos)
            break;
        at = slash + 1;
    }
    if (parts.size() < 2)
        return rest;
    std::wstring shown = parts[0] + L"/" + parts[1];
    size_t from = 2;
    if (parts.size() > 3 && (parts[2] == L"tree" || parts[2] == L"blob"))
        from = 4;  // and the branch
    std::wstring path;
    for (size_t k = from; k < parts.size(); ++k)
        path += (path.empty() ? L"" : L"/") + parts[k];
    return path.empty() ? shown : shown + L" › " + path;
}

// The pills a mod's row shows: what loads, in short.
struct PillSpec {
    int tone;
    std::wstring text;
    const char* icon;
    bool bare;
};

std::vector<PillSpec> row_pills(size_t i) {
    const ModEntry& m = app.mods[i];
    const ModStatus& st = app.modStatus[i];
    std::vector<PillSpec> p;
    if (!m.error.empty()) {
        p.push_back({2, tr(L"Won’t load"), nullptr, false});
        return p;
    }
    if (update_available(i))
        p.push_back({1, tr(L"Update available"), nullptr, false});
    switch (st.offline.trust) {
    case ModTrust::Official: p.push_back({0, tr(L"Official"), kIconCheck, true}); break;
    case ModTrust::FirstPartyUnsignedBuild:
        p.push_back({3, channel_named(SIM_QVFR_CHANNEL) == Channel::Nightly ? tr(L"Nightly · unsigned") : tr(L"Unsigned build"),
                     nullptr, false});
        break;
    case ModTrust::Community:
        p.push_back({0, tr(L"Approved"), kIconCheck, true});
        if (!st.facts.signatureTrusted)
            p.push_back({1, tr(L"Unsigned · offline only"), nullptr, false});
        else
            p.push_back(st.approval.online ? PillSpec{1, tr(L"Online allowed"), nullptr, false}
                                           : PillSpec{3, tr(L"Signed · offline"), nullptr, false});
        break;
    case ModTrust::Refused:
        if (st.approvable)
            p.push_back(st.approval.sha256.empty() ? PillSpec{1, tr(L"Needs approval"), nullptr, false}
                                                   : PillSpec{2, tr(L"File changed"), nullptr, false});
        else
            p.push_back({2, tr(L"Won’t load"), nullptr, false});
        break;
    }
    return p;
}

constexpr float kModRowH = 56;

// One group of the list ("Built in", "Third-party"): its heading and caption, then its rows in a rounded card; each row
// opens the mod's Details.
void mod_group(Body& b, const std::wstring& title, const std::wstring& caption, const std::vector<size_t>& rows) {
    if (rows.empty())
        return;
    b.custom(t::kBodyStrong.lineHeight, [&](const Rect& r) {
        app.canvas.text(title, t::kBodyStrong, {r.x, r.y, r.w, r.h}, col(app.c->label));
        const float tw = std::ceil(app.canvas.measure(title, t::kBodyStrong));
        app.canvas.text(caption, t::kSubheadline, {r.x + tw + 10, r.y, r.w - tw - 10, r.h}, col(app.c->label_muted));
        reader_text(r, title + L". " + caption, 3);
    });
    b.custom(kModRowH * static_cast<float>(rows.size()), [&](const Rect& box) {
        app.canvas.fill(box, col(app.c->group_bg), 8);
        app.canvas.stroke(box.inset(0.5f, 0.5f), col(app.c->card_border), 1, 8);
        for (size_t k = 0; k < rows.size(); ++k) {
            const size_t i = rows[k];
            const ModEntry& m = app.mods[i];
            const Rect row{box.x, box.y + kModRowH * static_cast<float>(k), box.w, kModRowH};
            if (pointer_in(row))
                app.canvas.fill(row.inset(1, 1), col(app.c->control_hover, 0.6f), 7);
            if (k > 0)
                app.canvas.line(row.x + 12, row.y + 0.5f, row.right() - 12, row.y + 0.5f, col(app.c->separator), 1);
            icon_tile({row.x + 12, row.y + 12, 32, 32});
            // Pills and the chevron, trailing.
            const std::vector<PillSpec> pills = row_pills(i);
            float px = row.right() - 14 - 12;
            app.canvas.path(kIconChevronRight, {px, row.y + (kModRowH - 12) / 2, 12, 12}, 16, col(app.c->label_muted), 1.6f);
            px -= 10;
            std::wstring spoken;
            for (auto it = pills.rbegin(); it != pills.rend(); ++it) {
                const float w = pill(0, 0, it->tone, it->text, it->icon, it->bare, false);
                px -= w;
                pill(px, row.y + (kModRowH - 22) / 2, it->tone, it->text, it->icon, it->bare, true);
                px -= 6;
                spoken = it->text + (spoken.empty() ? L"" : L", " + spoken);
            }
            const float tx = row.x + 56, tw = std::max(40.0f, px - tx - 8);
            const std::wstring name = m.name.empty() ? m.id : m.name;
            app.canvas.text(fit(name, t::kBodyStrong, tw), t::kBodyStrong, {tx, row.y + 9, tw, 20}, col(app.c->label));
            std::wstring sub = m.error.empty() ? games_line(m) : m.error;
            if (m.error.empty() && !mod_api::first_party_mod(m.id) && !m.version.empty())
                sub += (sub.empty() ? L"" : L" · ") + trf(L"v{version}", {{L"version", m.version}});
            app.canvas.text(fit(sub, t::kSubheadline, tw), t::kSubheadline, {tx, row.y + 29, tw, 17}, col(app.c->label_secondary));
            reader_text(row, name + L". " + sub + L". " + spoken);
            app.hits.push_back({static_cast<Ctl>(kModDetails0 + static_cast<int>(i)), row, -1, {}, name});
            if (focused(static_cast<Ctl>(kModDetails0 + static_cast<int>(i))))
                focus_ring(row.inset(1, 1), 7);
        }
    });
}

// ---- Details ----

// The banner's tone, title and text: what the core will do with the mod, for players.
struct Banner {
    int tone;  // 0 green, 1 amber, 2 red, 3 grey
    std::wstring title, text;
};

Banner banner_of(const ModEntry& m, const ModStatus& st) {
    if (!m.error.empty())
        return {2, tr(L"Won’t load"), m.error};
    switch (st.offline.trust) {
    case ModTrust::Official: return {0, tr(L"Official mod"), tr(L"Made and signed by Sim-QVFR. It runs in every launch.")};
    case ModTrust::FirstPartyUnsignedBuild:
        return {3, tr(L"Built in, unsigned test build"),
                tr(L"Comes with this test build of Sim-QVFR. This build isn’t signed, so the mod stays off while a game’s "
                   L"anti-cheat is active.")};
    case ModTrust::Community:
        if (!st.facts.signatureTrusted)
            return {1, tr(L"Third-party mod, not signed"),
                    tr(L"Not made by Sim-QVFR. You approved it, so it runs offline, but it never runs while a game’s "
                       L"anti-cheat is active.")};
        return {st.approval.online ? 1 : 3, trf(L"Third-party mod, signed by {signer}", {{L"signer", st.facts.signer}}),
                st.approval.online
                    ? tr(L"Not made by Sim-QVFR. You approved it and allowed it online: it also runs while a game’s "
                         L"anti-cheat is active, which may kick or ban you for it.")
                    : tr(L"Not made by Sim-QVFR. You approved it, so it runs offline. You can allow it online below.")};
    case ModTrust::Refused: break;
    }
    if (st.approvable && !st.approval.sha256.empty())
        return {2, tr(L"Not loading: the file changed"),
                tr(L"This mod’s file is different from the one you approved. Review the source, then approve it again to "
                   L"turn it back on.")};
    if (st.approvable)
        return {1, tr(L"Not approved yet"),
                tr(L"Not made by Sim-QVFR. It runs inside the game with the game’s access: approve it only if you trust "
                   L"who made it.")};
    return {2, tr(L"Won’t load"), widen_reason(st.offline.why)};
}

constexpr float kLabelCol = 110;

// A label/value row inside a details card: the label muted on the left, `value` paints the rest and returns its height.
float detail_row(float x, float y, float w, bool draw, bool first, const std::wstring& label,
                 const std::function<float(float vx, float vy, float vw, bool draw)>& value) {
    const float pad = 12;
    const float vh = value(x + pad + kLabelCol, y + pad, w - 2 * pad - kLabelCol, false);
    const float h = std::max(20.0f, vh) + 2 * pad;
    if (draw) {
        if (!first)
            app.canvas.line(x + pad, y + 0.5f, x + w - pad, y + 0.5f, col(app.c->separator), 1);
        app.canvas.text(label, t::kCallout, {x + pad, y + pad, kLabelCol - 8, 20}, col(app.c->label_muted));
        reader_text({x + pad, y + pad, kLabelCol - 8, 20}, label);
        value(x + pad + kLabelCol, y + pad, w - 2 * pad - kLabelCol, true);
    }
    return h;
}

// A small button inside a value (Copy, Show in Explorer): trailing at `right`.
float small_button(Ctl id, float right, float y, const std::wstring& label, bool draw) {
    const float w = std::ceil(app.canvas.measure(label, t::kSubheadline)) + 16;
    if (draw) {
        const Rect r{right - w, y - 2, w, 24};
        const bool over = pointer_in(r);
        app.canvas.fill(r, col(over ? app.c->control_hover : app.c->control_bg), t::kRadiusControl);
        app.canvas.stroke(r.inset(0.5f, 0.5f), col(app.c->control_border), 1, t::kRadiusControl);
        app.canvas.text(label, t::kSubheadline, r, col(app.c->label), Align::Center);
        app.hits.push_back({id, r, -1, {}, label});
        if (focused(id))
            focus_ring(r, t::kRadiusControl);
    }
    return w;
}

// A link (accent text) with a hit.
float link_text(Ctl id, float x, float y, float w, const std::wstring& text, bool draw) {
    const std::wstring shown = fit(text, t::kCallout, w);
    const float tw = std::min(w, std::ceil(app.canvas.measure(shown, t::kCallout)));
    if (draw) {
        const Rect r{x, y, tw, 20};
        app.canvas.text(shown, t::kCallout, r, col(app.c->accent));
        if (pointer_in(r))
            app.canvas.line(x, y + 17.5f, x + tw, y + 17.5f, col(app.c->accent), 1);
        app.hits.push_back({id, r, -1, {}, text});
        if (focused(id))
            focus_ring(r.inset(-2, -1), t::kRadiusControl);
    }
    return 20;
}

// The details' scrolled part: banner, the main card, Technical details. Height; draws with `draw`.
float details_content(size_t i, float x, float y, float w, bool draw) {
    const ModEntry& m = app.mods[i];
    const ModStatus& st = app.modStatus[i];
    const bool own = mod_api::first_party_mod(m.id);
    Canvas& cv = app.canvas;
    float h = 0;
    // The banner.
    {
        const Banner bn = banner_of(m, st);
        const uint32_t bg[] = {app.c->success_bg, app.c->warning_bg, app.c->danger_bg, app.c->segment_track};
        const uint32_t fg[] = {app.c->success, app.c->warning, app.c->danger, app.c->label};
        const float tw = w - 24 - 22;
        const float th = bn.text.empty() ? 0 : cv.measure_height(bn.text, t::kCallout, tw);
        const float bh = 12 + 20 + (th > 0 ? 2 + th : 0) + 12;
        if (draw) {
            const Rect r{x, y + h, w, bh};
            cv.fill(r, col(bg[bn.tone]), 8);
            cv.stroke(r.inset(0.5f, 0.5f), col(fg[bn.tone], 0.35f), 1, 8);
            const char* icon = bn.tone == 0 ? kIconCheck : bn.tone == 2 ? nullptr : bn.tone == 1 ? kIconWarn : nullptr;
            const Rect ir{r.x + 12, r.y + 14, 16, 16};
            if (icon)
                cv.path(icon, ir, 16, col(fg[bn.tone]), 1.6f);
            else {
                cv.circle(ir.x + 8, ir.y + 8, 6.5f, col(fg[bn.tone]), false, 1.5f);
                if (bn.tone == 2)
                    cv.path(kIconBlock, ir, 16, col(fg[bn.tone]), 1.5f);
            }
            cv.text(bn.title, t::kBodyStrong, {r.x + 36, r.y + 12, tw, 20}, col(fg[bn.tone]));
            if (th > 0)
                cv.text(bn.text, t::kCallout, {r.x + 36, r.y + 34, tw, th + 2}, col(app.c->label), Align::Leading, false, true);
            reader_text(r, bn.title + L". " + bn.text);
        }
        h += bh + 12;
    }
    // The main card: Games, Approval, Online, Updates, Settings.
    {
        using Value = std::function<float(float, float, float, bool)>;
        std::vector<std::pair<std::wstring, Value>> rows;
        if (!m.games.empty())
            rows.push_back({tr(L"Games"), [&m](float vx, float vy, float vw, bool d) {
                                float cx = 0, cy = 0;
                                for (const std::wstring& g : m.games) {
                                    const std::wstring name = mod_game_name(g);
                                    const float pw = std::ceil(app.canvas.measure(name, t::kBadge)) + 16;
                                    if (cx > 0 && cx + pw > vw) {
                                        cx = 0;
                                        cy += 26;
                                    }
                                    if (d) {
                                        const Rect r{vx + cx, vy + cy - 1, pw, 22};
                                        app.canvas.fill(r, col(app.c->control_bg), 6);
                                        app.canvas.stroke(r.inset(0.5f, 0.5f), col(app.c->control_border), 1, 6);
                                        app.canvas.text(name, t::kBadge, r, col(app.c->label), Align::Center);
                                    }
                                    cx += pw + 6;
                                }
                                return cy + 20;
                            }});
        if (m.error.empty() && st.approvable) {
            rows.push_back({tr(L"Approval"), [&st](float vx, float vy, float vw, bool d) {
                                const bool approved = st.offline.trust == ModTrust::Community;
                                const bool changed = !approved && !st.approval.sha256.empty();
                                const std::wstring head = approved ? tr(L"Approved for this exact file")
                                                          : changed ? tr(L"Needs approval again")
                                                                    : tr(L"Not approved");
                                const std::wstring sub =
                                    approved ? tr(L"If the file changes, it stops loading until you approve it again.")
                                    : changed ? trf(L"Approved {old}… · now {now}…", {{L"old", short_hash(st.approval.sha256)},
                                                                                     {L"now", short_hash(st.facts.sha256)}})
                                              : tr(L"It won’t run until you approve it.");
                                const t::TypeStyle& subStyle = changed ? t::kValue : t::kCallout;
                                const float sh = app.canvas.measure_height(sub, subStyle, vw);
                                if (d) {
                                    const int tone = approved ? 0 : 1;
                                    const Color c = col(tone == 0 ? app.c->success : app.c->warning);
                                    app.canvas.path(approved ? kIconCheck : kIconWarn, {vx, vy + 2, 16, 16}, 16, c, 1.6f);
                                    app.canvas.text(head, t::kBodyMedium, {vx + 22, vy, vw - 22, 20}, c);
                                    app.canvas.text(sub, subStyle, {vx, vy + 24, vw, sh + 2}, col(app.c->label_secondary),
                                                    Align::Leading, false, true);
                                }
                                return 24 + sh;
                            }});
            const int n = static_cast<int>(i);
            rows.push_back({tr(L"Online"), [&st, n](float vx, float vy, float vw, bool d) {
                                const bool approved = st.offline.trust == ModTrust::Community;
                                if (!st.facts.signatureTrusted || !approved) {
                                    const std::wstring text = !st.facts.signatureTrusted
                                                                  ? tr(L"Never runs while a game’s anti-cheat is active")
                                                                  : tr(L"Approve it first.");
                                    const float th = app.canvas.measure_height(text, t::kBodyMedium, vw);
                                    if (d)
                                        app.canvas.text(text, t::kBodyMedium, {vx, vy, vw, th + 2}, col(app.c->label),
                                                        Align::Leading, false, true);
                                    return th;
                                }
                                const std::wstring text = st.approval.online
                                                              ? tr(L"Allowed while a game’s anti-cheat is active. It may kick or ban you.")
                                                              : tr(L"Off while a game’s anti-cheat is active.");
                                const float tw = vw - kSwitchW - 12;
                                const float th = app.canvas.measure_height(text, t::kCallout, tw);
                                if (d) {
                                    app.canvas.text(text, t::kCallout, {vx, vy, tw, th + 2}, col(app.c->label), Align::Leading,
                                                    false, true);
                                    const Rect s{vx + vw - kSwitchW, vy - 2, kSwitchW, kSwitchH};
                                    switch_track(s, st.approval.online, false, pointer_in(s) ? 1 : 0);
                                    const Ctl id = static_cast<Ctl>(kModOnline0 + n);
                                    app.hits.push_back({id, s, -1, {}, tr(L"Allow online"), st.approval.online ? 1 : 0});
                                    if (focused(id))
                                        focus_ring(s, t::kRadiusSwitch);
                                }
                                return std::max(th, kSwitchH - 4);
                            }});
        }
        if (own && m.error.empty()) {
            const int n = static_cast<int>(i);
            rows.push_back({tr(L"Updates"), [&m, n](float vx, float vy, float vw, bool d) {
                                const auto it = app.modReleases.find(m.id);
                                const bool newer = update_available(static_cast<size_t>(n));
                                std::wstring folder, backupVersion;
                                const bool backup = find_mod_backup(mods_folder_path(), m.id, folder, backupVersion);
                                const std::wstring text = newer ? trf(L"Version {version} is available.", {{L"version", it->second.version}})
                                                          : !app.modsChecked ? tr(L"Not checked yet: Check for Updates.")
                                                          : it != app.modReleases.end() ? tr(L"Up to date.")
                                                                                        : tr(L"None released yet.");
                                if (d) {
                                    app.canvas.text(text, t::kCallout, {vx, vy, vw, 20}, col(app.c->label));
                                    float right = vx + vw;
                                    if (newer)
                                        right -= small_button(static_cast<Ctl>(kModInstall0 + n), right, vy, tr(L"Install"), true) + 6;
                                    if (backup)
                                        small_button(static_cast<Ctl>(kModRestore0 + n), right, vy,
                                                     trf(L"Go back to {version}", {{L"version", backupVersion}}), true);
                                }
                                return 20.0f;
                            }});
        }
        if (!m.settings.empty())
            rows.push_back({tr(L"Settings"), [](float vx, float vy, float vw, bool d) {
                                return link_text(kModDetailSettings, vx, vy, vw, tr(L"Open on the Game tab ›"), d);
                            }});
        if (!rows.empty()) {
            float ch = 0;
            for (size_t k = 0; k < rows.size(); ++k)
                ch += detail_row(x, 0, w, false, k == 0, rows[k].first, rows[k].second);
            if (draw) {
                const Rect r{x, y + h, w, ch};
                cv.fill(r, col(app.c->group_bg), 8);
                cv.stroke(r.inset(0.5f, 0.5f), col(app.c->card_border), 1, 8);
                float ry = y + h;
                for (size_t k = 0; k < rows.size(); ++k)
                    ry += detail_row(x, ry, w, true, k == 0, rows[k].first, rows[k].second);
            }
            h += ch + 12;
        }
    }
    // Technical details: open or closed.
    {
        const std::wstring head = tr(L"Technical details");
        if (draw) {
            const Rect r{x, y + h, w, 24};
            cv.path(app.modDetailTech ? kIconChevronDown : kIconChevronRight, {r.x + 2, r.y + 6, 12, 12}, 16,
                    col(app.c->label_secondary), 1.6f);
            cv.text(head, t::kBodyStrong, {r.x + 20, r.y, r.w - 20, 24}, col(app.c->label));
            const float hw = 20 + std::ceil(cv.measure(head, t::kBodyStrong));
            const Rect hit{r.x, r.y, hw, 24};
            app.hits.push_back({kModDetailTech, hit, -1, {}, head, app.modDetailTech ? 1 : 0});
            if (focused(kModDetailTech))
                focus_ring(hit.inset(-2, 0), t::kRadiusControl);
        }
        h += 24 + 8;
    }
    if (app.modDetailTech) {
        using Value = std::function<float(float, float, float, bool)>;
        std::vector<std::pair<std::wstring, Value>> rows;
        std::wstring built;
        if (!m.sdk.empty())
            built = trf(L"SDK {version}", {{L"version", m.sdk}});
        if (m.abi > 0)
            built += (built.empty() ? L"" : L" · ") + trf(L"ABI {abi}", {{L"abi", std::to_wstring(m.abi)}});
        if (!built.empty())
            rows.push_back({tr(L"Built with"), [built](float vx, float vy, float vw, bool d) {
                                if (d)
                                    app.canvas.text(fit(built, t::kValue, vw), t::kValue, {vx, vy, vw, 20}, col(app.c->label));
                                return 20.0f;
                            }});
        if (!st.facts.sha256.empty())
            rows.push_back({tr(L"File hash"), [&st](float vx, float vy, float vw, bool d) {
                                if (d) {
                                    const float bw = small_button(kModDetailCopy, vx + vw, vy, tr(L"Copy"), true);
                                    app.canvas.text(fit(short_hash(st.facts.sha256) + L"…", t::kValue, vw - bw - 8), t::kValue,
                                                    {vx, vy, vw - bw - 8, 20}, col(app.c->label));
                                }
                                return 20.0f;
                            }});
        rows.push_back({tr(L"Signature"), [&st](float vx, float vy, float vw, bool d) {
                            if (d)
                                app.canvas.text(fit(signed_line(st), t::kCallout, vw), t::kCallout, {vx, vy, vw, 20},
                                                col(app.c->label));
                            return 20.0f;
                        }});
        if (!m.source.empty())
            rows.push_back({tr(L"Source"), [&m](float vx, float vy, float vw, bool d) {
                                link_text(kModDetailSource, vx, vy, vw, source_shown(m.source), d);
                                if (!m.license.empty() && d)
                                    app.canvas.text(fit(m.license, t::kSubheadline, vw), t::kSubheadline, {vx, vy + 20, vw, 17},
                                                    col(app.c->label_secondary));
                                return m.license.empty() ? 20.0f : 37.0f;
                            }});
        rows.push_back({tr(L"Folder"), [&m](float vx, float vy, float vw, bool d) {
                            if (d) {
                                const float bw = small_button(kModDetailFolder, vx + vw, vy, tr(L"Show in Explorer"), true);
                                app.canvas.text(fit_left(m.folder, t::kValue, vw - bw - 8), t::kValue, {vx, vy, vw - bw - 8, 20},
                                                col(app.c->label));
                            }
                            return 20.0f;
                        }});
        float ch = 0;
        for (size_t k = 0; k < rows.size(); ++k)
            ch += detail_row(x, 0, w, false, k == 0, rows[k].first, rows[k].second);
        if (draw) {
            const Rect r{x, y + h, w, ch};
            cv.fill(r, col(app.c->group_bg), 8);
            cv.stroke(r.inset(0.5f, 0.5f), col(app.c->card_border), 1, 8);
            float ry = y + h;
            for (size_t k = 0; k < rows.size(); ++k)
                ry += detail_row(x, ry, w, true, k == 0, rows[k].first, rows[k].second);
        }
        h += ch;
    }
    return h;
}

}  // namespace

// A mod's Details, over the Settings panel (app.modDetail): its header, the scrolled content, and the footer's buttons.
// Only it answers while open.
void mod_details_overlay() {
    if (app.panel != PanelKind::Settings || app.modDetail < 0)
        return;
    const size_t i = static_cast<size_t>(app.modDetail);
    if (i >= app.mods.size() || i >= app.modStatus.size()) {
        app.modDetail = -1;
        return;
    }
    const ModEntry& m = app.mods[i];
    const ModStatus& st = app.modStatus[i];
    const bool own = mod_api::first_party_mod(m.id);
    Canvas& cv = app.canvas;
    const float W = cv.width(), H = cv.height();
    cv.fill({0, 0, W, H}, {0, app.dark ? 0.45f : 0.25f});
    app.hits.clear();
    const float w = std::min(520.0f, W - 32), pad = 20, inner = w - 2 * pad;
    const float headerH = 72, footerH = 64;
    const float contentH = details_content(i, 0, 0, inner, false);
    const float h = std::min(headerH + contentH + 16 + footerH, H - 32);
    const Rect card{std::round((W - w) / 2), std::round(std::max(16.0f, (H - h) / 2)), w, h};
    cv.fill(card, col(app.c->bg), 10);
    cv.stroke(card.inset(0.5f, 0.5f), col(app.c->field_border), 1, 10);
    // Header: the tile, the name, who made it; the close button.
    icon_tile({card.x + pad, card.y + 18, 36, 36});
    const std::wstring name = m.name.empty() ? m.id : m.name;
    cv.text(fit(name, t::kTitle3, inner - 90), t::kTitle3, {card.x + pad + 48, card.y + 17, inner - 90, 20}, col(app.c->label));
    reader_text({card.x + pad + 48, card.y + 17, inner - 90, 20}, name, 1);
    std::wstring sub = trf(L"by {author}", {{L"author", m.author.empty() ? std::wstring(tr(L"an unknown author")) : m.author}});
    if (!m.version.empty())
        sub += L" · " + trf(L"v{version}", {{L"version", m.version}});
    cv.text(fit(sub, t::kSubheadline, inner - 90), t::kSubheadline, {card.x + pad + 48, card.y + 39, inner - 90, 17},
            col(app.c->label_secondary));
    const Rect close{card.right() - pad - 24, card.y + 18, 24, 24};
    if (pointer_in(close))
        cv.fill(close, col(app.c->control_hover), t::kRadiusControl);
    cv.path(kIconClose, {close.x + 5, close.y + 5, 14, 14}, 16, col(app.c->label_secondary), 1.6f);
    app.hits.push_back({kModDetailClose, close, -1, tr(L"Close (Esc)"), tr(L"Close")});
    if (focused(kModDetailClose))
        focus_ring(close, t::kRadiusControl);
    // The content, scrolled.
    const Rect body{card.x + pad, card.y + headerH, inner, h - headerH - footerH - 8};
    app.modDetailScrollMax = std::max(0.0f, contentH - body.h);
    app.modDetailScroll = std::clamp(app.modDetailScroll, 0.0f, app.modDetailScrollMax);
    cv.push_clip(body);
    details_content(i, body.x, body.y - app.modDetailScroll, body.w, true);
    cv.pop_clip();
    // Footer: Remove Mod… (not Sim-QVFR's own) leading; View Source and the main button trailing.
    const float fy = card.bottom() - footerH;
    cv.line(card.x, fy + 0.5f, card.right(), fy + 0.5f, col(app.c->separator), 1);
    const float by = fy + (footerH - t::kControlHeight) / 2;
    if (!own) {
        const std::wstring label = tr(L"Remove Mod…");
        button(kModDetailRemove, {card.x + pad - 8, by, button_width(label), t::kControlHeight}, label, Btn::Danger);
    }
    const bool approveFirst = m.error.empty() && st.approvable && st.offline.trust != ModTrust::Community;
    const std::wstring main = approveFirst ? (st.approval.sha256.empty() ? tr(L"Approve…") : tr(L"Approve Again…")) : tr(L"Done");
    float bx = card.right() - pad - button_width(main);
    button(approveFirst ? kModDetailApprove : kModDetailClose, {bx, by, button_width(main), t::kControlHeight}, main, Btn::Primary);
    if (!m.source.empty()) {
        const std::wstring label = tr(L"View Source");
        const float bw = button_width(label);
        bx -= bw + 8;
        const Rect r{bx, by, bw, t::kControlHeight};
        button(kModDetailSource, r, label, Btn::Secondary);
        cv.stroke(r.inset(0.5f, 0.5f), col(app.c->control_border), 1, t::kRadiusControl);
    }
}

void open_mod_details(size_t i) {
    app.modDetail = static_cast<int>(i);
    app.modDetailScroll = 0;
    const ModStatus& st = app.modStatus[i];
    // Open when there is nothing to act on; closed while the mod needs the user (its banner and buttons come first).
    app.modDetailTech = st.offline.trust != ModTrust::Refused;
    app.focus = kModDetailClose;
    app.focusOption = -1;
    InvalidateRect(app.hwnd, nullptr, FALSE);
}

void close_mod_details() {
    if (app.modDetail < 0)
        return;
    const int i = app.modDetail;
    app.modDetail = -1;
    app.focus = static_cast<Ctl>(kModDetails0 + i);
    app.focusOption = -1;
    InvalidateRect(app.hwnd, nullptr, FALSE);
}

namespace {

// The details' own buttons (mods_press).
bool details_press(Ctl id) {
    if (app.modDetail < 0 || static_cast<size_t>(app.modDetail) >= app.mods.size())
        return false;
    const size_t i = static_cast<size_t>(app.modDetail);
    const ModEntry& m = app.mods[i];
    switch (id) {
    case kModDetailClose: close_mod_details(); return true;
    case kModDetailTech:
        app.modDetailTech = !app.modDetailTech;
        InvalidateRect(app.hwnd, nullptr, FALSE);
        return true;
    case kModDetailCopy:
        if (i < app.modStatus.size()) {
            const std::string& sha = app.modStatus[i].facts.sha256;
            copy_text(std::wstring(sha.begin(), sha.end()));
        }
        return true;
    case kModDetailFolder: ShellExecuteW(app.hwnd, L"explore", m.folder.c_str(), nullptr, nullptr, SW_SHOWNORMAL); return true;
    case kModDetailSource: mods_press(static_cast<Ctl>(kModSource0 + static_cast<int>(i)), -1); return true;
    case kModDetailRemove:
        close_mod_details();  // the list changes under it
        remove_mod(i);
        return true;
    case kModDetailApprove: approve(i); return true;
    case kModDetailSettings: {
        // The Game tab of a game it covers.
        // The Game tab, of a game it covers when the shown one isn't.
        const std::wstring shown = games::wide_id(games::launcher_game());
        const bool covers = std::any_of(m.games.begin(), m.games.end(),
                                        [&](const std::wstring& g) { return _wcsicmp(g.c_str(), shown.c_str()) == 0; });
        int index = -1;
        if (!covers && !m.games.empty()) {
            std::size_t count = 0;
            const games::GameInfo* const* all = games::all_games(count);
            for (std::size_t k = 0; k < count; ++k)
                if (_wcsicmp(games::wide_id(*all[k]).c_str(), m.games.front().c_str()) == 0)
                    index = static_cast<int>(k);
        }
        app.modDetail = -1;
        close_panel();
        if (index >= 0)
            select_game(index);
        activate(kPage, static_cast<int>(Pane::Game));
        return true;
    }
    default: return false;
    }
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
    if (details_press(id))
        return true;
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
            open_mod_details(i);
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
    // Header: "Mods" and what approval means; Rescan, Open Folder, Check for Updates, Add Mod… trailing.
    const std::wstring title = tr(L"Mods");
    const std::wstring intro = tr(L"Mods from anyone other than Sim-QVFR stay off until you approve them.");
    const std::vector<Body::ButtonSpec> tools = {{kModsReload, tr(L"Rescan"), Btn::Ghost, kIconRescan},
                                                 {kModsOpenFolder, tr(L"Open Folder"), Btn::Ghost, kIconFolder},
                                                 {kModsCheck, tr(L"Check for Updates"), Btn::Secondary},
                                                 {kModsAdd, tr(L"Add Mod…"), Btn::Primary, kIconPlus}};
    float toolsW = 0;
    for (const Body::ButtonSpec& s : tools)
        toolsW += button_width(s.label) + (s.icon ? 24.0f : 0.0f) + (toolsW > 0 ? 6.0f : 0.0f);
    const bool oneLine = w - toolsW > 220;
    const float textW = oneLine ? w - toolsW - 16 : w;
    const float introH = app.canvas.measure_height(intro, t::kCallout, textW);
    const float headH = std::max(oneLine ? t::kControlHeight : 0.0f, t::kHeadline.lineHeight + 2 + introH);
    b.custom(headH + (oneLine ? 0 : 8 + t::kControlHeight), [&](const Rect& r) {
        app.canvas.text(title, t::kHeadline, {r.x, r.y, textW, t::kHeadline.lineHeight}, col(app.c->label));
        reader_text({r.x, r.y, textW, t::kHeadline.lineHeight}, title, 2);
        app.canvas.text(intro, t::kCallout, {r.x, r.y + t::kHeadline.lineHeight + 2, textW, introH + 2}, col(app.c->label_secondary),
                        Align::Leading, false, true);
        reader_text({r.x, r.y + t::kHeadline.lineHeight + 2, textW, introH}, intro);
        float bx = oneLine ? r.right() - toolsW : r.x;
        const float by = oneLine ? r.y + (headH - t::kControlHeight) / 2 : r.y + headH + 8;
        for (const Body::ButtonSpec& s : tools) {
            const float bw = button_width(s.label) + (s.icon ? 24.0f : 0.0f);
            const Rect br{bx, by, bw, t::kControlHeight};
            button(s.id, br, s.label, s.style, s.icon);
            if (s.style == Btn::Secondary)
                app.canvas.stroke(br.inset(0.5f, 0.5f), col(app.c->control_border), 1, t::kRadiusControl);
            bx += bw + 6;
        }
    });
    if (app.modsBusy)
        b.note(tr(L"Working…"));
    else if (!app.modsMessage.empty())
        b.chip(app.modsTone, app.modsMessage);
    // Built in (Sim-QVFR's own), then third-party.
    std::vector<size_t> own, others;
    for (size_t i = 0; i < app.mods.size() && i < app.modStatus.size(); ++i)
        (mod_api::first_party_mod(app.mods[i].id) ? own : others).push_back(i);
    std::wstring ownCaption;
    if (!own.empty()) {
        const ModEntry& first = app.mods[own.front()];
        if (!first.version.empty())
            ownCaption = trf(L"v{version}", {{L"version", first.version}});
        if (!first.sdk.empty())
            ownCaption += (ownCaption.empty() ? L"" : L" · ") + trf(L"SDK {version}", {{L"version", first.sdk}});
    }
    mod_group(b, tr(L"Built in"), ownCaption, own);
    mod_group(b, tr(L"Third-party"), tr(L"Not made by Sim-QVFR"), others);
    // The drop zone: a zip dropped anywhere on the window adds it; a click chooses one.
    b.custom(56, [](const Rect& r) {
        const bool over = pointer_in(r);
        if (over)
            app.canvas.fill(r, col(app.c->control_hover, 0.5f), 8);
        // A dashed edge: short strokes along each side.
        const Color edge = col(over ? app.c->accent : app.c->control_border);
        for (float px = r.x + 8; px < r.right() - 8; px += 10) {
            app.canvas.line(px, r.y + 0.5f, std::min(px + 5, r.right() - 8), r.y + 0.5f, edge, 1);
            app.canvas.line(px, r.bottom() - 0.5f, std::min(px + 5, r.right() - 8), r.bottom() - 0.5f, edge, 1);
        }
        for (float py = r.y + 8; py < r.bottom() - 8; py += 10) {
            app.canvas.line(r.x + 0.5f, py, r.x + 0.5f, std::min(py + 5, r.bottom() - 8), edge, 1);
            app.canvas.line(r.right() - 0.5f, py, r.right() - 0.5f, std::min(py + 5, r.bottom() - 8), edge, 1);
        }
        const std::wstring text = tr(L"Drop a mod’s .zip anywhere in this window to add it");
        const float tw = std::ceil(app.canvas.measure(text, t::kCallout));
        const float x0 = r.x + (r.w - tw - 22) / 2;
        app.canvas.path(kIconUpload, {x0, r.y + (r.h - 14) / 2, 14, 14}, 16, col(app.c->label_secondary), 1.5f);
        app.canvas.text(text, t::kCallout, {x0 + 22, r.y, tw + 2, r.h}, col(app.c->label_secondary));
        app.hits.push_back({kModsAdd, r, -1, {}, text});
    });
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
