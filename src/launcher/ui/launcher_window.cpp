// The launcher window, built natively from the QVFR launcher kit (docs/qvfr-launcher-kit): a sidebar
// of panes (Rendering, Resolution, Foveation, Focus overlay, Headset menu, About), grouped forms with
// the kit's controls, a live foveation preview with readouts (the Headset menu pane: a placement preview
// to drag the in-headset settings menu around), and an action bar with the status badge,
// Restore Defaults and Launch. Every colour, size and type style comes from theme_tokens.h; every
// label is the kit's. Settings save as they change (settings.h); Launch runs launch_ace (main.cpp).
//
// Immediate mode: each paint lays out and draws the current pane and records hit rectangles; input
// acts on the last frame's rectangles and the settings, then repaints.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

#include <windows.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <windowsx.h>

#include "../launcher.h"
#include "../launch_plan.h"
#include "../settings.h"
#include "canvas.h"
#include "launcher_version.h"

#pragma comment(lib, "dwmapi.lib")

namespace aceqv::launcher {
namespace ui {
namespace {

namespace t = tokens;

enum class Pane { Rendering, Resolution, Foveation, Overlay, Menu, About };
constexpr int kPanes = 6;
enum Ctl {
    kNone, kSidebar, kMethod, kMode, kOuter, kInner, kWidth, kHeight, kCorner, kOutline, kColor, kFeather, kMenuYaw,
    kMenuPitch, kMenuWidth, kMenuPanel, kSupport, kRestore, kLaunch
};

constexpr wchar_t kSupportUrl[] = L"https://ko-fi.com/williem3";

struct Hit {
    Ctl id;
    Rect r;
    int option = -1;  // sidebar row, segment, popup item
};

enum class Status { Idle, Starting, Error };
// After a change while ACE runs: its live keys went to the running game (Applied), and resolution
// or method differ from what it runs with (NextLaunch).
enum class Live { Off, Applied, NextLaunch };

struct Popup {
    bool open = false;
    Ctl owner = kNone;
    Rect anchor;
    std::vector<std::wstring> items;
    int selected = 0, hover = -1;
};

constexpr wchar_t kMethodNames[2][32] = {L"Quad View Foveated Rendering", L"Two-loop Foveated Interlaced"};
constexpr wchar_t kModeNames[2][16] = {L"Eye tracked", L"Fixed"};
constexpr wchar_t kColorNames[3][8] = {L"Teal", L"Magenta", L"Green"};
constexpr float kMinWidth = 820, kMinHeight = 540, kDefaultWidth = 960, kDefaultHeight = 600;
constexpr float kTitlebar = t::kTitlebarHeight;  // the kit's window includes its title bar; ours is native
// The placement preview's reference view (degrees) and the panel's distance (the layer's kMenuDistance).
constexpr float kRefFovX = 100, kRefFovY = 80, kMenuDistance = 0.8f;
// The in-headset panel's image (the layer's kPanelWidth x kPanelHeight) and colours (menu_panel.cpp).
constexpr float kMenuAspect = 922.0f / 1024;
constexpr uint32_t kMenuBackground = 0x16181c, kMenuHighlight = 0x0a6e78, kMenuLabel = 0xe8eaed, kMenuDim = 0x9aa0a6;

struct App {
    HWND hwnd = nullptr;
    Canvas canvas;
    bool dark = false;
    const t::Colors* c = &t::kLight;
    Settings s;
    RuntimeInfo runtime;
    std::wstring ini;
    bool iniFound = false;
    FILETIME iniWritten{};  // the ini's write time when last read or saved here (follow_ini)
    Pane pane = Pane::Rendering;
    Ctl focus = kNone;
    bool keyboard = false;  // focus rings show after keyboard use only (:focus-visible)
    Ctl pressed = kNone, dragging = kNone;
    Rect dragTrack;
    Range dragRange{0, 100};
    Rect placeFrame;                   // the placement preview's reference view, as last drawn
    float grabYaw = 0, grabPitch = 0;  // dragging the panel: its centre less the pointer, degrees
    Popup popup;
    Status status = Status::Idle;
    Live live = Live::Off;
    std::wstring error;
    float scroll = 0, scrollMax = 0;
    Rect scrollArea;
    std::vector<Hit> hits;
} app;

Color col(uint32_t rgb, float alpha = 1) { return {rgb, alpha}; }

bool system_dark() {
    // ACE_QV_LAUNCHER_THEME=light|dark overrides the Windows app theme (for checking both).
    wchar_t forced[16] = {};
    if (GetEnvironmentVariableW(L"ACE_QV_LAUNCHER_THEME", forced, 16))
        return _wcsicmp(forced, L"dark") == 0;
    DWORD light = 1, size = sizeof(light);
    RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                 L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &light, &size);
    return light == 0;
}

COLORREF colorref(uint32_t rgb) { return RGB((rgb >> 16) & 0xff, (rgb >> 8) & 0xff, rgb & 0xff); }

void apply_theme() {
    app.dark = system_dark();
    app.c = app.dark ? &t::kDark : &t::kLight;
    const BOOL dark = app.dark;
    DwmSetWindowAttribute(app.hwnd, 20 /*DWMWA_USE_IMMERSIVE_DARK_MODE*/, &dark, sizeof(dark));
    const COLORREF caption = colorref(app.c->titlebar_bg), text = colorref(app.c->label);
    DwmSetWindowAttribute(app.hwnd, 35 /*DWMWA_CAPTION_COLOR*/, &caption, sizeof(caption));
    DwmSetWindowAttribute(app.hwnd, 36 /*DWMWA_TEXT_COLOR*/, &text, sizeof(text));
}

// ---- Settings flow -----------------------------------------------------------------------------

bool eye_warning() { return app.s.eyeTracking && app.runtime.eyeTracker == EyeTracker::Missing; }

// A running ACE reloads the live keys from its own ini (the run folder's copy) between frames.
void push_live() {
    app.live = Live::Off;
    const std::wstring running = running_live_ini(runtime_info_path());
    if (running.empty() || !save_live_settings(running, app.s))
        return;
    // QVFR resolution applies live up to the launch size; more, the method or Two-loop's resolution next launch.
    app.live = fits_launch(app.s, load_launch_sizes(runtime_info_path())) ? Live::Applied : Live::NextLaunch;
}

FILETIME ini_written() {
    WIN32_FILE_ATTRIBUTE_DATA a{};
    return GetFileAttributesExW(app.ini.c_str(), GetFileExInfoStandard, &a) ? a.ftLastWriteTime : FILETIME{};
}

void changed(bool save = true) {
    app.status = Status::Idle;
    if (save && app.iniFound) {
        save_settings(app.ini, app.s);
        app.iniWritten = ini_written();
        push_live();
    }
    InvalidateRect(app.hwnd, nullptr, FALSE);
}

// The in-headset settings menu saves into the same ini: show what it set (not while a knob is held).
constexpr UINT_PTR kFollowTimer = 1;
void follow_ini() {
    if (!app.iniFound || app.dragging != kNone)
        return;
    const FILETIME written = ini_written();
    if (CompareFileTime(&written, &app.iniWritten) == 0)
        return;
    app.iniWritten = written;
    app.s = load_settings(app.ini);
    InvalidateRect(app.hwnd, nullptr, FALSE);
}

int* slider_value(Ctl id) {
    switch (id) {
    case kOuter: return &app.s.outer;
    case kInner: return &app.s.inner;
    case kWidth: return &app.s.width;
    case kHeight: return &app.s.height;
    case kFeather: return &app.s.feather;
    case kCorner: return &app.s.corner;
    case kMenuYaw: return &app.s.menuYaw;
    case kMenuPitch: return &app.s.menuPitch;
    case kMenuWidth: return &app.s.menuWidth;
    default: return nullptr;
    }
}

Range slider_range(Ctl id) {
    switch (id) {
    case kOuter: return kOuterRange;
    case kInner: return kInnerRange;
    case kFeather: return kFeatherRange;
    case kCorner: return kCornerRange;
    case kMenuYaw: return kMenuYawRange;
    case kMenuPitch: return kMenuPitchRange;
    case kMenuWidth: return kMenuWidthRange;
    default: return kSizeRange;
    }
}

bool inner_disabled() { return app.s.mode == RenderMode::TwoLoop; }

std::vector<Ctl> focus_order() {
    std::vector<Ctl> order = {kSidebar};
    switch (app.pane) {
    case Pane::Rendering: order.insert(order.end(), {kMethod, kMode}); break;
    case Pane::Resolution:
        order.push_back(kOuter);
        if (!inner_disabled())
            order.push_back(kInner);
        break;
    case Pane::Foveation: order.insert(order.end(), {kWidth, kHeight, kCorner}); break;
    case Pane::Overlay:
        order.push_back(kOutline);
        if (app.s.outline)
            order.push_back(kColor);
        order.push_back(kFeather);
        break;
    case Pane::Menu: order.insert(order.end(), {kMenuYaw, kMenuPitch, kMenuWidth, kMenuPanel}); break;
    case Pane::About: order.push_back(kSupport); break;
    }
    order.insert(order.end(), {kRestore, kLaunch});
    return order;
}

void launch() {
    if (!app.iniFound) {
        app.status = Status::Error;
        app.error = L"ace-quadview.ini not found next to the launcher.";
        InvalidateRect(app.hwnd, nullptr, FALSE);
        return;
    }
    save_settings(app.ini, app.s);
    app.status = Status::Starting;
    RedrawWindow(app.hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW);
    std::wstring report;
    if (launch_ace(false, report) != 0) {
        app.status = Status::Error;
        const size_t at = report.find(L"ERROR: ");
        app.error = at == std::wstring::npos ? L"Launch failed." : report.substr(at + 7, report.find(L'\n', at) - at - 7);
    }
    InvalidateRect(app.hwnd, nullptr, FALSE);
}

// ---- Controls ----------------------------------------------------------------------------------

void focus_ring(const Rect& r, float radius) {
    // Solid 3px focus-ring, offset 1px.
    app.canvas.stroke({r.x - 4, r.y - 4, r.w + 8, r.h + 8}, col(app.c->focus_ring), 3, radius + 4);
}

bool focused(Ctl id) { return app.keyboard && app.focus == id && !app.popup.open; }

void button(Ctl id, const Rect& r, const std::wstring& label, bool primary) {
    const bool down = app.pressed == id;
    Color bg = col(primary ? app.c->accent_fill : app.c->control_bg);
    if (down)
        bg.rgb = ((((bg.rgb >> 16) & 0xff) * 92 / 100) << 16) | ((((bg.rgb >> 8) & 0xff) * 92 / 100) << 8) |
                 ((bg.rgb & 0xff) * 92 / 100);  // brightness 92%
    if (!primary)
        app.canvas.control_shadow(r, t::kRadiusControl, app.dark);
    app.canvas.fill(r, bg, t::kRadiusControl);
    app.canvas.stroke(r, col(primary ? app.c->accent_fill : app.c->control_border), 1, t::kRadiusControl);
    const t::TypeStyle style = primary ? t::TypeStyle{15, 20, 500, false} : t::kBody;
    app.canvas.text(label, style, r, col(primary ? app.c->on_accent : app.c->label), Align::Center);
    if (focused(id))
        focus_ring(r, t::kRadiusControl);
    app.hits.push_back({id, r});
}

float popup_width(const std::wstring* items, int count, float minWidth) {
    float w = 0;
    for (int i = 0; i < count; ++i)
        w = std::max(w, app.canvas.measure(items[i], t::kBody));
    return std::max(minWidth, std::ceil(w) + t::kSpace2 + 26);
}

void popup_button(Ctl id, const Rect& r, const std::wstring& value, bool disabled) {
    if (!disabled)
        app.canvas.control_shadow(r, t::kRadiusControl, app.dark);
    app.canvas.fill(r, col(app.c->control_bg), t::kRadiusControl);
    app.canvas.stroke(r, col(disabled ? app.c->separator : app.c->control_border), 1, t::kRadiusControl);
    app.canvas.text(value, t::kBody, {r.x + t::kSpace2, r.y, r.w - t::kSpace2 - 26, r.h},
                    col(disabled ? app.c->label_tertiary : app.c->label));
    // Chevron 8 x 12, 8px from the trailing edge.
    app.canvas.path("M1.5 4.25L4 1.75l2.5 2.5M1.5 7.75L4 10.25l2.5-2.5", {r.right() - 16, r.y + (r.h - 12) / 2, 8, 12},
                    8, col(app.c->label_secondary), 1.5f);
    if (focused(id))
        focus_ring(r, t::kRadiusControl);
    app.hits.push_back({id, r});
}

// The kit's PercentSlider: track, value readout (in `unit`) and an optional detail line. Height 20 (+16 with detail).
void slider(Ctl id, const Rect& r, int value, Range range, const std::wstring& detail, bool disabled,
            const wchar_t* unit = L"%") {
    const float valueW = 40, trackW = r.w - valueW - t::kSpace3;
    const Rect track{r.x, r.y, trackW, 20};
    const float knob = 18, travel = track.w - knob;
    const float f = range.max > range.min ? static_cast<float>(value - range.min) / (range.max - range.min) : 0;
    const float kx = track.x + travel * f, cy = track.y + 10;
    const Color fill = col(disabled ? app.c->label_tertiary : app.c->accent_fill);
    app.canvas.fill({track.x, cy - 2, track.w, 4}, col(app.c->segment_track), 2);
    // Filled from the range's start, or from 0 when it spans both signs (degrees left and right).
    const float zx = range.min < 0 && range.max > 0 ? track.x + travel * (0.0f - range.min) / (range.max - range.min) : track.x;
    const float kc = kx + knob / 2;
    app.canvas.fill({std::min(zx, kc), cy - 2, std::abs(kc - zx), 4}, fill, 2);
    const Rect k{kx, cy - knob / 2, knob, knob};
    app.canvas.control_shadow(k, knob / 2, app.dark);
    app.canvas.fill(k, col(app.c->control_bg), knob / 2);
    app.canvas.stroke(k, col(app.c->control_border), 1, knob / 2);
    if (focused(id))
        app.canvas.circle(k.x + knob / 2, k.y + knob / 2, knob / 2 + 1.5f, col(app.c->focus_ring), false, 3);
    app.canvas.text(std::to_wstring(value) + unit, t::kValue, {track.right() + t::kSpace3, r.y, valueW, 20},
                    col(disabled ? app.c->label_tertiary : app.c->label), Align::Trailing);
    if (!detail.empty())
        app.canvas.text(detail, t::kSubheadline, {r.x, r.y + 22, r.w, 14}, col(disabled ? app.c->label_tertiary : app.c->label_secondary));
    if (!disabled)
        app.hits.push_back({id, track});
}

float checkbox_height(const std::wstring& desc, float width) {
    return t::kBody.lineHeight + (desc.empty() ? 0 : 2 + app.canvas.measure_height(desc, t::kCallout, width - 22));
}

void checkbox(Ctl id, const Rect& r, bool checked, const std::wstring& label, const std::wstring& desc) {
    const Rect box{r.x, r.y + 1, 14, 14};
    if (checked) {
        app.canvas.fill(box, col(app.c->accent_fill), t::kRadiusCheck);
        app.canvas.path("M4 7.2l2.1 2.1L10.2 4.6", box, 14, col(app.c->on_accent), 2);
    } else {
        app.canvas.control_shadow(box, t::kRadiusCheck, app.dark);
        app.canvas.fill(box, col(app.c->control_bg), t::kRadiusCheck);
        app.canvas.stroke(box, col(app.c->control_border), 1, t::kRadiusCheck);
    }
    if (focused(id))
        focus_ring(box, t::kRadiusCheck);
    const float tx = r.x + 14 + t::kSpace2, tw = r.w - 14 - t::kSpace2;
    app.canvas.text(label, t::kBody, {tx, r.y, tw, t::kBody.lineHeight}, col(app.c->label));
    if (!desc.empty())
        app.canvas.text(desc, t::kCallout, {tx, r.y + t::kBody.lineHeight + 2, tw, 200}, col(app.c->label_secondary),
                        Align::Leading, false, true);
    app.hits.push_back({id, {r.x, r.y, 14 + t::kSpace2 + app.canvas.measure(label, t::kBody), t::kBody.lineHeight + 2}});
}

float segmented_width() {
    float w = 4;
    for (const auto& name : kColorNames)
        w += t::kSpace3 * 2 + 10 + 6 + app.canvas.measure(name, t::kBody) + 2;
    return std::ceil(w - 2);
}

uint32_t overlay(int i) { return i == 0 ? app.c->overlay_teal : i == 1 ? app.c->overlay_magenta : app.c->overlay_green; }

void segmented(Ctl id, const Rect& r, int selected, bool disabled) {
    app.canvas.fill(r, col(app.c->segment_track), t::kRadiusControl);
    float x = r.x + 2;
    for (int i = 0; i < 3; ++i) {
        const float w = t::kSpace3 * 2 + 10 + 6 + app.canvas.measure(kColorNames[i], t::kBody);
        const Rect item{x, r.y + 2, w, r.h - 4};
        if (i == selected) {
            if (!disabled)
                app.canvas.control_shadow(item, t::kRadiusControl - 2, app.dark);
            app.canvas.fill(item, col(app.c->control_bg), t::kRadiusControl - 2);
        }
        app.canvas.circle(item.x + t::kSpace3 + 5, item.y + item.h / 2, 5, col(overlay(i), disabled ? 0.4f : 1), true);
        app.canvas.text(kColorNames[i], t::kBody, {item.x + t::kSpace3 + 16, item.y, w - t::kSpace3 - 16, item.h},
                        col(disabled ? app.c->label_tertiary : app.c->label));
        if (i == selected && focused(id))
            focus_ring(item, t::kRadiusControl - 2);
        if (!disabled)
            app.hits.push_back({id, item, i});
        x += w + 2;
    }
}

void glyph(int tone, float x, float y, Color c) {
    const Rect r{x, y, 12, 12};
    if (tone == 0) {  // success: circle + check
        app.canvas.path("M3.75 6.1l1.5 1.5 3-3.2", r, 12, c, 1.3f);
        app.canvas.circle(x + 6, y + 6, 5, c, false, 1.3f);
    } else if (tone == 1) {  // warning: triangle + mark
        app.canvas.path("M6 1.5l5 9H1zM6 5v2.2", r, 12, c, 1.3f);
        app.canvas.circle(x + 6, y + 8.8f, 0.6f, c, true);
    } else if (tone == 2) {  // danger: circle + x
        app.canvas.circle(x + 6, y + 6, 5, c, false, 1.3f);
        app.canvas.path("M4.25 4.25l3.5 3.5M7.75 4.25l-3.5 3.5", r, 12, c, 1.3f);
    } else {
        app.canvas.circle(x + 6, y + 6, 2.5f, c, true);
    }
}

// StatusBadge: tone 0 success, 1 warning, 2 danger, 3 neutral. Returns its width.
float badge(float x, float y, int tone, const std::wstring& text, float maxWidth = 10000) {
    const t::TypeStyle style{11, 14, 500, false};
    const float w = std::min(maxWidth, 7 + 12 + 5 + std::ceil(app.canvas.measure(text, style)) + 9);
    const uint32_t bg[] = {app.c->success_bg, app.c->warning_bg, app.c->danger_bg, app.c->segment_track};
    const uint32_t fg[] = {app.c->success, app.c->warning, app.c->danger, app.c->label};
    app.canvas.fill({x, y, w, 20}, col(bg[tone]), 10);
    glyph(tone, x + 7, y + 4, col(fg[tone]));
    app.canvas.text(text, style, {x + 24, y, w - 24 - 9, 20}, col(fg[tone]));
    return w;
}

// ---- Form layout -------------------------------------------------------------------------------

// One FormRow inside a section box: label (+ description) leading, the control trailing.
struct Row {
    std::wstring label, desc;
    float controlW = 0, controlH = 0;
    bool disabled = false;
    std::function<void(const Rect&)> control;  // drawn at its rect
    std::function<float(float)> fullHeight;    // full-width rows (checkboxes): height for a width
    std::function<void(const Rect&)> full;
};

float row_height(const Row& row, float w) {
    const float inner = w - 2 * t::kSpace3;
    if (row.full)
        return std::max(t::kRowMinHeight, row.fullHeight(inner) + 2 * t::kSpace2);
    const float textW = inner - row.controlW - t::kSpace4;
    float textH = t::kBody.lineHeight;
    if (!row.desc.empty())
        textH += 2 + app.canvas.measure_height(row.desc, t::kCallout, textW);
    return std::max(t::kRowMinHeight, std::max(textH, row.controlH) + 2 * t::kSpace2);
}

// FormSection at (x, y), width w. Returns its height.
float section(float x, float y, float w, const std::wstring& title, const std::vector<Row>& rows, const std::wstring& footer,
              bool draw) {
    float h = 0;
    if (!title.empty()) {
        if (draw)
            app.canvas.text(title, t::kHeadline, {x + t::kSpace3, y, w - t::kSpace3, 16}, col(app.c->label));
        h += 16 + 6;
    }
    float boxH = 0;
    for (const Row& row : rows)
        boxH += row_height(row, w);
    const Rect box{x, y + h, w, boxH};
    if (draw) {
        app.canvas.fill(box, col(app.c->group_bg), t::kRadiusGroup);
        app.canvas.stroke(box, col(app.c->separator), 0.5f, t::kRadiusGroup);
        float ry = box.y;
        for (size_t i = 0; i < rows.size(); ++i) {
            const Row& row = rows[i];
            const float rh = row_height(row, w);
            if (i > 0)
                app.canvas.line(box.x + t::kSpace3, ry, box.right() - t::kSpace3, ry, col(app.c->separator), 0.5f);
            const Rect inner{box.x + t::kSpace3, ry + t::kSpace2, w - 2 * t::kSpace3, rh - 2 * t::kSpace2};
            if (row.full) {
                const float fh = row.fullHeight(inner.w);
                row.full({inner.x, inner.y + (inner.h - fh) / 2, inner.w, fh});
            } else {
                const float textW = inner.w - row.controlW - t::kSpace4;
                float textH = t::kBody.lineHeight;
                const float descH = row.desc.empty() ? 0 : app.canvas.measure_height(row.desc, t::kCallout, textW);
                if (!row.desc.empty())
                    textH += 2 + descH;
                const float ty = inner.y + (inner.h - textH) / 2;
                const Color labelColor = col(row.disabled ? app.c->label_tertiary : app.c->label);
                app.canvas.text(row.label, t::kBody, {inner.x, ty, textW, t::kBody.lineHeight}, labelColor);
                if (!row.desc.empty())
                    app.canvas.text(row.desc, t::kCallout, {inner.x, ty + t::kBody.lineHeight + 2, textW, descH + 2},
                                    col(row.disabled ? app.c->label_tertiary : app.c->label_secondary), Align::Leading, false, true);
                row.control({inner.right() - row.controlW, inner.y + (inner.h - row.controlH) / 2, row.controlW, row.controlH});
            }
            ry += rh;
        }
    }
    h += boxH;
    if (!footer.empty()) {
        const float fw = w - 2 * t::kSpace3, fh = app.canvas.measure_height(footer, t::kSubheadline, fw);
        if (draw)
            app.canvas.text(footer, t::kSubheadline, {x + t::kSpace3, y + h + 6 + 2, fw, fh + 2}, col(app.c->label_secondary),
                            Align::Leading, false, true);
        h += 6 + 2 + fh;
    }
    return h;
}

std::wstring px(Size s) { return std::to_wstring(s.width) + L" × " + std::to_wstring(s.height) + L" px"; }
std::wstring dims(Size s) { return std::to_wstring(s.width) + L" × " + std::to_wstring(s.height); }

Row slider_row(Ctl id, const std::wstring& label, const std::wstring& desc, int value, const std::wstring& detail,
               bool disabled = false, const wchar_t* unit = L"%") {
    Row row;
    row.label = label;
    row.desc = desc;
    row.controlW = 240;
    row.controlH = detail.empty() ? 20.0f : 36.0f;
    row.disabled = disabled;
    const Range range = slider_range(id);
    row.control = [=](const Rect& r) { slider(id, r, value, range, detail, disabled, unit); };
    return row;
}

// The current pane's content at (x, y), width w. Returns its height.
float pane_content(float x, float y, float w, bool draw) {
    const Readouts ro = readouts(app.s, app.runtime);
    const bool qvfr = app.s.mode == RenderMode::Qvfr;
    float h = 0;
    switch (app.pane) {
    case Pane::Rendering: {
        Row method, mode;
        method.label = L"Rendering method";
        {
            const std::wstring names[2] = {kMethodNames[0], kMethodNames[1]};
            method.controlW = popup_width(names, 2, 180);
        }
        method.controlH = t::kControlHeight;
        method.control = [](const Rect& r) { popup_button(kMethod, r, kMethodNames[app.s.mode == RenderMode::Qvfr ? 0 : 1], false); };
        mode.label = L"Foveated mode";
        mode.desc = app.s.eyeTracking ? L"The focus region follows your gaze." : L"The focus region stays at the center of each lens.";
        {
            const std::wstring names[2] = {kModeNames[0], kModeNames[1]};
            mode.controlW = popup_width(names, 2, 140);
        }
        mode.controlH = t::kControlHeight;
        mode.control = [](const Rect& r) { popup_button(kMode, r, kModeNames[app.s.eyeTracking ? 0 : 1], false); };
        h += section(x, y, w, L"Rendering", {method, mode},
                     qvfr ? L"Quad View Foveated Rendering draws a sharp focus view inside a lower-resolution outer view for each eye."
                          : L"Two-loop Foveated Interlaced renders each headset frame twice: the full view, then the focus view.",
                     draw);
        break;
    }
    case Pane::Resolution: {
        const bool known = app.runtime.known();
        const std::wstring footer =
            known ? L"Percent of the headset’s recommended resolution (" + std::to_wstring(app.runtime.width) + L" × " +
                        std::to_wstring(app.runtime.height) + L" px per eye). Outer and inner are set independently. Applies at next launch."
                  : L"Percent of the headset’s recommended resolution. Pixel sizes appear after the first launch. Outer and inner are "
                    L"set independently. Applies at next launch.";
        std::vector<Row> rows = {
            slider_row(kOuter, L"Outer resolution", L"The full field of view, rendered at lower density.", app.s.outer,
                       known ? px(ro.outer) : L""),
            slider_row(kInner, L"Inner resolution",
                       qvfr ? L"Pixel density inside the focus region."
                            : L"Set by outer resolution and foveated size in Two-loop Foveated Interlaced.",
                       qvfr ? app.s.inner : static_cast<int>(std::lround(ro.focusDensity * 100)),
                       known && qvfr ? px(ro.focus) : L"", !qvfr),
        };
        h += section(x, y, w, L"Per-eye resolution", rows, footer, draw);
        break;
    }
    case Pane::Foveation: {
        if (eye_warning()) {
            if (draw)
                badge(x, y, 1, L"Eye tracker not detected. The focus region stays centered until one connects.", w);
            h += 20 + t::kSpace6;
        }
        h += section(x, y + h, w, L"Focus region",
                     {slider_row(kWidth, L"Foveated width", L"", app.s.width, L""),
                      slider_row(kHeight, L"Foveated height", L"", app.s.height, L""),
                      slider_row(kCorner, L"Corner radius", L"Rounds the corners of the focus region.", app.s.corner, L"")},
                     L"Share of the outer view that the focus view covers.", draw);
        break;
    }
    case Pane::Overlay: {
        Row show;
        const std::wstring desc = L"Draws a colored border around the focus region.";
        show.fullHeight = [desc](float width) { return checkbox_height(desc, width); };
        show.full = [desc](const Rect& r) { checkbox(kOutline, r, app.s.outline, L"Show focus outline", desc); };
        Row color;
        color.label = L"Outline color";
        color.disabled = !app.s.outline;
        color.controlW = segmented_width();
        color.controlH = t::kControlHeight;
        color.control = [](const Rect& r) { segmented(kColor, r, static_cast<int>(app.s.color), !app.s.outline); };
        h += section(x, y, w, L"Focus overlay",
                     {show, color, slider_row(kFeather, L"Feather", L"Softens the edge of the focus region.", app.s.feather, L"")},
                     L"The outline is drawn into the headset image, to show where the focus region is.", draw);
        break;
    }
    case Pane::Menu: {
        h += section(x, y, w, L"Placement",
                     {slider_row(kMenuYaw, L"Horizontal", L"Degrees right of straight ahead. Negative is left.", app.s.menuYaw,
                                 L"", false, L"°"),
                      slider_row(kMenuPitch, L"Vertical", L"Degrees above eye level. Negative is below.", app.s.menuPitch, L"",
                                 false, L"°"),
                      slider_row(kMenuWidth, L"Size", L"Width of the panel, in degrees of your view.", app.s.menuWidth, L"",
                                 false, L"°")},
                     L"The settings menu stays 0.8 m in front of your head, wherever you look. Drag it in the preview, "
                     L"or move it from the menu itself in the headset. While the game runs, it moves at once.",
                     draw);
        const std::wstring open = L"Ctrl+Alt+arrows while the game has focus, or click the left controller stick. The right "
                                  L"stick selects and changes values; the menu hides after a few seconds.";
        const float oh = app.canvas.measure_height(open, t::kCallout, w - 2 * t::kSpace3);
        if (draw) {
            app.canvas.text(L"Opening the menu", t::kHeadline, {x + t::kSpace3, y + h + t::kSpace6, w - t::kSpace3, 16},
                            col(app.c->label));
            app.canvas.text(open, t::kCallout, {x + t::kSpace3, y + h + t::kSpace6 + 16 + 6, w - 2 * t::kSpace3, oh + 2},
                            col(app.c->label_secondary), Align::Leading, false, true);
        }
        h += t::kSpace6 + 16 + 6 + oh;
        break;
    }
    case Pane::About: {
        if (draw) {
            app.canvas.text(L"QVFR Launcher", t::kTitle1, {x, y, w, 26}, col(app.c->label));
            app.canvas.text(L"Starts Assetto Corsa EVO with foveated rendering enabled.", t::kBody, {x, y + 26 + 8, w, 16},
                            col(app.c->label_secondary));
            app.canvas.text(L"Version " + widen(ACE_QV_VERSION), t::kBody, {x, y + 26 + 8 + 16 + 8, w, 16},
                            col(app.c->label_secondary));
        }
        h += 26 + 8 + 16 + 8 + 16;
        // The same notice as the README's: unofficial, at the user's own risk.
        const std::wstring notice =
            L"ace-quadview is not made, approved or sanctioned by Kunos Simulazioni or the Assetto Corsa EVO "
            L"developers. It hooks the game's code and changes how it renders, so a game update can break it, and "
            L"it can crash the game. Use it in offline sessions only, and please don't report problems you see "
            L"while using it to the ACE developers.";
        const float nh = app.canvas.measure_height(notice, t::kCallout, w);
        if (draw) {
            app.canvas.text(L"Experimental and unofficial. Use at your own risk.", t::kHeadline,
                            {x, y + h + t::kSpace6, w, 16}, col(app.c->label));
            app.canvas.text(notice, t::kCallout, {x, y + h + t::kSpace6 + 16 + t::kSpace2, w, nh + 2},
                            col(app.c->label_secondary), Align::Leading, false, true);
        }
        h += t::kSpace6 + 16 + t::kSpace2 + nh;
        // Support: a line and a button that opens the Ko-fi page in the browser.
        const std::wstring support = L"If ace-quadview improves your races, you can support its development on Ko-fi.";
        const float sh = app.canvas.measure_height(support, t::kCallout, w);
        if (draw)
            app.canvas.text(support, t::kCallout, {x, y + h + t::kSpace6, w, sh + 2}, col(app.c->label_secondary),
                            Align::Leading, false, true);
        h += t::kSpace6 + sh + t::kSpace2;
        const float bw = std::ceil(app.canvas.measure(L"Support on Ko-fi", t::kBody)) + 2 * t::kSpace3;
        if (draw)
            button(kSupport, {x, y + h, bw, t::kControlHeight}, L"Support on Ko-fi", false);
        h += t::kControlHeight;
        break;
    }
    }
    return h;
}

// FoveationPreview (one eye, a schematic) at (x, y), width w. Returns its height.
float preview(float x, float y, float w, bool draw) {
    const Readouts ro = readouts(app.s, app.runtime);
    const bool known = app.runtime.known();
    // Without a recommended size, the kit's example aspect (2584 x 2160) shapes the drawing.
    const Size outer = known ? ro.outer : Size{2584 * app.s.outer / 100, 2160 * app.s.outer / 100};
    const float W = 320, H = std::round(W * outer.height / std::max(1, outer.width)), k = w / W;
    const float frameH = H * k;
    if (draw) {
        const Rect frame{x, y, w, frameH};
        ComPtr<ID2D1RoundedRectangleGeometry> clip;
        app.canvas.factory()->CreateRoundedRectangleGeometry(D2D1::RoundedRect(frame.d2d(), t::kRadiusGroup, t::kRadiusGroup), &clip);
        ComPtr<ID2D1Layer> layer;
        ID2D1RenderTarget* rt = app.canvas.target();
        rt->CreateLayer(&layer);
        rt->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), clip.Get()), layer.Get());
        const auto R = [&](float rx, float ry, float rw, float rh) { return Rect{x + rx * k, y + ry * k, rw * k, rh * k}; };
        const bool tfi = app.s.mode == RenderMode::TwoLoop;
        const bool eye = app.s.eyeTracking && !eye_warning();
        const float fw = W * app.s.width / 100, fh = H * app.s.height / 100;
        const float gx = eye ? 0.6f : 0.5f, gy = eye ? 0.45f : 0.5f;
        const float fx = std::clamp(gx * W - fw / 2, 0.0f, W - fw), fy = std::clamp(gy * H - fh / 2, 0.0f, H - fh);
        // Grid cell size shows pixel density: 8 in the periphery, 8 / ratio in the focus region.
        const double ratio = tfi ? ro.focusDensity * 100 / app.s.outer : ro.focusDensity * 100 / app.s.outer;
        const float pc = 8, fc = std::clamp(static_cast<float>(pc / std::max(0.01, ratio)), 2.0f, 8.0f);
        app.canvas.fill(R(0, 0, W, H), col(app.c->preview_periphery));
        for (float gxl = 0; gxl <= W; gxl += pc)
            app.canvas.line(x + gxl * k, y, x + gxl * k, y + frameH, col(app.c->preview_canvas), 0.6f * k);
        for (float gyl = 0; gyl <= H; gyl += pc)
            app.canvas.line(x, y + gyl * k, x + w, y + gyl * k, col(app.c->preview_canvas), 0.6f * k);
        if (tfi)
            for (float iy = 0; iy < H; iy += 4)
                app.canvas.fill(R(0, iy, W, 2), col(app.c->preview_canvas, 0.55f));
        // Feather: the focus region fades in over feather % of its size, as the compositor blends it;
        // its corners rounded by corner % of half the shorter side.
        const float band = std::min(fw, fh) * app.s.feather / 100.0f;
        const float corner = std::min(fw, fh) / 2 * app.s.corner / 100.0f;
        const int steps = band > 0.5f ? 6 : 1;
        for (int i = 0; i < steps; ++i) {
            const float in = band * i / steps, alpha = steps == 1 ? 1.0f : static_cast<float>(i + 1) / steps;
            app.canvas.fill(R(fx + in, fy + in, fw - 2 * in, fh - 2 * in), col(app.c->preview_focus, alpha),
                            std::max(0.0f, corner - in) * k);
        }
        // The focus region's finer grid (the kit's focus pattern), on its own grid origin, inside its shape.
        ComPtr<ID2D1RoundedRectangleGeometry> shape;
        app.canvas.factory()->CreateRoundedRectangleGeometry(D2D1::RoundedRect(R(fx, fy, fw, fh).d2d(), corner * k, corner * k), &shape);
        ComPtr<ID2D1Layer> focusLayer;
        rt->CreateLayer(&focusLayer);
        rt->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), shape.Get()), focusLayer.Get());
        for (float lx = fx; lx <= fx + fw; lx += fc)
            app.canvas.line(x + lx * k, y + fy * k, x + lx * k, y + (fy + fh) * k, col(app.c->preview_periphery), 0.4f * k);
        for (float ly = fy; ly <= fy + fh; ly += fc)
            app.canvas.line(x + fx * k, y + ly * k, x + (fx + fw) * k, y + ly * k, col(app.c->preview_periphery), 0.4f * k);
        rt->PopLayer();
        if (app.s.outline)
            app.canvas.stroke(R(fx, fy, fw, fh), col(overlay(static_cast<int>(app.s.color))), 2 * k, corner * k);
        if (eye) {
            const float cx = x + gx * W * k, cy = y + gy * H * k;
            const Color label = col(app.c->preview_label);
            app.canvas.circle(cx, cy, 5 * k, label, false, 1.2f * k);
            app.canvas.line(cx - 9 * k, cy, cx - 5 * k, cy, label, 1.2f * k);
            app.canvas.line(cx + 5 * k, cy, cx + 9 * k, cy, label, 1.2f * k);
            app.canvas.line(cx, cy - 9 * k, cx, cy - 5 * k, label, 1.2f * k);
            app.canvas.line(cx, cy + 5 * k, cx, cy + 9 * k, label, 1.2f * k);
        }
        const t::TypeStyle tiny{9.5f * k, 12 * k, 400, false};
        if (known) {
            app.canvas.text(L"Focus " + dims(ro.focus), tiny, R(fx + 6, fy + 4, W - fx - 6, 12), col(app.c->preview_label), Align::Leading, false);
            app.canvas.text(L"Outer " + dims(ro.outer), tiny, R(8, H - 17, W - 16, 12), col(app.c->preview_label), Align::Leading, false);
        }
        rt->PopLayer();
        app.canvas.stroke(frame, col(app.c->separator), 0.5f, t::kRadiusGroup);
    }
    float h = frameH + t::kSpace2;
    // Caption: method · mode · focus density.
    const std::wstring caption = std::wstring(kMethodNames[app.s.mode == RenderMode::Qvfr ? 0 : 1]) + L"  ·  " +
                                 (app.s.eyeTracking && !eye_warning() ? L"Eye tracked" : L"Fixed") + L"  ·  Focus density " +
                                 std::to_wstring(static_cast<int>(std::lround(ro.focusDensity * 100))) + L"%";
    const float ch = app.canvas.measure_height(caption, t::kSubheadline, w);
    if (draw)
        app.canvas.text(caption, t::kSubheadline, {x, y + h, w, ch + 2}, col(app.c->label_secondary), Align::Leading, false, true);
    h += ch + t::kSpace4;
    // Readouts.
    const t::TypeStyle dt{12, 16, 400, false};
    const std::wstring dash = L"—";
    char mp[32], saved[48];
    std::snprintf(mp, sizeof(mp), "%.2f M of %.2f M", ro.pixels / 1e6, ro.runtimePixels / 1e6);
    const int savedPercent = static_cast<int>(std::lround(ro.saved * 100));
    if (savedPercent >= 0)
        std::snprintf(saved, sizeof(saved), "%d%%", savedPercent);
    else
        std::snprintf(saved, sizeof(saved), "None (%d%% more)", -savedPercent);
    const std::pair<std::wstring, std::wstring> lines[] = {
        {L"Runtime resolution", known ? dims(ro.runtime) : dash},
        {L"Outer view", known ? dims(ro.outer) : dash},
        {L"Focus view", known ? dims(ro.focus) : dash},
        {L"Pixels per frame", known ? widen(mp) : dash},
        {L"Pixels saved", known ? widen(saved) : dash},
        {L"Center PPD", ro.centerPpd > 0 ? std::to_wstring(std::lround(ro.centerPpd)) : dash},
    };
    for (const auto& [name, value] : lines) {
        if (draw) {
            app.canvas.text(name, dt, {x, y + h, w / 2, 16}, col(app.c->label_secondary));
            app.canvas.text(value, t::kValue, {x + w / 2, y + h, w / 2, 16}, col(app.c->label), Align::Trailing);
        }
        h += 16 + 4;
    }
    return h - 4;
}

// Angular size of the panel: its height in degrees for a width in degrees, at any distance.
float menu_height_deg(int widthDeg) {
    constexpr float kRad = 3.14159265f / 180;
    return 2 * std::atan(std::tan(widthDeg * kRad / 2) * kMenuAspect) / kRad;
}

std::wstring degrees(int v, const wchar_t* plus, const wchar_t* minus) {
    return v == 0 ? L"0°" : std::to_wstring(std::abs(v)) + L"° " + (v > 0 ? plus : minus);
}

// The settings menu's place (Headset menu pane) at (x, y), width w: a kRefFovX x kRefFovY view in
// degrees with the panel, drawn in its own colours, where it floats. Returns its height.
float placement_preview(float x, float y, float w, bool draw) {
    const float k = w / kRefFovX, frameH = std::round(kRefFovY * k);
    const Rect frame{x, y, w, frameH};
    const float pw = app.s.menuWidth * k, ph = menu_height_deg(app.s.menuWidth) * k;
    const float pcx = x + (kRefFovX / 2 + app.s.menuYaw) * k, pcy = y + (kRefFovY / 2 - app.s.menuPitch) * k;
    const Rect panel{pcx - pw / 2, pcy - ph / 2, pw, ph};
    if (draw) {
        app.placeFrame = frame;
        ComPtr<ID2D1RoundedRectangleGeometry> clip;
        app.canvas.factory()->CreateRoundedRectangleGeometry(D2D1::RoundedRect(frame.d2d(), t::kRadiusGroup, t::kRadiusGroup), &clip);
        ComPtr<ID2D1Layer> layer;
        ID2D1RenderTarget* rt = app.canvas.target();
        rt->CreateLayer(&layer);
        rt->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), clip.Get()), layer.Get());
        app.canvas.fill(frame, col(app.c->preview_periphery));
        // A 10° grid, with the horizon and straight ahead stronger.
        for (float d = 10; d < kRefFovX; d += 10)
            app.canvas.line(x + d * k, y, x + d * k, y + frameH, col(app.c->preview_canvas), 0.6f);
        for (float d = 10; d < kRefFovY; d += 10)
            app.canvas.line(x, y + d * k, x + w, y + d * k, col(app.c->preview_canvas), 0.6f);
        const float cx = x + w / 2, cy = y + frameH / 2;
        const Color axis = col(app.c->preview_label, 0.35f);
        app.canvas.line(cx, y, cx, y + frameH, axis, 1);
        app.canvas.line(x, cy, x + w, cy, axis, 1);
        app.canvas.circle(cx, cy, 5, col(app.c->preview_label, 0.8f), false, 1.2f);
        // The panel as the headset shows it: title, resolution line, the settings rows (the first
        // selected) and the hint, in its own colours (in panel pixels, 1024 x 922).
        const bool active = app.dragging == kMenuPanel;
        app.canvas.fill({panel.x, panel.y + 2, panel.w, panel.h}, {0, 0.35f}, 4);
        app.canvas.fill(panel, col(kMenuBackground, 0.92f), 4);
        const float m = pw * 36 / 1024, u = ph / 922;
        const auto bar = [&](float bx, float by, float bw, float bh, Color c) {
            app.canvas.fill({panel.x + bx, panel.y + by * u, bw, std::max(1.5f, bh * u)}, c, 1);
        };
        bar(m, 30, pw * 0.42f, 34, col(kMenuLabel));
        bar(m, 100, pw * 0.36f, 22, col(kMenuDim));
        bar(pw / 2, 100, pw * 0.3f, 22, col(kMenuLabel, 0.8f));
        for (int i = 0; i < 11; ++i) {
            const float ry = 200 + 50.0f * i;
            if (i == 0)
                app.canvas.fill({panel.x + m * 0.55f, panel.y + (ry - 8) * u, pw - 1.1f * m, 50 * u}, col(kMenuHighlight), 1);
            bar(m, ry + 8, pw * (0.22f + 0.04f * (i % 3)), 18, col(kMenuLabel, 0.85f));
            const float vw = pw * 0.12f;
            bar(pw - m - vw, ry + 8, vw, 18, col(kMenuLabel, 0.85f));
        }
        bar(m, 880, pw * 0.6f, 14, col(kMenuDim, 0.7f));
        app.canvas.stroke(panel, col(active ? app.c->accent : app.c->preview_label, active ? 1 : 0.45f), active ? 1.5f : 0.75f, 4);
        if (focused(kMenuPanel))
            focus_ring(panel, 4);
        const t::TypeStyle tiny{9.5f, 12, 400, false};
        app.canvas.text(L"Reference view  " + std::to_wstring(static_cast<int>(kRefFovX)) + L"° × " +
                            std::to_wstring(static_cast<int>(kRefFovY)) + L"°",
                        tiny, {x + 8, y + 5, w - 16, 12}, col(app.c->preview_label, 0.75f), Align::Leading, false);
        rt->PopLayer();
        app.canvas.stroke(frame, col(app.c->separator), 0.5f, t::kRadiusGroup);
        app.hits.push_back({kMenuPanel, frame, 0});  // empty space: the panel's centre jumps there
        app.hits.push_back({kMenuPanel, panel, 1});  // the panel: dragged from where it is held
    }
    float h = frameH + t::kSpace2;
    const std::wstring caption = L"Drag the panel to place it  ·  Headsets’ fields of view vary";
    const float ch = app.canvas.measure_height(caption, t::kSubheadline, w);
    if (draw)
        app.canvas.text(caption, t::kSubheadline, {x, y + h, w, ch + 2}, col(app.c->label_secondary), Align::Leading, false, true);
    h += ch + t::kSpace4;
    const t::TypeStyle dt{12, 16, 400, false};
    char size[48], metres[48], distance[16];
    std::snprintf(size, sizeof(size), "%d° × %d°", app.s.menuWidth, static_cast<int>(std::lround(menu_height_deg(app.s.menuWidth))));
    std::snprintf(metres, sizeof(metres), "%.2f m", 2 * kMenuDistance * std::tan(app.s.menuWidth * 3.14159265 / 360));
    std::snprintf(distance, sizeof(distance), "%.1f m", kMenuDistance);
    const std::pair<std::wstring, std::wstring> lines[] = {
        {L"Horizontal", degrees(app.s.menuYaw, L"right", L"left")},
        {L"Vertical", degrees(app.s.menuPitch, L"up", L"down")},
        {L"Panel size", widen(size)},
        {L"Panel width", widen(metres)},
        {L"Distance", widen(distance)},
    };
    for (const auto& [name, value] : lines) {
        if (draw) {
            app.canvas.text(name, dt, {x, y + h, w / 2, 16}, col(app.c->label_secondary));
            app.canvas.text(value, t::kValue, {x + w / 2, y + h, w / 2, 16}, col(app.c->label), Align::Trailing);
        }
        h += 16 + 4;
    }
    return h - 4;
}

// The preview column: the placement preview on the Headset menu pane, the foveation preview elsewhere.
float side_preview(float x, float y, float w, bool draw) {
    return app.pane == Pane::Menu ? placement_preview(x, y, w, draw) : preview(x, y, w, draw);
}

// ---- Window ------------------------------------------------------------------------------------

struct Item {
    const wchar_t* label;
    const char* icon[3];
};
// The kit's sidebar glyphs (16 x 16, 1.5 stroke); circles and rects are drawn as paths.
const Item kItems[] = {
    {L"Rendering", {"M8 2l6 3-6 3-6-3 6-3z", "M2 8.5l6 3 6-3", "M2 11.5l6 3 6-3"}},
    {L"Resolution", {"M3.25 2.75H12.75C13.6 2.75 14.25 3.4 14.25 4.25V9.75C14.25 10.6 13.6 11.25 12.75 11.25H3.25C2.4 11.25 1.75 10.6 1.75 9.75V4.25C1.75 3.4 2.4 2.75 3.25 2.75Z", "M5.5 14h5M8 11.25V14", nullptr}},
    {L"Foveation", {"M1.5 8S4 3.5 8 3.5 14.5 8 14.5 8 12 12.5 8 12.5 1.5 8 1.5 8z", nullptr, nullptr}},
    {L"Focus overlay", {"M5 5H11V11H5Z", nullptr, nullptr}},
    {L"Headset menu", {"M3.25 3.25H12.75C13.6 3.25 14.25 3.9 14.25 4.75V11.25C14.25 12.1 13.6 12.75 12.75 12.75H3.25C2.4 12.75 1.75 12.1 1.75 11.25V4.75C1.75 3.9 2.4 3.25 3.25 3.25Z", "M4.5 6.5H8.5", "M4.5 9.5H11.5"}},
    {L"About", {"M8 7.25V11", nullptr, nullptr}},
};

void sidebar_icon(int i, const Rect& r, Color c) {
    for (const char* p : kItems[i].icon)
        if (p)
            app.canvas.path(p, r, 16, c, 1.5f);
    if (i == 2)
        app.canvas.circle(r.x + 8, r.y + 8, 2, c, false, 1.5f);
    if (i == 3)  // the outer square, dashed 2 / 2 along each edge
        for (float d = 1.75f; d < 14.25f; d += 4) {
            const float e = std::min(d + 2, 14.25f);
            app.canvas.line(r.x + d, r.y + 1.75f, r.x + e, r.y + 1.75f, c, 1.5f);
            app.canvas.line(r.x + d, r.y + 14.25f, r.x + e, r.y + 14.25f, c, 1.5f);
            app.canvas.line(r.x + 1.75f, r.y + d, r.x + 1.75f, r.y + e, c, 1.5f);
            app.canvas.line(r.x + 14.25f, r.y + d, r.x + 14.25f, r.y + e, c, 1.5f);
        }
    if (i == 5) {
        app.canvas.circle(r.x + 8, r.y + 8, 6.25f, c, false, 1.5f);
        app.canvas.circle(r.x + 8, r.y + 5, 0.9f, c, true);
    }
}

// The open pop-up menu: under its button, as wide as the button or its longest item (check mark
// column included), right-aligned with the button when it would leave the window.
Rect popup_menu_rect() {
    const Popup& p = app.popup;
    float w = p.anchor.w;
    for (const auto& item : p.items)
        w = std::max(w, std::ceil(app.canvas.measure(item, t::kBody)) + 22 + 4 + 10 + 8);
    Rect menu{p.anchor.x, p.anchor.y + p.anchor.h + 4, w, 22.0f * p.items.size() + 10};
    if (menu.right() > app.canvas.width() - 8)
        menu.x = p.anchor.right() - w;
    if (menu.bottom() > app.canvas.height())
        menu.y = p.anchor.y - menu.h - 4;
    return menu;
}

void paint() {
    if (!app.canvas.begin())
        return;
    app.hits.clear();
    Canvas& cv = app.canvas;
    const float W = cv.width(), Hh = cv.height();
    const float actionH = 10 + t::kControlHeightLarge + 10;
    const float bodyH = Hh - actionH;
    cv.fill({0, 0, W, Hh}, col(app.c->window_bg));

    // Sidebar.
    cv.fill({0, 0, t::kSidebarWidth, bodyH}, col(app.c->sidebar_bg));
    cv.line(t::kSidebarWidth - 0.25f, 0, t::kSidebarWidth - 0.25f, bodyH, col(app.c->separator), 0.5f);
    float sy = t::kSpace2;
    for (int i = 0; i < kPanes; ++i) {
        const Rect row{10, sy, t::kSidebarWidth - 20, 28};
        const bool selected = static_cast<int>(app.pane) == i;
        if (selected)
            cv.fill(row, col(app.c->sidebar_selection), t::kRadiusControl);
        const Color fg = col(selected ? app.c->on_accent : app.c->label);
        sidebar_icon(i, {row.x + t::kSpace2, row.y + 6, 16, 16}, col(selected ? app.c->on_accent : app.c->accent));
        cv.text(kItems[i].label, t::kBody, {row.x + t::kSpace2 + 16 + t::kSpace2, row.y, row.w - 40, 28}, fg);
        if (i == 2 && eye_warning()) {
            const float bx = row.right() - t::kSpace2 - 16, by = row.y + 6;
            cv.circle(bx + 8, by + 8, 8, col(selected ? app.c->on_accent : app.c->warning_bg), true);
            const Color mark = col(selected ? app.c->accent_fill : app.c->warning);
            cv.line(bx + 8, by + 5, bx + 8, by + 8.5f, mark, 1.6f);
            cv.circle(bx + 8, by + 10.75f, 0.9f, mark, true);
        }
        if (selected && focused(kSidebar))
            focus_ring(row, t::kRadiusControl);
        app.hits.push_back({kSidebar, row, i});
        sy += 28 + 2;
    }

    // Main and preview columns; below 900 wide the preview drops under the form.
    const bool narrow = W < 900;
    const float mainX = t::kSidebarWidth, mainW = narrow ? W - mainX : W - mainX - t::kPreviewWidth;
    const float contentX = mainX + t::kSpace5, contentW = mainW - 2 * t::kSpace5;
    app.scrollArea = {mainX, 0, W - mainX, bodyH};
    cv.push_clip(app.scrollArea);
    const float y0 = t::kSpace5 - app.scroll;
    cv.text(kItems[static_cast<int>(app.pane)].label, t::kTitle2, {contentX, y0, contentW, 22}, col(app.c->label));
    float contentH = 22 + t::kSpace6;
    contentH += pane_content(contentX, y0 + contentH, contentW, true);
    float total = contentH + t::kSpace5;
    if (narrow) {
        const float py = y0 + total + t::kSpace2;
        cv.text(L"Preview", t::kHeadline, {contentX + 2, py, contentW, 16}, col(app.c->label));
        const float pw = std::min(contentW, t::kPreviewWidth - t::kSpace5);
        total += t::kSpace2 + 16 + t::kSpace4 + side_preview(contentX, py + 16 + t::kSpace4, pw, true) + t::kSpace5;
    }
    cv.pop_clip();
    app.scrollMax = std::max(0.0f, total + t::kSpace5 - bodyH);
    if (!narrow) {
        const float ax = W - t::kPreviewWidth, aw = t::kPreviewWidth - t::kSpace5;
        cv.text(L"Preview", t::kHeadline, {ax + 2, t::kSpace5, aw, 16}, col(app.c->label));
        side_preview(ax, t::kSpace5 + 16 + t::kSpace4, aw, true);
    }

    // Action bar: status badge, Restore Defaults, Launch.
    const Rect bar{0, bodyH, W, actionH};
    cv.fill(bar, col(app.c->titlebar_bg));
    cv.line(0, bodyH + 0.25f, W, bodyH + 0.25f, col(app.c->separator), 0.5f);
    const float launchW = std::ceil(cv.measure(L"Launch Assetto Corsa EVO", {15, 20, 500, false})) + 36;
    const Rect launchR{W - t::kSpace5 - launchW, bodyH + 10, launchW, t::kControlHeightLarge};
    const float restoreW = std::ceil(cv.measure(L"Restore Defaults", t::kBody)) + 2 * t::kSpace3;
    const Rect restoreR{launchR.x - t::kSpace3 - restoreW, bodyH + (actionH - t::kControlHeight) / 2, restoreW, t::kControlHeight};
    const float badgeMax = restoreR.x - t::kSpace3 - t::kSpace5;
    const float by = bodyH + (actionH - 20) / 2;
    const Readouts ro = readouts(app.s, app.runtime);
    if (app.status == Status::Starting)
        badge(t::kSpace5, by, 3, L"Starting Assetto Corsa EVO…", badgeMax);
    else if (app.status == Status::Error)
        badge(t::kSpace5, by, 2, app.error, badgeMax);
    else if (!app.iniFound)
        badge(t::kSpace5, by, 2, L"ace-quadview.ini not found next to the launcher.", badgeMax);
    else if (eye_warning())
        badge(t::kSpace5, by, 1, L"Eye tracker not detected", badgeMax);
    else if (app.runtime.known() && ro.overRuntimeLimit)
        badge(t::kSpace5, by, 1, L"Larger than the runtime’s maximum image size", badgeMax);
    else if (app.live == Live::NextLaunch)
        badge(t::kSpace5, by, 1, app.s.mode == RenderMode::Qvfr
                                     ? L"Applied live up to the launch resolution · the rest at the next launch"
                                     : L"Foveation applied live · resolution and method at the next launch",
              badgeMax);
    else if (app.live == Live::Applied)
        badge(t::kSpace5, by, 0, L"Applied to the running game", badgeMax);
    else
        badge(t::kSpace5, by, 0, L"Ready", badgeMax);
    button(kRestore, restoreR, L"Restore Defaults", false);
    button(kLaunch, launchR, L"Launch Assetto Corsa EVO", true);

    // An open pop-up menu, over everything.
    if (app.popup.open) {
        const Popup& p = app.popup;
        const float itemH = 22;
        const Rect menu = popup_menu_rect();
        cv.fill({menu.x, menu.y + 2, menu.w, menu.h}, {0, app.dark ? 0.5f : 0.18f}, t::kRadiusControl + 2);
        cv.fill(menu, col(app.c->control_bg), t::kRadiusControl + 2);
        cv.stroke(menu, col(app.c->separator), 1, t::kRadiusControl + 2);
        for (size_t i = 0; i < p.items.size(); ++i) {
            const Rect item{menu.x + 5, menu.y + 5 + itemH * i, menu.w - 10, itemH};
            const bool hot = static_cast<int>(i) == p.hover;
            if (hot)
                cv.fill(item, col(app.c->accent_fill), 4);
            if (static_cast<int>(i) == p.selected)
                cv.path("M3.5 8.2l2.6 2.6L12.5 4.5", {item.x + 4, item.y + 4, 14, 14}, 16, col(hot ? app.c->on_accent : app.c->label), 1.6f);
            cv.text(p.items[i], t::kBody, {item.x + 22, item.y, item.w - 26, itemH}, col(hot ? app.c->on_accent : app.c->label));
        }
    }
    cv.end();
}

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
    *v = app.dragRange.min + static_cast<int>(std::lround(f * (app.dragRange.max - app.dragRange.min)));
    changed(false);
    if (*v != before && app.iniFound)
        push_live();  // the running game follows the knob; the settings save once it is let go
}

// Dragging the panel in the placement preview: its centre follows the pointer (less where it was held).
void set_panel_from(float x, float y) {
    const float k = app.placeFrame.w / kRefFovX;
    if (k <= 0)
        return;
    const int yaw = std::clamp(static_cast<int>(std::lround((x - app.placeFrame.x) / k - kRefFovX / 2 + app.grabYaw)),
                               kMenuYawRange.min, kMenuYawRange.max);
    const int pitch = std::clamp(static_cast<int>(std::lround(kRefFovY / 2 - (y - app.placeFrame.y) / k + app.grabPitch)),
                                 kMenuPitchRange.min, kMenuPitchRange.max);
    const bool moved = yaw != app.s.menuYaw || pitch != app.s.menuPitch;
    app.s.menuYaw = yaw;
    app.s.menuPitch = pitch;
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
    app.popup.items = owner == kMethod ? std::vector<std::wstring>{kMethodNames[0], kMethodNames[1]}
                                       : std::vector<std::wstring>{kModeNames[0], kModeNames[1]};
    app.popup.selected = owner == kMethod ? (app.s.mode == RenderMode::Qvfr ? 0 : 1) : (app.s.eyeTracking ? 0 : 1);
    app.popup.hover = app.popup.selected;
    app.popup.open = true;
    InvalidateRect(app.hwnd, nullptr, FALSE);
}

void choose(Ctl owner, int option) {
    if (owner == kMethod)
        app.s.mode = option == 0 ? RenderMode::Qvfr : RenderMode::TwoLoop;
    else if (owner == kMode)
        app.s.eyeTracking = option == 0;
    changed();
}

void close_popup(bool accept) {
    if (accept && app.popup.hover >= 0)
        choose(app.popup.owner, app.popup.hover);
    app.popup.open = false;
    InvalidateRect(app.hwnd, nullptr, FALSE);
}

int popup_item_at(float x, float y) {
    const Popup& p = app.popup;
    const float itemH = 22;
    const Rect menu = popup_menu_rect();
    for (size_t i = 0; i < p.items.size(); ++i)
        if (Rect{menu.x + 5, menu.y + 5 + itemH * i, menu.w - 10, itemH}.contains(x, y))
            return static_cast<int>(i);
    return -1;
}

void activate(Ctl id, int option) {
    switch (id) {
    case kSidebar:
        app.pane = static_cast<Pane>(option);
        app.scroll = 0;
        break;
    case kMethod:
    case kMode: open_popup(id); return;
    case kOutline: app.s.outline = !app.s.outline; changed(); return;
    case kColor: app.s.color = static_cast<OutlineColor>(option); changed(); return;
    case kRestore:
        app.s = Settings{};
        changed();
        return;
    case kLaunch: launch(); return;
    case kSupport: ShellExecuteW(app.hwnd, L"open", kSupportUrl, nullptr, nullptr, SW_SHOWNORMAL); return;
    default: break;
    }
    InvalidateRect(app.hwnd, nullptr, FALSE);
}

float dip(LPARAM lp, bool y) {
    return (y ? GET_Y_LPARAM(lp) : GET_X_LPARAM(lp)) / app.canvas.dpi_scale();
}

void on_key(WPARAM key) {
    const bool shift = GetKeyState(VK_SHIFT) < 0;
    if (app.popup.open) {
        const int n = static_cast<int>(app.popup.items.size());
        if (key == VK_UP || key == VK_DOWN)
            app.popup.hover = (app.popup.hover + (key == VK_DOWN ? 1 : n - 1)) % n;
        else if (key == VK_RETURN || key == VK_SPACE)
            close_popup(true);
        else if (key == VK_ESCAPE)
            close_popup(false);
        InvalidateRect(app.hwnd, nullptr, FALSE);
        return;
    }
    if (key == VK_TAB) {
        const auto order = focus_order();
        auto it = std::find(order.begin(), order.end(), app.focus);
        int i = it == order.end() ? (shift ? 0 : -1) : static_cast<int>(it - order.begin());
        i = (i + (shift ? static_cast<int>(order.size()) - 1 : 1)) % static_cast<int>(order.size());
        app.focus = order[i];
        app.keyboard = true;
        InvalidateRect(app.hwnd, nullptr, FALSE);
        return;
    }
    if (key == VK_RETURN) {
        launch();
        return;
    }
    app.keyboard = true;
    if (int* v = slider_value(app.focus)) {
        const Range r = slider_range(app.focus);
        int nv = *v;
        if (key == VK_LEFT || key == VK_DOWN)
            nv -= 1;
        else if (key == VK_RIGHT || key == VK_UP)
            nv += 1;
        else if (key == VK_PRIOR)
            nv += 10;
        else if (key == VK_NEXT)
            nv -= 10;
        else if (key == VK_HOME)
            nv = r.min;
        else if (key == VK_END)
            nv = r.max;
        else
            return;
        *v = std::clamp(nv, r.min, r.max);
        changed();
        return;
    }
    switch (app.focus) {
    case kSidebar:
        if (key == VK_UP || key == VK_DOWN) {
            const int p = std::clamp(static_cast<int>(app.pane) + (key == VK_DOWN ? 1 : -1), 0, kPanes - 1);
            activate(kSidebar, p);
        }
        break;
    case kMenuPanel: {  // arrows move the panel a degree, Page Up / Page Down ten
        const int step = key == VK_PRIOR || key == VK_NEXT ? 10 : 1;
        const int dx = key == VK_RIGHT ? step : key == VK_LEFT ? -step : 0;
        const int dy = key == VK_UP || key == VK_PRIOR ? step : key == VK_DOWN || key == VK_NEXT ? -step : 0;
        if (dx || dy) {
            app.s.menuYaw = std::clamp(app.s.menuYaw + dx, kMenuYawRange.min, kMenuYawRange.max);
            app.s.menuPitch = std::clamp(app.s.menuPitch + dy, kMenuPitchRange.min, kMenuPitchRange.max);
            changed();
        }
        break;
    }
    case kColor:
        if (key == VK_LEFT || key == VK_RIGHT || key == VK_UP || key == VK_DOWN) {
            const int d = key == VK_RIGHT || key == VK_DOWN ? 1 : 2;
            activate(kColor, (static_cast<int>(app.s.color) + d) % 3);
        }
        break;
    case kMethod:
    case kMode:
    case kOutline:
    case kSupport:
    case kRestore:
    case kLaunch:
        if (key == VK_SPACE)
            activate(app.focus, -1);
        break;
    default: break;
    }
}

LRESULT CALLBACK window_proc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_CREATE:
        app.hwnd = hwnd;
        apply_theme();
        if (!app.canvas.initialize(hwnd))
            return -1;
        app.iniWritten = ini_written();
        SetTimer(hwnd, kFollowTimer, 500, nullptr);
        return 0;
    case WM_TIMER:
        if (wParam == kFollowTimer)
            follow_ini();
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        BeginPaint(hwnd, &ps);
        paint();
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_SIZE:
        app.canvas.resize();
        app.scroll = std::min(app.scroll, app.scrollMax);
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    case WM_DPICHANGED: {
        app.canvas.set_dpi(HIWORD(wParam));
        const RECT* r = reinterpret_cast<const RECT*>(lParam);
        SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    }
    case WM_GETMINMAXINFO: {
        const UINT dpi = GetDpiForWindow(hwnd);
        RECT r{0, 0, MulDiv(static_cast<int>(kMinWidth), dpi, 96), MulDiv(static_cast<int>(kMinHeight - kTitlebar), dpi, 96)};
        AdjustWindowRectExForDpi(&r, WS_OVERLAPPEDWINDOW, FALSE, 0, dpi);
        reinterpret_cast<MINMAXINFO*>(lParam)->ptMinTrackSize = {r.right - r.left, r.bottom - r.top};
        return 0;
    }
    case WM_SETTINGCHANGE:
        if (lParam && wcscmp(reinterpret_cast<const wchar_t*>(lParam), L"ImmersiveColorSet") == 0) {
            apply_theme();
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    case WM_ACTIVATE:
        if (LOWORD(wParam) != WA_INACTIVE) {  // a session may have reported the headset meanwhile
            app.runtime = load_runtime_info(runtime_info_path());
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        break;
    case WM_LBUTTONDOWN: {
        const float x = dip(lParam, false), y = dip(lParam, true);
        app.keyboard = false;
        if (app.popup.open) {
            app.popup.hover = popup_item_at(x, y);
            close_popup(app.popup.hover >= 0);
            return 0;
        }
        const Hit* h = hit_at(x, y);
        if (!h)
            return 0;
        app.focus = h->id;
        if (slider_value(h->id)) {
            app.dragging = h->id;
            app.dragTrack = h->r;
            app.dragRange = slider_range(h->id);
            SetCapture(hwnd);
            set_slider_from_x(x);
        } else if (h->id == kMenuPanel) {
            app.dragging = kMenuPanel;
            const float k = app.placeFrame.w / kRefFovX;
            // Held on the panel: that point stays under the pointer; on empty space, the centre jumps there.
            app.grabYaw = h->option == 1 ? app.s.menuYaw - ((x - app.placeFrame.x) / k - kRefFovX / 2) : 0;
            app.grabPitch = h->option == 1 ? app.s.menuPitch - (kRefFovY / 2 - (y - app.placeFrame.y) / k) : 0;
            SetCapture(hwnd);
            set_panel_from(x, y);
        } else if (h->id == kRestore || h->id == kLaunch || h->id == kSupport) {
            app.pressed = h->id;
            SetCapture(hwnd);
            InvalidateRect(hwnd, nullptr, FALSE);
        } else {
            activate(h->id, h->option);
        }
        return 0;
    }
    case WM_MOUSEMOVE: {
        const float x = dip(lParam, false), y = dip(lParam, true);
        if (app.dragging == kMenuPanel)
            set_panel_from(x, y);
        else if (app.dragging != kNone)
            set_slider_from_x(x);
        else if (app.popup.open) {
            const int hover = popup_item_at(x, y);
            if (hover != app.popup.hover) {
                app.popup.hover = hover;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
        }
        return 0;
    }
    case WM_LBUTTONUP: {
        ReleaseCapture();
        if (app.dragging != kNone) {
            app.dragging = kNone;
            changed();  // save once the knob is let go
        }
        if (app.pressed != kNone) {
            const Ctl pressed = app.pressed;
            app.pressed = kNone;
            const Hit* h = hit_at(dip(lParam, false), dip(lParam, true));
            if (h && h->id == pressed)
                activate(pressed, -1);
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    }
    case WM_MOUSEWHEEL:
        if (app.scrollMax > 0) {
            app.scroll = std::clamp(app.scroll - GET_WHEEL_DELTA_WPARAM(wParam) / 120.0f * 40, 0.0f, app.scrollMax);
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    case WM_KEYDOWN:
        on_key(wParam);
        return 0;
    case WM_GETDLGCODE:
        return DLGC_WANTALLKEYS;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

}  // namespace
}  // namespace ui

int run_window(HINSTANCE instance, int show) {
    using namespace ui;
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    app.ini = settings_path();
    app.iniFound = GetFileAttributesW(app.ini.c_str()) != INVALID_FILE_ATTRIBUTES;
    if (app.iniFound)
        app.s = load_settings(app.ini);
    app.runtime = load_runtime_info(runtime_info_path());
    app.focus = kLaunch;  // Return launches

    WNDCLASSEXW cls{sizeof(cls)};
    cls.lpfnWndProc = window_proc;
    cls.hInstance = instance;
    cls.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    cls.lpszClassName = L"QvfrLauncher";
    // The app icon (launcher.rc): the large one for Alt+Tab and the taskbar, the small one for the title bar.
    cls.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(1));
    cls.hIconSm = static_cast<HICON>(LoadImageW(instance, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON),
                                                GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR));
    RegisterClassExW(&cls);
    // The kit's 960 x 600 window includes its 38px title bar; the native caption replaces it.
    const UINT dpi = GetDpiForSystem();
    RECT r{0, 0, MulDiv(static_cast<int>(kDefaultWidth), dpi, 96), MulDiv(static_cast<int>(kDefaultHeight - kTitlebar), dpi, 96)};
    AdjustWindowRectExForDpi(&r, WS_OVERLAPPEDWINDOW, FALSE, 0, dpi);
    HWND hwnd = CreateWindowExW(0, cls.lpszClassName, L"QVFR Launcher", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                                r.right - r.left, r.bottom - r.top, nullptr, nullptr, instance, nullptr);
    if (!hwnd)
        return 1;
    ShowWindow(hwnd, show);
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return static_cast<int>(msg.wParam);
}

}  // namespace aceqv::launcher
