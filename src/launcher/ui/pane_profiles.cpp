// The Profiles pane (profile/profiles.h): using, saving, sharing and deleting profiles.
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include <windows.h>

#include "../settings.h"
#include "window_state.h"
#include "controls.h"
#include "form.h"

namespace simqvfr::launcher::ui::win {

// Profiles (profile/profiles.h): the presets, then the saved profiles, each with what it does and its buttons.
float profiles_pane(float x, float y, float w, bool draw) {
    static const wchar_t* const kPresetNotes[] = {L"The lightest load, for a steady frame rate.", L"Between the two.",
                                                  L"The sharpest centre, for a fast GPU."};
    const std::vector<Profile> presets = builtin_profiles(app.s.eyeTracking);
    // A row's buttons, right-aligned in its control rect: (id, label), each the profile's index as its option;
    // kNone: a green badge with the label instead ("In use").
    const auto buttons_row = [](size_t i, std::vector<std::pair<Ctl, std::wstring>> list) {
        float width = 0;
        std::vector<float> widths;
        for (const auto& [id, label] : list) {
            widths.push_back(id == kNone ? badge_width(label)
                                         : button_width(label));
            width += widths.back();
        }
        width += t::kSpace2 * (list.size() - 1);
        Row r;
        r.controlW = width;
        r.controlH = t::kControlHeight;
        r.control = [i, list, widths](const Rect& rect) {
            float bx = rect.x;
            for (size_t b = 0; b < list.size(); ++b) {
                if (list[b].first == kNone) {
                    badge(bx, rect.y + (rect.h - 20) / 2, 0, list[b].second);
                } else {
                    const Ctl id = list[b].first;
                    button(id, {bx, rect.y, widths[b], rect.h}, list[b].second,
                           id == kProfileDelete ? Btn::Danger
                           : id == kProfileShare ? Btn::Ghost
                           : id == kProfileUpdate ? Btn::Tint
                                                  : Btn::Secondary);
                    app.hits.back().option = static_cast<int>(i);
                }
                bx += widths[b] + t::kSpace2;
            }
        };
        return r;
    };
    // The one in use shows a badge for its Use button while the settings still match it. Changed since, it says so and
    // offers Update (a saved profile: the settings now go into it) and Revert (back to what it holds).
    std::vector<Row> presetRows, savedRows;
    for (size_t i = 0; i < app.profileNames.size(); ++i) {
        const std::wstring& name = app.profileNames[i];
        const bool current = _wcsicmp(name.c_str(), app.profileCurrent.c_str()) == 0, changedSince = current && !app.profileMatches;
        const bool preset = i < presets.size();
        std::vector<std::pair<Ctl, std::wstring>> buttons;
        if (changedSince && !preset)
            buttons.push_back({kProfileUpdate, L"Update"});
        buttons.push_back(changedSince ? std::pair<Ctl, std::wstring>{kProfileRevert, L"Revert"}
                          : current    ? std::pair<Ctl, std::wstring>{kNone, L"In use"}
                                       : std::pair<Ctl, std::wstring>{kProfileUse, L"Use"});
        const std::wstring changedNote = changedSince ? L"In use, changed since." : L"";
        if (preset) {
            Row r = buttons_row(i, buttons);
            r.label = name;
            r.desc = preset_summary(presets[i]) + L". " + kPresetNotes[i] + (changedNote.empty() ? L"" : L" " + changedNote);
            presetRows.push_back(r);
        } else {
            buttons.insert(buttons.end(), {{kProfileShare, L"Share"}, {kProfileDelete, L"Delete"}});
            Row r = buttons_row(i, buttons);
            r.label = name;
            r.desc = changedNote;
            savedRows.push_back(r);
        }
    }
    float h = 0;
    h += section(x, y + h, w, app.s.eyeTracking ? L"Presets for eye tracked foveation" : L"Presets for fixed foveation", presetRows,
                 L"They set the resolution only; the Foveated mode (Foveation) picks the set.",
                 draw) +
         t::kSpace6;
    if (savedRows.empty()) {
        Row none;
        none.label = L"None yet";
        none.desc = L"Save the settings you have as a profile, or paste one someone shared with you.";
        savedRows.push_back(none);
    }
    Row actions;
    actions.fullHeight = [](float) { return t::kControlHeight; };
    actions.full = [](const Rect& r) {
        const float saveW = button_width(L"Save current settings…") + 24;
        const float pasteW = button_width(L"Paste a shared profile") + 24;
        button(kProfileSave, {r.x, r.y, saveW, t::kControlHeight}, L"Save current settings…", Btn::Tint, kIconPlus);
        button(kProfilePaste, {r.x + saveW + t::kSpace2, r.y, pasteW, t::kControlHeight}, L"Paste a shared profile", Btn::Ghost,
               kIconPaste);
    };
    savedRows.push_back(actions);
    h += section(x, y + h, w, L"Your profiles", savedRows,
                 app.profileMessage.empty() ? L"Share copies one as a line of text; the headset menu switches between them."
                                            : app.profileMessage,
                 draw);
    return h;
}

void use_profile(const std::wstring& name) {
    Profile p;
    if (!load_profile(profiles_folder(app.ini), name, p, app.s.eyeTracking) || !apply_profile(app.ini.c_str(), p)) {
        app.profileMessage = L"Couldn’t use " + name + L".";
        return;
    }
    set_current_profile(app.ini.c_str(), p.name);
    app.s = load_settings(app.ini);
    changed();  // saved as the launcher saves, and to a running game
    app.profileMessage = L"Using " + p.name + L".";
    refresh_profiles();
}

namespace {

// Saves `p` under its name, asking first (a sheet) when one of that name exists; `then` once it is saved.
void store_profile(const Profile& p, std::function<void()> then) {
    const std::wstring folder = profiles_folder(app.ini);
    const auto save = [p, folder, then] {
        if (!save_profile(folder, p)) {
            notify(L"The profile couldn’t be saved", L"Sim-QVFR couldn’t write it in " + folder + L".");
            return;
        }
        if (then)
            then();
    };
    Profile existing;
    if (!load_profile(folder, p.name, existing, app.s.eyeTracking)) {
        save();
        return;
    }
    Sheet s;
    s.title = L"Replace " + p.name + L"?";
    s.text = L"A profile named " + p.name + L" exists. Replacing it puts these settings in its place.";
    s.buttons = {L"Cancel", L"Replace"};
    s.focus = 0;
    s.danger = true;
    s.done = [save](int button, const std::wstring&) {
        if (button == 1)
            save();
    };
    open_sheet(std::move(s));
}

// Save current settings…: a name asked for in a sheet (`name` filled in), then the settings saved under it.
void ask_profile_name(const std::wstring& name) {
    Sheet s;
    s.title = L"Save current settings";
    s.text = L"A profile holds every setting of the picture. Name the settings you have now.";
    s.field = true;
    s.fieldLabel = L"Name";
    s.fieldText = name;
    s.buttons = {L"Cancel", L"Save"};
    s.done = [](int button, const std::wstring& typed) {
        if (button != 1)
            return;
        if (!valid_profile_name(typed)) {
            notify(L"Choose another name",
                   L"Use 1 to 40 characters, none of \\ / : * ? \" < > |, and not a preset’s name (Performance, Balanced, Quality).",
                   [typed] { ask_profile_name(typed); });
            return;
        }
        store_profile(settings_profile(app.s, typed), [typed] {
            set_current_profile(app.ini.c_str(), typed);
            app.profileMessage = L"Saved " + typed + L".";
            refresh_profiles();
        });
    };
    open_sheet(std::move(s));
}

}  // namespace

void save_current_profile() { ask_profile_name(is_preset_name(app.profileCurrent) ? L"" : app.profileCurrent); }

void paste_profile() {
    std::string text;
    for (const wchar_t c : clipboard_text())  // a shared profile is ASCII
        text += c < 128 ? static_cast<char>(c) : '?';
    Profile p;
    if (!shared_profile(text, p)) {
        notify(L"No shared profile on the clipboard", L"Copy the whole line that starts with QVFR1:, then try again.");
        return;
    }
    store_profile(p, [name = p.name] {
        refresh_profiles();
        app.profileMessage = L"Added " + name + L".";
        Sheet s;
        s.title = L"Use " + name + L" now?";
        s.text = L"It’s in Your profiles. Using it sets the picture to what it holds.";
        s.buttons = {L"Not Now", L"Use It"};
        s.done = [name](int button, const std::wstring&) {
            if (button == 1)
                use_profile(name);
            InvalidateRect(app.hwnd, nullptr, FALSE);
        };
        open_sheet(std::move(s));
    });
}

}  // namespace simqvfr::launcher::ui::win
