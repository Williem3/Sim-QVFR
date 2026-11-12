// The OpenXR layers card (layer_order.h): the order with the launcher and Windows' own list.
#include <filesystem>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include <windows.h>

#include "../launcher.h"
#include "../launch_plan.h"
#include "window_state.h"
#include "card.h"
#include "controls.h"
#include "form.h"

namespace simqvfr::launcher::ui::win {

// ---- OpenXR layers -----------------------------------------------------------------------------

// Reads both profiles again; Windows' draft is kept while it has changes (`keepDraft`).
void refresh_layers(bool keepDraft) {
    app.layerPlan = launcher_layer_plan();
    const bool dirty = app.layersDraft != app.layersWindows || app.layersLockMoved;
    app.layersWindows = read_system_layers(HKEY_LOCAL_MACHINE, HKEY_CURRENT_USER, kImplicitLayersKey, &app.layersLockMoved);
    if (!keepDraft || !dirty || app.layersDraft.size() != app.layersWindows.size())
        app.layersDraft = app.layersWindows;
}

int layer_rows() {
    return static_cast<int>(app.layerProfile == 0 ? app.layerPlan.layers.size() : app.layersDraft.size());
}

// A layer's name as people know it; the registered name for the rest.
std::wstring friendly_layer(const std::string& name, const std::wstring& manifest) {
    static const std::pair<const char*, const wchar_t*> kKnown[] = {
        {"XR_APILAYER_MBUCCHIA_toolkit", L"OpenXR Toolkit"},
        {"XR_APILAYER_DIENERTECH_VECTORXR", L"VectorXR"},
        {"XR_APILAYER_CHEEKY_foveated_dlss", L"Cheeky Foveated DLSS"},
        {"XR_APILAYER_MBUCCHIA_quad_views_foveated", L"Quad-Views-Foveated"},
        {"XR_APILAYER_SIM_qvfr", L"Sim-QVFR"},
        {"XR_APILAYER_NOVENDOR_motion_compensation", L"OpenXR Motion Compensation"},
        {"XR_APILAYER_FREDEMMOTT_OpenKneeboard", L"OpenKneeboard"},
        {"XR_APILAYER_FREDEMMOTT_HandTrackedCockpitClicking", L"Hand Tracked Cockpit Clicking"},
    };
    for (const auto& [n, label] : kKnown)
        if (_stricmp(n, name.c_str()) == 0)
            return label;
    if (name.empty())
        return std::filesystem::path(manifest).filename().wstring();
    return widen(name);
}

// The launcher's profile changes in the ini, then the plan is read again.
void edit_launcher_layers(const std::function<void(LauncherLayers&)>& change) {
    if (!app.iniFound)
        return;
    LauncherLayers profile = read_launcher_layers(app.ini);
    change(profile);
    write_launcher_layers(app.ini, profile);
    app.iniText = ini_text();
    refresh_layers();
}

void toggle_layer(int i) {
    if (app.layerProfile == 0) {
        if (i < 0 || i >= static_cast<int>(app.layerPlan.layers.size()) || !app.layerPlan.layers[i].lock.empty())
            return;
        const PlannedLayer l = app.layerPlan.layers[i];
        edit_launcher_layers([&](LauncherLayers& p) { set_launcher_layer(p, l.name, !l.on, l.systemOn); });
    } else if (i >= 0 && i < static_cast<int>(app.layersDraft.size())) {
        app.layersDraft[i].on = !app.layersDraft[i].on;
    }
    app.layerMessage.clear();
}

void move_layer(int i, int step) {
    if (app.layerProfile == 0) {
        if (i < 0 || i >= static_cast<int>(app.layerPlan.layers.size()))
            return;
        const std::string name = app.layerPlan.layers[i].name;
        bool moved = false;
        edit_launcher_layers([&](LauncherLayers& p) { moved = move_launcher_layer(p, app.layerPlan, name, step); });
        if (moved)
            app.layerRow = i + step;
    } else if (move_system_layer(app.layersDraft, static_cast<size_t>(i), step)) {
        app.layerRow = i + step;
    }
    app.layerMessage.clear();
}

void apply_layers() {
    const std::wstring error = apply_system_layers(app.hwnd, app.layersDraft);
    refresh_layers(!error.empty());
    app.layerMessage = error.empty() ? L"Windows’ OpenXR layers updated. A backup of each list is in the logs folder." : error;
    app.layerTone = error.empty() ? 0 : 2;
}

namespace {

void mini_check(int option, const Rect& box, bool on, bool disabled, const std::wstring& name, const std::wstring& tip) {
    switch_track(box, on, disabled);
    if (!disabled)
        app.hits.push_back({kLayerOn, {box.x - 4, box.y - 4, box.w + 8, box.h + 8}, option, tip, name, on ? 1 : 0});
}

void arrow_button(Ctl id, int option, const Rect& r, bool disabled) {
    app.canvas.fill(r, col(!disabled && pointer_in(r) ? app.c->control_hover : app.c->control_bg, disabled ? 0.5f : 1),
                    t::kRadiusControl);
    app.canvas.path(id == kLayerUp ? "M3 7.5L6 4.5l3 3" : "M3 4.5L6 7.5l3-3", {r.x + (r.w - 12) / 2, r.y + (r.h - 12) / 2, 12, 12},
                    12, col(disabled ? app.c->label_tertiary : app.c->label), 1.5f);
    if (!disabled)
        app.hits.push_back({id, r, option, L"", id == kLayerUp ? L"Move up" : L"Move down"});  // the row says which layer
}

constexpr float kLayerControlsW = 40 + 14 + 32 + 4 + 32;

// One movable layer's switch (disabled when locked) and its moves, at r (kLayerControlsW x 32).
void layer_controls(const Rect& r, const std::wstring& name, int index, bool on, bool switchLocked, bool canUp, bool canDown,
                    const std::wstring& tip = {}) {
    mini_check(index, {r.x, r.y + 5, 40, 22}, on, switchLocked, name, tip);
    arrow_button(kLayerUp, index, {r.x + 54, r.y, 32, 32}, !canUp);
    arrow_button(kLayerDown, index, {r.x + 90, r.y, 32, 32}, !canDown);
    if (focused(kLayerList) && app.layerRow == index)
        focus_ring({r.x - 2, r.y - 2, r.w + 4, r.h + 4}, t::kRadiusControl);
}

// The chain a frame goes through with this profile, the game first, the runtime last.
std::wstring layer_chain() {
    std::wstring chain = short_name();
    const auto add = [&chain](const std::wstring& name) { chain += L"  →  " + name; };
    if (app.layerProfile == 0) {
        for (const std::string& name : app.layerPlan.above)
            add(friendly_layer(name));
        add(friendly_layer(kLayerName));
        for (const PlannedLayer& l : app.layerPlan.layers)
            if (l.on)
                add(friendly_layer(l.name));
    } else {
        for (const SystemLayer& l : app.layersDraft)
            if (l.on)
                add(friendly_layer(l.name, l.manifest));
    }
    add(L"OpenXR runtime");
    return chain;
}

}  // namespace

// The OpenXR layers card: the profile (with the launcher, or Windows' own list), its layers nearest the game first, each
// with its switch and moves, and the chain a frame goes through.
Card layers_card() {
    Card c{Tab::Game, L"OpenXR layers", Applies::NextLaunch};
    c.body = [](Body& b) {
        b.segments(kLayerProfile, L"Profile", {kLayerProfileNames[0], kLayerProfileNames[1]}, app.layerProfile);
        b.note(app.layerProfile == 0
                   ? L"What " + short_name() + L" loads when you launch it from here. Windows’ own list stays as it is."
                   : L"What every other app loads, and " + short_name() + L" started from Steam. Changes Windows’ registry.");
        if (app.layerProfile == 0) {
            b.group(L"Load order, nearest the game first");
            for (const std::string& name : app.layerPlan.above)
                b.status(friendly_layer(name), L"No switch to turn it off per launch, so it stays above the Sim-QVFR layer.", 3,
                         L"Above");
            b.status(friendly_layer(kLayerName), L"Always nearest the game.", 3, L"Locked");
            const int n = static_cast<int>(app.layerPlan.layers.size());
            for (int i = 0; i < n; ++i) {
                const PlannedLayer& l = app.layerPlan.layers[i];
                const std::wstring desc = !l.lock.empty() ? widen(l.lock) : l.systemOn ? L"" : L"Off in Windows";
                const bool on = l.on, locked = !l.lock.empty();
                const std::wstring name = friendly_layer(l.name);
                b.trailing(name, desc, kLayerControlsW, 32,
                           [=](const Rect& r) { layer_controls(r, name, i, on, locked, i > 0, i + 1 < n); });
            }
        } else {
            const int n = static_cast<int>(app.layersDraft.size());
            for (const bool machine : {true, false}) {
                bool any = false;
                for (int i = 0; i < n; ++i) {
                    const SystemLayer& l = app.layersDraft[i];
                    if (l.machine != machine)
                        continue;
                    if (!any) {
                        b.group(machine ? L"All users" : L"This user");
                        b.note(machine ? L"Changing these needs administrator rights: Windows asks once when you apply."
                                       : L"Windows loads these after the machine-wide ones.");
                    }
                    any = true;
                    const bool up = i > 0 && app.layersDraft[i - 1].machine == machine && app.layersDraft[i - 1].lock.empty() &&
                                    l.lock.empty();
                    const bool down = i + 1 < n && app.layersDraft[i + 1].machine == machine && l.lock.empty();
                    // Its manifest's path is its switch's tip, not its line (no developer paths in the text).
                    std::wstring desc = l.lock.empty() ? std::wstring() : widen(l.lock);
                    if (l.name.empty())
                        desc += (desc.empty() ? L"" : L" · ") + std::wstring(L"Its manifest can’t be read");
                    const bool on = l.on;
                    const std::wstring name = friendly_layer(l.name, l.manifest), manifest = l.manifest;
                    b.trailing(name, desc, kLayerControlsW, 32,
                               [=](const Rect& r) { layer_controls(r, name, i, on, false, up, down, manifest); });
                }
            }
            if (app.layersDraft.empty())
                b.chip(3, L"No OpenXR layers are registered in Windows.");
            if (layers_dirty()) {
                b.chip(1, L"Changes not applied");
                b.actions({{kLayerRevert, L"Revert"}, {kLayerApply, L"Apply", Btn::Primary}});
            }
            b.note(app.layersLockMoved ? L"The Sim-QVFR layer is not first in its list: applying moves it there."
                                       : L"The current list is backed up into the logs folder before it changes.");
        }
        if (!app.layerMessage.empty()) {
            b.chip(app.layerTone, app.layerTone == 0 ? L"Applied" : L"Not applied");
            b.note(app.layerMessage);
        }
        b.group(L"A frame’s path");
        b.note(layer_chain());
    };
    // With the launcher, Reset this card forgets the profile: launches follow Windows' order and switches again.
    if (app.layerProfile == 0)
        c.reset = [](Settings&) {
            edit_launcher_layers([](LauncherLayers& p) { p = {}; });
            app.layerMessage.clear();
        };
    return c;
}

}  // namespace simqvfr::launcher::ui::win
