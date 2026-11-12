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

#pragma comment(lib, "comdlg32.lib")

namespace simqvfr::launcher::ui::win {

// ---- Input -------------------------------------------------------------------------------------

const Hit* hit_at(float x, float y) {
    for (auto it = app.hits.rbegin(); it != app.hits.rend(); ++it)
        if (it->r.contains(x, y))
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

void open_popup(Ctl owner) {
    for (const Hit& h : app.hits)
        if (h.id == owner) {
            app.popup.anchor = h.r;
            break;
        }
    app.popup.owner = owner;
    if (owner == kCheekyOuterPreset || owner == kCheekyFocusPreset) {  // Cheeky's presets (the outer views have no game default)
        const bool focus = owner == kCheekyFocusPreset;
        const int value = focus ? app.s.cheeky.focusPreset : app.s.cheeky.outerPreset;
        app.popup.items.clear();
        app.popup.selected = 0;
        for (const CheekyPreset& p : kCheekyPresets)
            if (focus || p.value != 0) {
                if (p.value == value)
                    app.popup.selected = static_cast<int>(app.popup.items.size());
                app.popup.items.push_back(p.name);
            }
    } else if (owner == kDlssFocusMode || owner == kDlssFocusPreset || owner == kDlssEyePreset ||
               owner == kDlssSharpening) {  // DLSS in LMU
        const auto fill = [](const auto& list, int value) {
            app.popup.items.clear();
            for (const DlssChoice& c : list)
                app.popup.items.push_back(c.name);
            app.popup.selected = dlss_choice_index(list, value, true);
        };
        if (owner == kDlssFocusMode)
            fill(kDlssFocusModes, app.s.dlss.focusScale);
        else if (owner == kDlssFocusPreset)
            fill(kDlssFocusPresets, app.s.dlss.focusPreset);
        else if (owner == kDlssEyePreset)
            fill(kDlssEyePresets, app.s.dlss.eyePreset);
        else
            fill(kDlssTextureSharpening, app.s.dlss.textureSharpening);
    } else if (owner == kCheekyNrPreset) {
        app.popup.items.assign(std::begin(kCheekyNrPresets), std::end(kCheekyNrPresets));
        app.popup.selected = app.s.cheeky.nrPreset;
    } else if (owner == kFgMultiplier) {
        app.popup.items.assign(std::begin(kFgMultipliers), std::end(kFgMultipliers));
        app.popup.selected = std::clamp(app.s.cheeky.fgMultiplier - 2, 0, 2);
    } else if (owner == kFgRelease) {
        app.popup.items.assign(std::begin(kCheekyFgReleaseNames), std::end(kCheekyFgReleaseNames));
        app.popup.selected = cheeky_fg_release_index(app.s.cheeky.fgRelease);
    } else if (owner == kFgScale || owner == kFgPeriphery) {
        app.popup.items.assign(std::begin(kFgQuarters), std::end(kFgQuarters));
        app.popup.selected = std::clamp((owner == kFgScale ? app.s.cheeky.fgScale : app.s.cheeky.fgPeriphery) / 25 - 1, 0, 3);
    } else if (owner == kVisor) {
        app.popup.items.assign(std::begin(kVisorPositionNames), std::end(kVisorPositionNames));
        app.popup.selected = static_cast<int>(app.s.helmet.visor);
    } else if (owner == kShape) {
        app.popup.items.assign(std::begin(kShapeNames), std::end(kShapeNames));
        app.popup.selected = app.s.shape.corners ? 1 : 0;
    } else if (owner == kDiagKind) {
        app.popup.items.assign(std::begin(kIssueKinds), std::end(kIssueKinds));
        app.popup.selected = app.reportKind;
    } else if (owner == kHelmetDetail) {
        app.popup.items = {L"Performance (2048)", L"Detail (4096)"};
        app.popup.selected = app.s.helmet.detail ? 1 : 0;
    } else if (owner == kLayerProfile) {
        app.popup.items = {kLayerProfileNames[0], kLayerProfileNames[1]};
        app.popup.selected = app.layerProfile;
    } else if (owner == kCheekyNrPasses) {
        app.popup.items = {L"1 pass", L"2 passes", L"3 passes"};
        app.popup.selected = app.s.cheeky.nrPasses - 1;
    } else {
        app.popup.items = owner == kMethod ? std::vector<std::wstring>{kMethodNames[0], kMethodNames[1]}
                                           : std::vector<std::wstring>{kModeNames[0], kModeNames[1]};
        app.popup.selected = owner == kMethod ? (app.s.mode == RenderMode::Qvfr ? 0 : 1) : (app.s.eyeTracking ? 0 : 1);
    }
    app.popup.hover = app.popup.selected;
    app.popup.open = true;
    InvalidateRect(app.hwnd, nullptr, FALSE);
}

namespace {

void choose(Ctl owner, int option) {
    if (owner == kDiagKind) {  // not a setting
        app.reportKind = option;
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
    if (!pane_shown(static_cast<int>(app.pane)))
        app.pane = static_cast<Pane>(shown_pane(static_cast<int>(app.pane), -1, 0));
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
    if (accept && app.popup.hover >= 0)
        choose(app.popup.owner, app.popup.hover);
    app.popup.open = false;
    InvalidateRect(app.hwnd, nullptr, FALSE);
}

int popup_item_at(float x, float y) {
    const Popup& p = app.popup;
    const float itemH = kMenuItemH;
    const Rect menu = popup_menu_rect();
    for (size_t i = 0; i < p.items.size(); ++i)
        if (Rect{menu.x + 6, menu.y + 6 + itemH * i, menu.w - 12, itemH}.contains(x, y))
            return static_cast<int>(i);
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
