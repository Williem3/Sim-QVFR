// The pointer: what's under it, dragging sliders and the menu panel, popups, the game tabs, and the folder
// and runtime pickers.
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

#include <windows.h>
#include <commdlg.h>
#include <shlobj.h>

#include "../launcher.h"
#include "../launch_plan.h"
#include "../preflight.h"
#include "../report.h"
#include "../settings.h"
#include "../../games/game.h"
#include "window_state.h"
#include "card.h"

#pragma comment(lib, "comdlg32.lib")

namespace simqvfr::launcher::ui::win {

// ---- Input -------------------------------------------------------------------------------------

const Hit* hit_at(float x, float y) {
    for (auto it = app.hits.rbegin(); it != app.hits.rend(); ++it)
        if (it->id != kText && it->r.contains(x, y))
            return &*it;
    return nullptr;
}

void set_slider_from_x(float x) {
    int* v = slider_value(app.dragging);
    if (!v)
        return;
    const float travel = app.dragTrack.w - 18;
    const float f = std::clamp((x - app.dragTrack.x - 9) / travel, 0.0f, 1.0f);
    const int before = *v;
    // Inner resolution in its steps (100% reachable); the arrow keys still move every slider by 1.
    *v = slider_value_at(app.dragRange, f, app.dragging == kInner ? kInnerDragStep : 1);
    if (app.dragging == kColorValue)
        apply_color_edit();  // the brightness: the edited game's colour follows
    changed(false);
    if (*v != before && app.iniFound)
        push_live();  // the running game follows the knob; the settings save once it is let go
}

// The place a preview drag or the arrows change: the settings menu's or the performance overlay's.
std::pair<int*, int*> placement_of(Ctl id) {
    return id == kOsdPanel ? std::pair{&app.s.osd.yaw, &app.s.osd.pitch} : std::pair{&app.s.menuYaw, &app.s.menuPitch};
}

// Dragging a panel in the placement preview: its centre follows the pointer (less where it was held).
void set_panel_from(float x, float y) {
    const float k = app.placeFrame.w / kRefFovX;
    if (k <= 0)
        return;
    const auto [yawOf, pitchOf] = placement_of(app.dragging);
    const int yaw = std::clamp(static_cast<int>(std::lround((x - app.placeFrame.x) / k - kRefFovX / 2 + app.grabYaw)),
                               kMenuYawRange.min, kMenuYawRange.max);
    const int pitch = std::clamp(static_cast<int>(std::lround(kRefFovY / 2 - (y - app.placeFrame.y) / k + app.grabPitch)),
                                 kMenuPitchRange.min, kMenuPitchRange.max);
    const bool moved = yaw != *yawOf || pitch != *pitchOf;
    *yawOf = yaw;
    *pitchOf = pitch;
    changed(false);
    if (moved && app.iniFound)
        push_live();  // the running game moves it as it is dragged; the settings save once it is let go
}

// A choice's items and the one chosen now: a pop-up's, or a segmented control's (they share them).
int choice_items(Ctl owner, std::vector<std::wstring>& items) {
    int selected = 0;
    items.clear();
    if (owner == kOverviewProfile)
        return overview_profile(items);
    if (owner == kCheekyOuterPreset || owner == kCheekyFocusPreset) {  // Cheeky's presets (the outer views have no game default)
        const bool focus = owner == kCheekyFocusPreset;
        const int value = focus ? app.s.cheeky.focusPreset : app.s.cheeky.outerPreset;
        items.clear();
        selected = 0;
        for (const CheekyPreset& p : kCheekyPresets)
            if (focus || p.value != 0) {
                if (p.value == value)
                    selected = static_cast<int>(items.size());
                items.push_back(p.name);
            }
    } else if (owner == kDlssFocusMode || owner == kDlssFocusPreset || owner == kDlssEyePreset ||
               owner == kDlssSharpening) {  // DLSS in LMU
        const auto fill = [&items, &selected](const auto& list, int value) {
            items.clear();
            for (const DlssChoice& c : list)
                items.push_back(c.name);
            selected = dlss_choice_index(list, value, true);
        };
        if (owner == kDlssFocusMode)
            fill(kDlssFocusModes, app.s.dlss.focusScale);
        else if (owner == kDlssFocusPreset)
            fill(kDlssFocusPresets, app.s.dlss.focusPreset);
        else if (owner == kDlssEyePreset)
            fill(kDlssEyePresets, app.s.dlss.eyePreset);
        else
            fill(kDlssTextureSharpening, app.s.dlss.textureSharpening);
    } else if (owner == kNrStyle || owner == kNrPreset) {  // DLSS-NR in LMU
        const bool style = owner == kNrStyle;
        items.clear();
        if (style)
            for (const DlssChoice& c : kNrStyles)
                items.push_back(c.name);
        else
            for (const DlssChoice& c : kNrPresets)
                items.push_back(c.name);
        selected = style ? dlss_choice_index(kNrStyles, app.s.nr.style) : dlss_choice_index(kNrPresets, app.s.nr.preset);
    } else if (owner == kNrOrder) {
        items = {L"After DLSS", L"Before DLSS"};
        selected = app.s.nr.beforeDlss ? 1 : 0;
    } else if (owner == kCheekyNrPreset) {
        items.assign(std::begin(kCheekyNrPresets), std::end(kCheekyNrPresets));
        selected = app.s.cheeky.nrPreset;
    } else if (owner == kFgMultiplier) {
        items.assign(std::begin(kFgMultipliers), std::end(kFgMultipliers));
        selected = std::clamp(app.s.cheeky.fgMultiplier - 2, 0, 2);
    } else if (owner == kFgRelease) {
        items.assign(std::begin(kCheekyFgReleaseNames), std::end(kCheekyFgReleaseNames));
        selected = cheeky_fg_release_index(app.s.cheeky.fgRelease);
    } else if (owner == kFgScale || owner == kFgPeriphery) {
        items.assign(std::begin(kFgQuarters), std::end(kFgQuarters));
        selected = std::clamp((owner == kFgScale ? app.s.cheeky.fgScale : app.s.cheeky.fgPeriphery) / 25 - 1, 0, 3);
    } else if (owner == kVisor) {
        items.assign(std::begin(kVisorPositionNames), std::end(kVisorPositionNames));
        selected = static_cast<int>(app.s.helmet.visor);
    } else if (owner == kShape) {
        items.assign(std::begin(kShapeNames), std::end(kShapeNames));
        selected = app.s.shape.corners ? 1 : 0;
    } else if (owner == kDiagKind) {
        items.assign(std::begin(kIssueKinds), std::end(kIssueKinds));
        selected = app.reportKind;
    } else if (owner == kHelmetDetail) {
        items = {L"Performance", L"Detail"};  // 2048 or 4096 a side: the help says so
        selected = app.s.helmet.detail ? 1 : 0;
    } else if (owner == kLayerProfile) {
        items = {kLayerProfileNames[0], kLayerProfileNames[1]};
        selected = app.layerProfile;
    } else if (owner == kCheekyNrPasses) {
        items = {L"1 pass", L"2 passes", L"3 passes"};
        selected = app.s.cheeky.nrPasses - 1;
    } else {
        items = owner == kMethod ? std::vector<std::wstring>{kMethodNames[0], kMethodNames[1]}
                                           : std::vector<std::wstring>{kModeNames[0], kModeNames[1]};
        selected = owner == kMethod ? (app.s.mode == RenderMode::Qvfr ? 0 : 1) : (app.s.eyeTracking ? 0 : 1);
    }
    return selected;
}

// The choices shown as segmented controls (card.cpp); the others open a pop-up menu.
bool segmented_ctl(Ctl id) {
    switch (id) {
    case kMode:
    case kShape:
    case kVisor:
    case kHelmetDetail:
    case kDlssFocusPreset:
    case kDlssEyePreset:
    case kNrOrder:
    case kFgMultiplier:
    case kFgScale:
    case kFgPeriphery:
    case kLayerProfile:
    case kOverviewProfile: return true;
    default: return false;
    }
}

void open_popup(Ctl owner) {
    if (owner == kProfileMenu) {
        open_profile_menu();
        return;
    }
    app.popup.kinds.clear();
    app.popup.actions.clear();
    for (const Hit& h : app.hits)
        if (h.id == owner) {
            app.popup.anchor = h.r;
            break;
        }
    app.popup.owner = owner;
    app.popup.selected = choice_items(owner, app.popup.items);
    app.popup.hover = app.popup.selected;
    app.popup.open = true;
    InvalidateRect(app.hwnd, nullptr, FALSE);
}

// A card's ⋯ menu (option: the card's index this frame): Reset this card.
void open_card_menu(int card) {
    if (card < 0 || card >= static_cast<int>(app.cardResets.size()))
        return;
    Popup& p = app.popup;
    p = {};
    p.owner = kCardMenu;
    for (const Hit& h : app.hits)
        if (h.id == kCardMenu && h.option == card)
            p.anchor = h.r;
    p.items = {L"Reset this card…"};
    p.actions = {card};
    p.selected = -1;
    p.hover = 0;
    p.open = true;
    InvalidateRect(app.hwnd, nullptr, FALSE);
}

namespace {

// The profile menu's actions besides a profile (its index in app.profileNames, 0 and up).
enum ProfileAction { kActUpdate = -1, kActRevert = -2, kActSave = -3, kActPaste = -4, kActManage = -5, kActRestore = -6 };

int profile_index(const std::wstring& name) {
    for (size_t i = 0; i < app.profileNames.size(); ++i)
        if (_wcsicmp(app.profileNames[i].c_str(), name.c_str()) == 0)
            return static_cast<int>(i);
    return -1;
}

}  // namespace

// The top bar's profile menu: what the settings are now, the presets and saved profiles (the one in use checked), the
// one in use's Update and Revert once the settings differ from it, saving and pasting, the Profiles page, and Restore
// Defaults (its Undo stays in the status and Ctrl+Z).
void open_profile_menu() {
    Popup& p = app.popup;
    p = {};
    p.owner = kProfileMenu;
    for (const Hit& h : app.hits)
        if (h.id == kProfileMenu)
            p.anchor = h.r;
    const auto add = [&p](PopupRow kind, const std::wstring& text, int action) {
        p.items.push_back(text);
        p.kinds.push_back(kind);
        p.actions.push_back(action);
    };
    const Readouts ro = readouts(app.s, app.runtime);
    add(PopupRow::Note,
        std::wstring(app.s.eyeTracking ? L"Eye tracked" : L"Fixed") + L"  ·  Outer " + std::to_wstring(app.s.outer) + L"%  ·  Focus " +
            std::to_wstring(static_cast<int>(std::lround(ro.focusDensity * 100))) + L"%",
        0);
    add(PopupRow::Separator, L"", 0);
    const int current = profile_index(app.profileCurrent);
    const size_t presets = builtin_profiles(app.s.eyeTracking).size();
    p.selected = -1;
    for (size_t i = 0; i < app.profileNames.size(); ++i) {
        if (i == presets && presets > 0)
            add(PopupRow::Separator, L"", 0);
        const bool inUse = static_cast<int>(i) == current;
        if (inUse)
            p.selected = static_cast<int>(p.items.size());
        add(PopupRow::Item, app.profileNames[i] + (inUse && !app.profileMatches ? L"  ·  changed since" : L""), static_cast<int>(i));
    }
    add(PopupRow::Separator, L"", 0);
    if (current >= 0 && !app.profileMatches) {
        if (static_cast<size_t>(current) >= presets)
            add(PopupRow::Item, L"Update " + app.profileCurrent + L" with these settings", kActUpdate);
        add(PopupRow::Item, L"Revert to " + app.profileCurrent, kActRevert);
    }
    add(PopupRow::Item, L"Save current settings…", kActSave);
    add(PopupRow::Item, L"Paste a shared profile", kActPaste);
    add(PopupRow::Item, L"Manage profiles…", kActManage);
    add(PopupRow::Separator, L"", 0);
    add(PopupRow::Item, L"Restore defaults for " + short_name(), kActRestore);
    p.hover = p.selected;
    p.open = true;
    InvalidateRect(app.hwnd, nullptr, FALSE);
}

namespace {

void profile_menu_action(int option) {
    if (option < 0 || option >= static_cast<int>(app.popup.actions.size()))
        return;
    const int action = app.popup.actions[static_cast<size_t>(option)], current = profile_index(app.profileCurrent);
    switch (action) {
    case kActUpdate: activate(kProfileUpdate, current); break;
    case kActRevert: activate(kProfileRevert, current); break;
    case kActSave: activate(kProfileSave, -1); break;
    case kActPaste: activate(kProfilePaste, -1); break;
    case kActManage: open_panel(PanelKind::Profiles); break;
    case kActRestore: activate(kRestore, -1); break;
    default: activate(kProfileUse, action); break;
    }
}

void choose(Ctl owner, int option) {
    if (owner == kProfileMenu) {  // actions, not settings: each saves as it would from its own button
        profile_menu_action(option);
        return;
    }
    if (owner == kCardMenu) {
        if (option >= 0 && option < static_cast<int>(app.popup.actions.size()))
            reset_card(app.popup.actions[static_cast<size_t>(option)]);
        return;
    }
    if (owner == kDiagKind) {  // not a setting
        app.reportKind = option;
        return;
    }
    if (owner == kOverviewProfile) {  // a preset used, or the profile menu
        choose_overview_profile(option);
        return;
    }
    if (owner == kMethod)
        app.s.mode = option == 0 ? RenderMode::Qvfr : RenderMode::TwoLoop;
    else if (owner == kMode)
        app.s.eyeTracking = option == 0;
    else if (owner == kCheekyFocusPreset)
        app.s.cheeky.focusPreset = kCheekyPresets[option].value;
    else if (owner == kCheekyOuterPreset)
        app.s.cheeky.outerPreset = kCheekyPresets[option + 1].value;  // the list without the game default
    else if (owner == kDlssFocusMode)
        app.s.dlss.focusScale = kDlssFocusModes[std::clamp(option, 0, static_cast<int>(std::size(kDlssFocusModes)) - 1)].value;
    else if (owner == kDlssFocusPreset)
        app.s.dlss.focusPreset = kDlssFocusPresets[std::clamp(option, 0, static_cast<int>(std::size(kDlssFocusPresets)) - 1)].value;
    else if (owner == kDlssEyePreset)
        app.s.dlss.eyePreset = kDlssEyePresets[std::clamp(option, 0, static_cast<int>(std::size(kDlssEyePresets)) - 1)].value;
    else if (owner == kDlssSharpening)
        app.s.dlss.textureSharpening =
            kDlssTextureSharpening[std::clamp(option, 0, static_cast<int>(std::size(kDlssTextureSharpening)) - 1)].value;
    else if (owner == kNrStyle)
        app.s.nr.style = kNrStyles[std::clamp(option, 0, static_cast<int>(std::size(kNrStyles)) - 1)].value;
    else if (owner == kNrPreset)
        app.s.nr.preset = kNrPresets[std::clamp(option, 0, static_cast<int>(std::size(kNrPresets)) - 1)].value;
    else if (owner == kNrOrder)
        app.s.nr.beforeDlss = option == 1;
    else if (owner == kCheekyNrPreset)
        app.s.cheeky.nrPreset = option;
    else if (owner == kCheekyNrPasses)
        app.s.cheeky.nrPasses = option + 1;
    else if (owner == kFgMultiplier)
        app.s.cheeky.fgMultiplier = option + 2;
    else if (owner == kFgScale)
        app.s.cheeky.fgScale = (option + 1) * 25;
    else if (owner == kFgPeriphery)
        app.s.cheeky.fgPeriphery = (option + 1) * 25;
    else if (owner == kFgRelease)
        app.s.cheeky.fgRelease = kCheekyFgReleasePoints[std::clamp(option, 0, 2)];
    else if (owner == kVisor)
        app.s.helmet.visor = static_cast<VisorPosition>(std::clamp(option, 0, 2));
    else if (owner == kHelmetDetail)
        app.s.helmet.detail = option == 1;
    else if (owner == kShape)
        app.s.shape.corners = option == 1;
    else if (owner == kLayerProfile) {
        app.layerProfile = std::clamp(option, 0, 1);
        app.layerRow = 0;
        app.layerMessage.clear();
        InvalidateRect(app.hwnd, nullptr, FALSE);
        return;
    }
    changed();
}

}  // namespace

void choose_option(Ctl owner, int option) { choose(owner, option); }

// A game tab: [launcher] game, then everything that follows the game (its checks, its features, its palette).
void select_game(int option) {
    std::size_t count = 0;
    const games::GameInfo* const* all = games::all_games(count);
    if (option < 0 || static_cast<std::size_t>(option) >= count || all[option] == &games::launcher_game())
        return;
    if (app.iniFound) {
        // Each game keeps its own settings: this one's go to its copy, the other's come back into the ini.
        switch_game_settings(app.ini, widen(games::launcher_game().id), widen(all[option]->id));
        WritePrivateProfileStringW(L"launcher", L"game", widen(all[option]->id).c_str(), app.ini.c_str());
        choose_launcher_game();
        app.s = load_settings(app.ini);
        app.iniText = ini_text();
        app.undoRestore.reset();  // it held the other game's settings
        refresh_profiles();  // the profile in use is the game's
    } else {
        games::set_launcher_game(*all[option]);  // for this window only: there is no ini to keep it in
    }
    if (!pane_shown(static_cast<int>(app.pane)))  // a page this game hasn't (DLSS): its tab's first
        show_tab(tab_of(app.pane));
    apply_theme();
    refresh_checks();
    InvalidateRect(app.hwnd, nullptr, FALSE);
}

int game_index() {
    std::size_t count = 0;
    const games::GameInfo* const* all = games::all_games(count);
    for (std::size_t i = 0; i < count; ++i)
        if (all[i] == &games::launcher_game())
            return static_cast<int>(i);
    return 0;
}

void close_popup(bool accept) {
    const Popup p = app.popup;  // choose may open a sheet, or another menu
    app.popup.open = false;
    if (accept && p.hover >= 0 && (p.kinds.empty() || p.kinds[static_cast<size_t>(p.hover)] == PopupRow::Item))
        choose(p.owner, p.hover);
    InvalidateRect(app.hwnd, nullptr, FALSE);
}

// The item at (x, y); -1 for none, a separator or a note.
int popup_item_at(float x, float y) {
    const Popup& p = app.popup;
    const Rect menu = popup_menu_rect();
    float top = menu.y + 6;
    for (size_t i = 0; i < p.items.size(); ++i) {
        const float h = popup_row_height(i);
        if (Rect{menu.x + 6, top, menu.w - 12, h}.contains(x, y))
            return p.kinds.empty() || p.kinds[i] == PopupRow::Item ? static_cast<int>(i) : -1;
        top += h;
    }
    return -1;
}

// Picks Cheeky's runtime DLL (from Cheeky's Standalone ZIP) and saves its path to [cheeky] runtime.
void choose_cheeky_runtime() {
    wchar_t path[MAX_PATH] = {};
    const std::wstring current = cheeky_runtime_path(app.ini, layer_folder());
    wcsncpy_s(path, current.c_str(), _TRUNCATE);
    OPENFILENAMEW ofn{sizeof(ofn)};
    ofn.hwndOwner = app.hwnd;
    ofn.lpstrFilter = L"Cheeky runtime (CheekyFoveatedDLSSRuntime.dll)\0CheekyFoveatedDLSSRuntime.dll\0DLL files\0*.dll\0";
    ofn.lpstrFile = path;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrTitle = L"Choose Cheeky Foveated DLSS's runtime";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (GetOpenFileNameW(&ofn) && app.iniFound) {
        save_cheeky_runtime(app.ini, path);
        app.iniText = ini_text();
    }
    InvalidateRect(app.hwnd, nullptr, FALSE);
}

// Sim-QVFR's own DLSS: another nvngx_dlss.dll than the one Sim-QVFR includes (a newer one from a game); its folder is
// saved as [dlss] folder (every game's), read by the game at its next launch. Choosing the included one saves none.
void choose_dlss_dll() {
    wchar_t path[MAX_PATH] = {};
    const std::wstring current = dlss_dll_path(app.s.dlss.folder, layer_folder());
    wcsncpy_s(path, current.c_str(), _TRUNCATE);
    OPENFILENAMEW ofn{sizeof(ofn)};
    ofn.hwndOwner = app.hwnd;
    ofn.lpstrFilter = L"NVIDIA DLSS (nvngx_dlss.dll)\0nvngx_dlss.dll\0";
    ofn.lpstrFile = path;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrTitle = L"Choose NVIDIA’s nvngx_dlss.dll (DLSS 310 or newer)";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (GetOpenFileNameW(&ofn)) {
        std::wstring folder = path;
        const size_t slash = folder.find_last_of(L"\\/");
        folder = slash == std::wstring::npos ? L"" : folder.substr(0, slash);
        std::error_code ec;
        if (!folder.empty() && std::filesystem::equivalent(folder, layer_folder(), ec))
            folder.clear();
        app.s.dlss.folder = folder;
        changed();
    }
    InvalidateRect(app.hwnd, nullptr, FALSE);
}

// DLSS-NR: the player's own nvngx_dlssnr.dll (a game that ships DLSS-NR has one; Sim-QVFR includes none). Its folder is
// saved as [dlss_nr] folder (every game's), read by the game at its next launch.
void choose_nr_dll() {
    wchar_t path[MAX_PATH] = {};
    if (!app.s.nr.folder.empty())
        wcsncpy_s(path, (app.s.nr.folder + L"\\nvngx_dlssnr.dll").c_str(), _TRUNCATE);
    OPENFILENAMEW ofn{sizeof(ofn)};
    ofn.hwndOwner = app.hwnd;
    ofn.lpstrFilter = L"NVIDIA DLSS-NR (nvngx_dlssnr.dll)\0nvngx_dlssnr.dll\0";
    ofn.lpstrFile = path;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrTitle = L"Choose NVIDIA’s nvngx_dlssnr.dll (from a game that ships DLSS-NR)";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (GetOpenFileNameW(&ofn)) {
        std::wstring folder = path;
        const size_t slash = folder.find_last_of(L"\\/");
        app.s.nr.folder = slash == std::wstring::npos ? L"" : folder.substr(0, slash);
        changed();
    }
    InvalidateRect(app.hwnd, nullptr, FALSE);
}

// Use included: back to the nvngx_dlss.dll Sim-QVFR includes.
void use_included_dlss() {
    app.s.dlss.folder.clear();
    changed();
    InvalidateRect(app.hwnd, nullptr, FALSE);
}

// Launch checks' Choose folder… (a game Steam doesn't list): the game's folder, saved as [launcher] game_dir_<id>.
void choose_game_folder() {
    const games::GameInfo& game = games::launcher_game();
    IFileOpenDialog* dialog = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog))))
        return;
    DWORD options = 0;
    dialog->GetOptions(&options);
    dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
    const std::wstring title = L"Choose " + game_name() + L"’s folder";
    dialog->SetTitle(title.c_str());
    std::wstring picked;
    if (SUCCEEDED(dialog->Show(app.hwnd))) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(dialog->GetResult(&item))) {
            PWSTR path = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) && path) {
                picked = path;
                CoTaskMemFree(path);
            }
            item->Release();
        }
    }
    dialog->Release();
    if (picked.empty())
        return;
    const std::wstring folder = game_folder_from(picked, game);
    if (folder.empty()) {
        notify(L"That folder doesn’t hold " + game_name(),
               picked + L" has no " + game.exeName + L". Choose the folder Steam’s Browse local files opens for " + game_name() + L".");
        return;
    }
    if (!app.iniFound || !WritePrivateProfileStringW(L"launcher", (L"game_dir_" + widen(game.id)).c_str(), folder.c_str(),
                                                     app.ini.c_str())) {
        notify(L"The folder couldn’t be saved", L"sim-qvfr.ini wasn’t found next to the launcher.");
        return;
    }
    app.iniText = ini_text();
    refresh_installed();  // found now
    if (app.status == Status::Error)
        app.status = Status::Idle;  // "not found: choose its folder" is answered
}

// Restore Defaults' Undo: the settings from before it.
void undo_restore() {
    if (!app.undoRestore)
        return;
    app.s = *app.undoRestore;
    changed();  // also ends the Undo
}

}  // namespace simqvfr::launcher::ui::win
