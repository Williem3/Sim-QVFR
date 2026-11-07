// The OpenXR layers pane (layer_order.h): the order with the launcher and Windows' own list.
#include <filesystem>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include <windows.h>

#include "../launcher.h"
#include "../launch_plan.h"
#include "window_state.h"
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

void mini_check(int option, const Rect& box, bool on, bool disabled) {
    switch_track(box, on, disabled);
    if (!disabled)
        app.hits.push_back({kLayerOn, {box.x - 4, box.y - 4, box.w + 8, box.h + 8}, option});
}

void arrow_button(Ctl id, int option, const Rect& r, bool disabled) {
    app.canvas.fill(r, col(!disabled && pointer_in(r) ? app.c->control_hover : app.c->control_bg, disabled ? 0.5f : 1),
                    t::kRadiusControl);
    app.canvas.path(id == kLayerUp ? "M3 7.5L6 4.5l3 3" : "M3 4.5L6 7.5l3-3", {r.x + (r.w - 12) / 2, r.y + (r.h - 12) / 2, 12, 12},
                    12, col(disabled ? app.c->label_tertiary : app.c->label), 1.5f);
    if (!disabled)
        app.hits.push_back({id, r, option});
}

constexpr float kLayerControlsW = 40 + 14 + 32 + 4 + 32;

// One movable layer: its switch (disabled when locked) and its moves.
Row layer_row(const std::wstring& label, const std::wstring& desc, int index, bool on, bool switchLocked, bool canUp,
              bool canDown) {
    Row row;
    row.label = label;
    row.desc = desc;
    row.controlW = kLayerControlsW;
    row.controlH = 32;
    row.control = [=](const Rect& r) {
        mini_check(index, {r.x, r.y + 5, 40, 22}, on, switchLocked);
        arrow_button(kLayerUp, index, {r.x + 54, r.y, 32, 32}, !canUp);
        arrow_button(kLayerDown, index, {r.x + 90, r.y, 32, 32}, !canDown);
        if (focused(kLayerList) && app.layerRow == index)
            focus_ring({r.x - 2, r.y - 2, r.w + 4, r.h + 4}, t::kRadiusControl);
    };
    return row;
}

// A layer nothing here can change: a badge saying so.
Row locked_row(const std::wstring& label, const std::wstring& desc, const std::wstring& text) {
    Row row;
    row.label = label;
    row.desc = desc;
    row.controlW = badge_width(text);
    row.controlH = 20;
    row.control = [text](const Rect& r) { badge(r.x, r.y, 3, text); };
    return row;
}

}  // namespace

float layers_pane(float x, float y, float w, bool draw) {
    float h = 0;
    {
        const std::wstring names[2] = {kLayerProfileNames[0], kLayerProfileNames[1]};
        Row profile = choice_row(kLayerProfile, L"Profile", names, 2, app.layerProfile, false);
        profile.desc = app.layerProfile == 0
                           ? L"What " + short_name() + L" loads when you launch it from here. Windows’ own list stays as it is."
                           : L"What every other app loads, and " + short_name() + L" started from Steam. Changes Windows’ registry.";
        h += section(x, y, w, L"", {profile}, L"", draw) + t::kSpace6;
    }
    if (app.layerProfile == 0) {
        std::vector<Row> rows;
        for (const std::string& name : app.layerPlan.above)
            rows.push_back(locked_row(friendly_layer(name), L"Has no switch to turn it off per launch, so it stays above the Sim-QVFR layer.",
                                      L"Above"));
        rows.push_back(locked_row(friendly_layer(kLayerName), L"Always nearest the game.", L"Locked"));
        const int n = static_cast<int>(app.layerPlan.layers.size());
        for (int i = 0; i < n; ++i) {
            const PlannedLayer& l = app.layerPlan.layers[i];
            std::wstring desc = !l.lock.empty() ? widen(l.lock) : l.systemOn ? L"" : L"Off in Windows";
            if (friendly_layer(l.name) != widen(l.name))
                desc += (desc.empty() ? L"" : L" · ") + widen(l.name);
            rows.push_back(layer_row(friendly_layer(l.name), desc, i, l.on, !l.lock.empty(), i > 0, i + 1 < n));
        }
        h += section(x, y + h, w, L"Load order", rows,
                     L"Nearest the game first: each launch loads them below the Sim-QVFR layer in this order.", draw,
                     Applies::NextLaunch);
        Row reset;
        reset.label = L"Windows’ order";
        reset.desc = L"Forget this profile: launches follow Windows’ order and switches again.";
        reset.controlW = button_width(L"Reset");
        reset.controlH = t::kControlHeight;
        reset.control = [](const Rect& r) { button(kLayerReset, r, L"Reset", false); };
        h += t::kSpace6 + section(x, y + h + t::kSpace6, w, L"", {reset}, L"", draw);
    } else {
        for (const bool machine : {true, false}) {
            std::vector<Row> rows;
            const int n = static_cast<int>(app.layersDraft.size());
            for (int i = 0; i < n; ++i) {
                const SystemLayer& l = app.layersDraft[i];
                if (l.machine != machine)
                    continue;
                const bool up = i > 0 && app.layersDraft[i - 1].machine == machine && app.layersDraft[i - 1].lock.empty() && l.lock.empty();
                const bool down = i + 1 < n && app.layersDraft[i + 1].machine == machine && l.lock.empty();
                std::wstring desc = l.lock.empty() ? std::wstring() : widen(l.lock) + L" · ";
                desc += l.name.empty() ? L"Unreadable manifest: " + l.manifest : l.manifest;
                rows.push_back(layer_row(friendly_layer(l.name, l.manifest), desc, i, l.on, false, up, down));
            }
            if (rows.empty())
                continue;
            h += section(x, y + h, w, machine ? L"All users (HKLM)" : L"This user (HKCU)", rows,
                         machine ? L"Changing these needs administrator rights: Windows asks once when you apply."
                                 : L"Windows loads these after the machine-wide ones.",
                         draw, Applies::NextLaunch) +
                 t::kSpace6;
        }
        if (app.layersDraft.empty()) {
            if (draw)
                badge(x, y + h, 3, L"No OpenXR layers are registered in Windows.", w);
            h += 20 + t::kSpace6;
        }
        Row apply;
        apply.label = layers_dirty() ? L"Changes not applied" : L"Windows’ list";
        apply.desc = app.layersLockMoved ? L"The Sim-QVFR layer is not first in its list: applying moves it there."
                                         : L"Nearest the game first. The current list is backed up into logs before it changes.";
        const float applyW = button_width(L"Apply"),
                    revertW = button_width(L"Revert");
        apply.controlW = applyW + t::kSpace2 + revertW;
        apply.controlH = t::kControlHeight;
        const bool dirty = layers_dirty();
        apply.control = [=](const Rect& r) {
            if (dirty) {
                button(kLayerRevert, {r.x, r.y, revertW, r.h}, L"Revert", false);
                button(kLayerApply, {r.x + revertW + t::kSpace2, r.y, applyW, r.h}, L"Apply", true);
            }
        };
        h += section(x, y + h, w, L"", {apply}, L"", draw);
    }
    if (!app.layerMessage.empty()) {
        if (draw)
            badge(x, y + h + t::kSpace4, app.layerTone, app.layerMessage, w);
        h += t::kSpace4 + 20;
    }
    return h;
}

// The preview: the chain a frame goes through, the game at the top, the runtime at the bottom.
float layers_preview(float x, float y, float w, bool draw) {
    std::vector<std::pair<std::wstring, int>> chain;  // tone: 0 end, 1 locked, 2 layer
    chain.push_back({game_name(), 0});
    if (app.layerProfile == 0) {
        for (const std::string& name : app.layerPlan.above)
            chain.push_back({friendly_layer(name), 2});
        chain.push_back({friendly_layer(kLayerName), 1});
        for (const PlannedLayer& l : app.layerPlan.layers)
            if (l.on)
                chain.push_back({friendly_layer(l.name), l.lock.empty() ? 2 : 1});
    } else {
        for (const SystemLayer& l : app.layersDraft)
            if (l.on)
                chain.push_back({friendly_layer(l.name, l.manifest), l.lock.empty() ? 2 : 1});
    }
    chain.push_back({L"OpenXR runtime", 0});
    const float pill = 30, gap = 14;
    if (draw)
        for (size_t i = 0; i < chain.size(); ++i) {
            const Rect r{x, y + i * (pill + gap), w, pill};
            const auto& [text, tone] = chain[i];
            app.canvas.fill(r, col(tone == 0 ? app.c->segment_track : tone == 1 ? app.c->accent_fill : app.c->group_bg), pill / 2);
            if (tone == 2)
                app.canvas.stroke(r, col(app.c->separator), 1, pill / 2);
            app.canvas.text(text, t::kBody, {r.x + 14, r.y, r.w - 28, r.h}, col(tone == 1 ? app.c->on_accent : app.c->label),
                            Align::Center);
            if (i + 1 < chain.size()) {
                const float cx = x + w / 2, top = r.bottom() + 2, bottom = r.bottom() + gap - 2;
                app.canvas.line(cx, top, cx, bottom, col(app.c->label_secondary), 1.5f);
                app.canvas.path("M2 3l3 3 3-3", {cx - 5, bottom - 6, 10, 8}, 10, col(app.c->label_secondary), 1.5f);
            }
        }
    return chain.size() * (pill + gap) - gap;
}

}  // namespace simqvfr::launcher::ui::win
