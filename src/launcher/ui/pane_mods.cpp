// The Game tab's Mods card: the installed game mods (mod_catalog.h), who made each, what the core's trust check will do
// with it (mod_api/mod_trust.h), the user's approval of a third-party mod and its Allow online, and the settings its
// settings.json declares. The launcher only reads the mods' files and hashes their DLLs; it never loads one.
#include <algorithm>
#include <filesystem>
#include <string>
#include <vector>

#include <windows.h>

#include "../launcher.h"
#include "../mod_catalog.h"
#include "../../mod_api/mod_trust.h"
#include "window_state.h"
#include "card.h"
#include "controls.h"

namespace simqvfr::launcher::ui::win {

namespace fs = std::filesystem;
using mod_api::ModTrust;

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

void mod_rows(Body& b, size_t i) {
    const ModEntry& m = app.mods[i];
    const ModStatus& st = app.modStatus[i];
    b.group(m.name);
    if (!m.error.empty()) {
        b.status(m.id, m.error, 2, tr(L"Won’t load"));
        return;
    }
    const auto [tone, text] = chip_of(st);
    std::wstring who = m.author.empty() ? tr(L"Unknown author") : m.author;
    if (!m.version.empty())
        who += L" · " + m.version;
    b.status(who, signed_line(st) + L" " + (st.offline.trust == ModTrust::Community || st.approvable
                                                  ? tr(L"Not made by Sim-QVFR.")
                                                  : std::wstring()),
             tone, text);
    if (st.approvable) {
        const bool approved = st.offline.trust == ModTrust::Community;
        if (!approved && !st.approval.sha256.empty())
            b.note(tr(L"Its file changed since you approved it: approve it again to use it."));
        b.action(approved ? tr(L"Approved") : tr(L"Approve this mod"),
                 approved ? trf(L"This exact file ({hash}…). If it changes, it stops loading until you approve it again.",
                                {{L"hash", short_hash(st.facts.sha256)}})
                          : tr(L"It runs inside the game. Approve it only if you trust who made it."),
                 static_cast<Ctl>(kModApprove0 + static_cast<int>(i)), approved ? tr(L"Take back") : tr(L"Approve…"),
                 -1, approved ? Btn::Secondary : Btn::Primary);
        if (approved) {
            const bool canOnline = st.facts.signatureTrusted;
            b.toggle(static_cast<Ctl>(kModOnline0 + static_cast<int>(i)), tr(L"Allow online"), st.approval.online,
                     !canOnline ? tr(L"Not signed by its author, so it never runs while a game’s anti-cheat runs.")
                     : st.approval.online
                         ? tr(L"On: it also runs while the game’s anti-cheat runs. The anti-cheat may kick or ban you for it.")
                         : tr(L"Off: it stays off while the game’s anti-cheat runs."),
                     !canOnline, Applies::NextLaunch);
        }
    } else if (st.offline.trust == ModTrust::Refused) {
        b.note(widen_reason(st.offline.why));
    }
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
    if (st.offline.trust != ModTrust::Community || !st.facts.signatureTrusted)
        return;
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

Card mods_card() {
    ensure_read();
    Card c{Tab::Game, tr(L"Mods")};
    c.body = [](Body& b) {
        if (app.mods.empty()) {
            b.note(tr(L"No game mods are installed (the mods folder beside the layer folder is empty)."));
        } else {
            for (size_t i = 0; i < app.mods.size() && i < app.modStatus.size(); ++i) {
                if (i > 0)
                    b.divider();
                mod_rows(b, i);
            }
        }
        b.actions({{kModsReload, tr(L"Look again")}});
    };
    return c;
}

}  // namespace simqvfr::launcher::ui::win
