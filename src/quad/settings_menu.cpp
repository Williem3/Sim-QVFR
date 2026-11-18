#include "settings_menu.h"

#include "focus_mask.h"
#include "fov_crop.h"
#include "views4_layout.h"

#include <algorithm>
#include <cmath>

#include <windows.h>

#include "../ini.h"
#include "../lang.h"

namespace simqvfr {
namespace {

// The menu's words in the player's language for the panel (`shown`), or in English for the log ("settings menu: ...
// -> ..." stays English whatever the language). Literals passed here are marked I18N for tools/lang_extract.py.
struct Words {
    bool shown = true;
    const wchar_t* t(const wchar_t* english) const { return shown ? lang::tr(english) : english; }
    std::wstring f(const wchar_t* english, std::initializer_list<std::pair<const wchar_t*, std::wstring>> args) const {
        return shown ? lang::trf(english, args) : lang::fill(english, args);
    }
};

struct Spec {
    MenuItem item;
    const wchar_t* label;
    int min, max, step;
};

// A slider of the shared table's key (qvfr_settings.h), so the menu's ranges and steps are the launcher's and the layer's.
constexpr Spec spec_of(MenuItem item, const wchar_t* label, const IniInt& k) { return {item, label, k.min, k.max, k.step}; }
static_assert(kCropTop.max == static_cast<int>(kCropMax * 100), "the table's crop sides are fov_crop.h's kCropMax");

constexpr Spec kSpecs[] = {
    spec_of(MenuItem::Periphery, I18N(L"Periphery resolution"), kPeripheryScale),
    spec_of(MenuItem::Density, I18N(L"Focus density"), kFocusDensity),
    spec_of(MenuItem::ObjectDetail, I18N(L"Object detail"), kObjectDetail),
    spec_of(MenuItem::Sharpening, I18N(L"Sharpening"), kSharpenPercent),
    spec_of(MenuItem::EyeOff, I18N(L"Eye off"), kEyeOff),  // [quad] eye_off: none, left, right
    // Steps of kFocusQuadDistances.
    {MenuItem::FocusQuadDistance, I18N(L"Focus quad distance"), 0, kFocusQuadSteps - 1, 1},
    spec_of(MenuItem::Width, I18N(L"Focus width"), kFocusPercent),
    spec_of(MenuItem::Height, I18N(L"Focus height"), kFocusHeightPercent),
    spec_of(MenuItem::Feather, I18N(L"Feather"), kFeatherPercent),
    spec_of(MenuItem::Corners, I18N(L"Corner radius"), kCornerRadiusPercent),
    spec_of(MenuItem::Outline, I18N(L"Show focus outline"), kDebugBorder),
    spec_of(MenuItem::EyeTracking, I18N(L"Foveated mode"), kGaze),
    {MenuItem::FocusOffset, I18N(L"Gaze offset"), -kFocusOffsetMax, kFocusOffsetMax, 1},
    {MenuItem::GazeDeadzone, I18N(L"Gaze dead-zone"), 0, kGazeDeadzoneMax, 5},  // tenths of a degree, 0.5 a step
    // The Foveation section's Custom corners: each corner's insets, percent of the box (focus_shape.h's kFocusCornerMax).
    {MenuItem::Shape, I18N(L"Shape"), 0, 1, 1},
    {MenuItem::ShapeTopLeftIn, I18N(L"Top left in"), 0, kFocusCornerMax, 5},
    {MenuItem::ShapeTopLeftDown, I18N(L"Top left down"), 0, kFocusCornerMax, 5},
    {MenuItem::ShapeTopRightIn, I18N(L"Top right in"), 0, kFocusCornerMax, 5},
    {MenuItem::ShapeTopRightDown, I18N(L"Top right down"), 0, kFocusCornerMax, 5},
    {MenuItem::ShapeBottomLeftIn, I18N(L"Bottom left in"), 0, kFocusCornerMax, 5},
    {MenuItem::ShapeBottomLeftUp, I18N(L"Bottom left up"), 0, kFocusCornerMax, 5},
    {MenuItem::ShapeBottomRightIn, I18N(L"Bottom right in"), 0, kFocusCornerMax, 5},
    {MenuItem::ShapeBottomRightUp, I18N(L"Bottom right up"), 0, kFocusCornerMax, 5},
    // The performance overlay's place, degrees (the launcher's sliders).
    {MenuItem::Overlay, I18N(L"Show the overlay"), 0, 1, 1},
    {MenuItem::OverlayYaw, I18N(L"Horizontal"), -kOsdYawMax, kOsdYawMax, 1},
    {MenuItem::OverlayPitch, I18N(L"Vertical"), -kOsdPitchMax, kOsdPitchMax, 1},
    {MenuItem::OverlayWidth, I18N(L"Size"), kOsdWidthMin, kOsdWidthMax, 1},
    // The clock's place, degrees (its size: the launcher's slider).
    {MenuItem::Clock, I18N(L"Show the clock"), 0, 1, 1},
    {MenuItem::ClockYaw, I18N(L"Horizontal"), -kClockYawMax, kClockYawMax, 1},
    {MenuItem::ClockPitch, I18N(L"Vertical"), -kClockPitchMax, kClockPitchMax, 1},
    {MenuItem::ClockWidth, I18N(L"Size"), kClockWidthMin, kClockWidthMax, 1},
    // The panel's place, degrees (the launcher's kMenuYawRange, kMenuPitchRange, kMenuWidthRange).
    {MenuItem::MenuYaw, I18N(L"Horizontal"), -kMenuYawMax, kMenuYawMax, 1},
    {MenuItem::MenuPitch, I18N(L"Vertical"), -kMenuPitchMax, kMenuPitchMax, 1},
    {MenuItem::MenuWidth, I18N(L"Size"), kMenuWidthMin, kMenuWidthMax, 1},
    // The page switch and the Foveated DLSS page (cheeky_settings.h's ranges; presets step through its list).
    {MenuItem::Page, I18N(L"Page"), 0, 1, 1},
    {MenuItem::CheekyProcessing, I18N(L"Foveated DLSS"), 0, 1, 1},
    {MenuItem::CheekyOuterDlaa, I18N(L"Cheaper outer views"), 0, 1, 1},
    {MenuItem::CheekyOuterScale, I18N(L"Outer DLAA scale"), kCheekyScaleMin, kCheekyScaleMax, kCheekyScaleStep},
    {MenuItem::CheekyOuterPreset, I18N(L"Outer preset"), 0, 0, 1},
    {MenuItem::CheekyFocusPreset, I18N(L"Focus preset"), 0, 0, 1},
    {MenuItem::CheekySupersampling, I18N(L"Focus supersampling"), kCheekySupersamplingMin, kCheekySupersamplingMax,
     kCheekySupersamplingStep},
    {MenuItem::CheekyMotionFix, I18N(L"Fix motion-vector blur"), 0, 1, 1},
    {MenuItem::CheekyNr, L"DLSS-NR", 0, 1, 1},
    {MenuItem::CheekyNrPreset, I18N(L"DLSS-NR preset"), 0, kCheekyNrPresetMax, 1},
    {MenuItem::CheekyNrPasses, I18N(L"DLSS-NR passes"), 1, kCheekyNrPassesMax, 1},
    {MenuItem::FgOn, I18N(L"Frame generation"), 0, 1, 1},
    {MenuItem::FgMultiplier, I18N(L"Multiplier"), 2, kCheekyFgMultiplierMax, 1},
    {MenuItem::FgSize, I18N(L"Center region"), kCheekyFgSizeMin, kCheekyFgSizeMax, kCheekyFgSizeStep},
    {MenuItem::FgScale, I18N(L"Center FG scale"), 25, 100, 25},
    {MenuItem::FgPeriphery, I18N(L"Periphery FG scale"), 25, 100, 25},
    {MenuItem::FgRelease, I18N(L"Release game"), 0, 2, 1},
    {MenuItem::FgBorder, I18N(L"Show the center region"), 0, 1, 1},
    // The View & helmet section's crop: shares of each eye's view removed at each side (fov_crop.h's kCropMax).
    spec_of(MenuItem::Crop, I18N(L"Crop the view"), kCropEnabled),
    spec_of(MenuItem::CropTop, I18N(L"Top"), kCropTop),
    spec_of(MenuItem::CropBottom, I18N(L"Bottom"), kCropBottom),
    spec_of(MenuItem::CropOuter, I18N(L"Outer side (temple)"), kCropOuter),
    spec_of(MenuItem::CropInner, I18N(L"Inner side (nose)"), kCropInner),
    {MenuItem::Helmet, I18N(L"Show a helmet"), 0, 1, 1},
    {MenuItem::HelmetRim, I18N(L"Rim"), 0, kHelmetRimMax, 1},
    {MenuItem::HelmetCorners, I18N(L"Corners"), 0, kHelmetCornersMax, 10},
    {MenuItem::HelmetLight, I18N(L"Lighting"), 0, kHelmetLightMax, kHelmetLightStep},
    {MenuItem::HelmetDebug, I18N(L"Show helmet light"), 0, 1, 1},
    {MenuItem::Visor, I18N(L"Visor"), 0, 2, 1},
    {MenuItem::HelmetSize, I18N(L"Size"), kHelmetSizeMin, kHelmetSizeMax, 5},
    {MenuItem::HelmetHeight, I18N(L"Height"), -kHelmetHeightMax, kHelmetHeightMax, 1},
    {MenuItem::HelmetDepth, I18N(L"Depth"), 0, kHelmetDepthMax, 1},
    {MenuItem::HelmetDetail, I18N(L"Detail"), 0, 1, 1},
    {MenuItem::Optimize, I18N(L"Auto-fit to headset rate"), 0, 0, 1},
    {MenuItem::Profile, I18N(L"Profile"), 0, 0, 1},
    {MenuItem::HotkeyFooter, I18N(L"Show the hotkeys under the menu"), 0, 1, 1},
    spec_of(MenuItem::CpuIsolateMain, I18N(L"Give the main thread a core of its own"), kCpuIsolateMain),
    // The DLSS page (Sim-QVFR's own; engine/dlss_settings.h's choices).
    {MenuItem::DlssOn, I18N(L"Use DLSS"), 0, 1, 1},
    {MenuItem::DlssUpscaler, I18N(L"Upscaler"), 0, 1, 1},
    {MenuItem::DlssFocusMode, I18N(L"Focus views"), 0, 0, 1},
    {MenuItem::DlssFocusPreset, I18N(L"Focus preset"), 0, 0, 1},
    {MenuItem::DlssEyePreset, I18N(L"Periphery preset"), 0, 0, 1},
    {MenuItem::DlssSharpening, I18N(L"Texture sharpening"), 0, 0, 1},
    // The DLSS-NR page (engine/nr_settings.h's ranges).
    {MenuItem::NrOn, I18N(L"Use DLSS-NR"), 0, 1, 1},
    {MenuItem::NrIntensity, I18N(L"Intensity"), 0, 100, 5},
    {MenuItem::NrStyle, I18N(L"Style"), 0, 2, 1},
    {MenuItem::NrPreset, I18N(L"Preset"), 0, 2, 1},
    {MenuItem::NrLocalTone, I18N(L"Local tone"), 0, 200, 10},
    {MenuItem::NrLocalStructure, I18N(L"Local structure"), 0, 200, 10},
    {MenuItem::NrAutoMask, I18N(L"Auto mask"), 0, 1, 1},
    {MenuItem::NrOrder, I18N(L"Runs"), 0, 1, 1},
    {MenuItem::NrFocusScale, I18N(L"Focus working size"), kNrScaleMin, 100, 5},
    {MenuItem::NrEdgeBlend, I18N(L"Edge blend"), 0, kNrEdgeMax, 2},
    {MenuItem::NrPeriphery, I18N(L"Periphery too"), 0, 1, 1},
    {MenuItem::NrPeripheryScale, I18N(L"Periphery working size"), kNrScaleMin, 100, 5},
    {MenuItem::NrPeripheryCoverage, I18N(L"Periphery covered"), kNrCoverageMin, 100, 10},
    {MenuItem::NrLimit, I18N(L"Working size limit"), kNrLimitMin, kNrLimitMax, 1},  // tenths of a megapixel a view
    // The sections: the launcher's card names (HeadDlss: "FSR" while FSR runs the views, menu_lines).
    {MenuItem::HeadResolution, I18N(L"Resolution"), 0, 0, 0},
    {MenuItem::HeadFoveation, I18N(L"Foveation"), 0, 0, 0},
    {MenuItem::HeadView, I18N(L"View & helmet"), 0, 0, 0},
    {MenuItem::HeadDlss, L"DLSS", 0, 0, 0},
    {MenuItem::HeadNr, L"DLSS-NR", 0, 0, 0},
    {MenuItem::HeadCheeky, I18N(L"Foveated DLSS"), 0, 0, 0},
    {MenuItem::HeadFg, I18N(L"Frame generation"), 0, 0, 0},
    {MenuItem::HeadMenu, I18N(L"Headset menu"), 0, 0, 0},
    {MenuItem::HeadOverlay, I18N(L"Performance overlay"), 0, 0, 0},
    {MenuItem::HeadClock, I18N(L"Clock"), 0, 0, 0},
    {MenuItem::HeadVision, I18N(L"Vision"), 0, 0, 0},
    {MenuItem::HeadCpu, L"CPU", 0, 0, 0},
};

// AMD FSR runs the DLSS page's views: a PC without an NVIDIA RTX card, or the experiment's Upscaler on FSR.
bool fsr_runs(const SettingsMenu& menu) {
    return menu.launch.fsrPc || (menu.launch.fsr && menu.values.dlss.upscaler == 1);
}

const Spec& spec(MenuItem item) {
    for (const Spec& s : kSpecs)
        if (s.item == item)
            return s;
    return kSpecs[0];
}

// The DLSS-NR page's values: a number of NrSettings', or (`flag`) one of its switches. Neither: not its item.
int* nr_number(NrSettings& n, MenuItem item) {
    switch (item) {
    case MenuItem::NrIntensity: return &n.intensity;
    case MenuItem::NrStyle: return &n.style;
    case MenuItem::NrPreset: return &n.preset;
    case MenuItem::NrLocalTone: return &n.localTone;
    case MenuItem::NrLocalStructure: return &n.localStructure;
    case MenuItem::NrFocusScale: return &n.focusScale;
    case MenuItem::NrEdgeBlend: return &n.edgeBlend;
    case MenuItem::NrPeripheryScale: return &n.peripheryScale;
    case MenuItem::NrPeripheryCoverage: return &n.peripheryCoverage;
    case MenuItem::NrLimit: return &n.workingLimit;
    default: return nullptr;
    }
}
bool* nr_flag(NrSettings& n, MenuItem item) {
    switch (item) {
    case MenuItem::NrOn: return &n.on;
    case MenuItem::NrAutoMask: return &n.autoMask;
    case MenuItem::NrOrder: return &n.beforeDlss;
    case MenuItem::NrPeriphery: return &n.periphery;
    default: return nullptr;
    }
}

int* value_of(MenuValues& v, MenuItem item) {
    switch (item) {
    case MenuItem::Periphery: return &v.periphery;
    case MenuItem::Density: return &v.density;
    case MenuItem::ObjectDetail: return &v.objectDetail;
    case MenuItem::Sharpening: return &v.sharpen;
    case MenuItem::EyeOff: return &v.eyeOff;
    case MenuItem::FocusQuadDistance: return &v.focusQuadStep;
    case MenuItem::Width: return &v.width;
    case MenuItem::Height: return &v.height;
    case MenuItem::Feather: return &v.feather;
    case MenuItem::Corners: return &v.corners;
    case MenuItem::FocusOffset: return &v.offset.for_mode(v.eyeTracking);
    case MenuItem::GazeDeadzone: return &v.gazeDeadzone;
    case MenuItem::ShapeTopLeftIn: return &v.shape.in[static_cast<int>(FocusCorner::TopLeft)];
    case MenuItem::ShapeTopLeftDown: return &v.shape.vertical[static_cast<int>(FocusCorner::TopLeft)];
    case MenuItem::ShapeTopRightIn: return &v.shape.in[static_cast<int>(FocusCorner::TopRight)];
    case MenuItem::ShapeTopRightDown: return &v.shape.vertical[static_cast<int>(FocusCorner::TopRight)];
    case MenuItem::ShapeBottomLeftIn: return &v.shape.in[static_cast<int>(FocusCorner::BottomLeft)];
    case MenuItem::ShapeBottomLeftUp: return &v.shape.vertical[static_cast<int>(FocusCorner::BottomLeft)];
    case MenuItem::ShapeBottomRightIn: return &v.shape.in[static_cast<int>(FocusCorner::BottomRight)];
    case MenuItem::ShapeBottomRightUp: return &v.shape.vertical[static_cast<int>(FocusCorner::BottomRight)];
    case MenuItem::MenuYaw: return &v.placement.yaw;
    case MenuItem::MenuPitch: return &v.placement.pitch;
    case MenuItem::MenuWidth: return &v.placement.width;
    case MenuItem::OverlayYaw: return &v.overlay.yaw;
    case MenuItem::OverlayPitch: return &v.overlay.pitch;
    case MenuItem::OverlayWidth: return &v.overlay.width;
    case MenuItem::ClockYaw: return &v.clock.yaw;
    case MenuItem::ClockPitch: return &v.clock.pitch;
    case MenuItem::ClockWidth: return &v.clock.width;
    case MenuItem::CropTop: return &v.cropTop;
    case MenuItem::CropBottom: return &v.cropBottom;
    case MenuItem::CropOuter: return &v.cropOuter;
    case MenuItem::CropInner: return &v.cropInner;
    case MenuItem::HelmetRim: return &v.helmet.rim;
    case MenuItem::HelmetCorners: return &v.helmet.corners;
    case MenuItem::HelmetLight: return &v.helmet.light;
    case MenuItem::HelmetSize: return &v.helmet.size;
    case MenuItem::HelmetHeight: return &v.helmet.height;
    default: return nullptr;
    }
}

// Only a value that differs is written: a step changes one key of the ~50 written (ini_put.h).
bool write(const wchar_t* ini, const wchar_t* section, const wchar_t* key, int value) {
    return ini_put(ini, section, key, value);
}

bool write(const wchar_t* ini, const IniInt& k, int value) { return ini_put(ini, k.section, k.key, value); }

}  // namespace

std::vector<MenuItem> menu_items(bool qvfr) {
    std::vector<MenuItem> items;
    if (qvfr)
        items = {MenuItem::Periphery, MenuItem::Density};
    items.insert(items.end(), {MenuItem::Width, MenuItem::Height, MenuItem::Feather, MenuItem::Corners,
                               MenuItem::Outline, MenuItem::EyeTracking, MenuItem::FocusOffset, MenuItem::GazeDeadzone});
    return items;
}

namespace {

// The View page: the launcher's View & helmet card, as the game has it.
bool view_section(const SettingsMenu& menu) { return menu.launch.qvfr && (menu.launch.cropPage || menu.launch.helmetPage); }

}  // namespace

std::vector<MenuPage> menu_pages(const SettingsMenu& menu) {
    std::vector<MenuPage> pages = {MenuPage::Picture, MenuPage::Foveation};
    if (view_section(menu))
        pages.push_back(MenuPage::View);
    pages.push_back(MenuPage::Headset);
    pages.push_back(MenuPage::Game);  // every game and method: the CPU card
    return pages;
}

MenuPage menu_page(const SettingsMenu& menu) {
    const std::vector<MenuPage> pages = menu_pages(menu);
    return pages[std::clamp(menu.page, 0, static_cast<int>(pages.size()) - 1)];
}

std::vector<MenuItem> menu_page_items(const SettingsMenu& menu) {
    std::vector<MenuItem> items = {MenuItem::Page};
    const auto add = [&items](std::initializer_list<MenuItem> more) { items.insert(items.end(), more); };
    const MenuLaunch& l = menu.launch;
    const MenuValues& v = menu.values;
    const MenuPage page = menu_page(menu);
    if (page == MenuPage::Headset) {  // the launcher's Headset tab: its cards in its order
        add({MenuItem::HeadMenu, MenuItem::MenuYaw, MenuItem::MenuPitch, MenuItem::MenuWidth, MenuItem::HotkeyFooter,
             MenuItem::HeadOverlay, MenuItem::Overlay});
        if (v.overlay.enabled)  // its place while it shows, as the launcher reveals it
            add({MenuItem::OverlayYaw, MenuItem::OverlayPitch, MenuItem::OverlayWidth});
        add({MenuItem::HeadClock, MenuItem::Clock});
        if (v.clock.enabled)
            add({MenuItem::ClockYaw, MenuItem::ClockPitch, MenuItem::ClockWidth});
    } else if (page == MenuPage::Game) {
        add({MenuItem::HeadCpu, MenuItem::CpuIsolateMain});
    } else if (page == MenuPage::View) {  // the launcher's View & helmet card
        items.push_back(MenuItem::HeadView);
        if (l.cropPage) {
            items.push_back(MenuItem::Crop);
            if (v.crop)
                add({MenuItem::CropTop, MenuItem::CropBottom, MenuItem::CropOuter, MenuItem::CropInner});
        }
        if (l.helmetPage) {
            items.push_back(MenuItem::Helmet);
            if (v.helmet.on && l.helmetPieces)
                add({MenuItem::Visor, MenuItem::HelmetSize, MenuItem::HelmetHeight, MenuItem::HelmetDepth,
                     MenuItem::HelmetDetail, MenuItem::HelmetLight, MenuItem::HelmetDebug});
            else if (v.helmet.on)
                add({MenuItem::HelmetRim, MenuItem::HelmetCorners, MenuItem::HelmetLight, MenuItem::HelmetDebug});
        }
    } else if (page == MenuPage::Foveation) {
        // The launcher's Foveation card in its order: mode, size, shape (its corner radius, or each corner's insets),
        // feather, the focus's vertical place, the gaze dead-zone (eye tracked), the outline.
        add({MenuItem::HeadFoveation, MenuItem::EyeTracking, MenuItem::Width, MenuItem::Height});
        if (l.shapePage)
            items.push_back(MenuItem::Shape);
        if (l.shapePage && v.shape.corners)
            add({MenuItem::ShapeTopLeftIn, MenuItem::ShapeTopLeftDown, MenuItem::ShapeTopRightIn, MenuItem::ShapeTopRightDown,
                 MenuItem::ShapeBottomLeftIn, MenuItem::ShapeBottomLeftUp, MenuItem::ShapeBottomRightIn,
                 MenuItem::ShapeBottomRightUp});
        else
            items.push_back(MenuItem::Corners);
        add({MenuItem::Feather, MenuItem::FocusOffset});
        if (v.eyeTracking)
            items.push_back(MenuItem::GazeDeadzone);
        if (l.qvfr && l.focusQuads)  // focus_layer=2's test
            items.push_back(MenuItem::FocusQuadDistance);
        items.push_back(MenuItem::Outline);
    } else {  // Picture: the profile, the Resolution card, the game's upscaler cards, Vision
        if (!menu.profiles.empty())
            items.push_back(MenuItem::Profile);
        if (l.qvfr) {
            add({MenuItem::HeadResolution, MenuItem::Periphery, MenuItem::Density});
            if (l.objectDetail)  // only a game with it (X-Plane)
                items.push_back(MenuItem::ObjectDetail);
            if (l.sharpening)
                items.push_back(MenuItem::Sharpening);
            if (l.optimize)
                items.push_back(MenuItem::Optimize);
        }
        if (l.apiDlss)  // the API path's DLSS: the focus views' mode and preset, live (the periphery: F, fixed)
            add({MenuItem::HeadDlss, MenuItem::DlssFocusMode, MenuItem::DlssFocusPreset});
        if (l.qvfr && l.ownDlss) {
            add({MenuItem::HeadDlss, MenuItem::DlssOn});
            // AMD FSR has no NGX presets: their rows go while it runs the views. The Upscaler row is the RTX test's only.
            const bool fsr = fsr_runs(menu);
            if (v.dlss.on && l.fsr && !l.fsrPc)
                items.push_back(MenuItem::DlssUpscaler);
            if (v.dlss.on && fsr)
                add({MenuItem::DlssFocusMode, MenuItem::DlssSharpening});
            else if (v.dlss.on)
                add({MenuItem::DlssFocusMode, MenuItem::DlssFocusPreset, MenuItem::DlssEyePreset, MenuItem::DlssSharpening});
            if (l.ownNr) {
                add({MenuItem::HeadNr, MenuItem::NrOn});
                if (v.nr.on) {
                    add({MenuItem::NrIntensity, MenuItem::NrStyle, MenuItem::NrPreset, MenuItem::NrLocalTone,
                         MenuItem::NrLocalStructure, MenuItem::NrAutoMask, MenuItem::NrOrder, MenuItem::NrFocusScale,
                         MenuItem::NrLimit, MenuItem::NrEdgeBlend, MenuItem::NrPeriphery});
                    if (v.nr.periphery)
                        add({MenuItem::NrPeripheryScale, MenuItem::NrPeripheryCoverage});
                }
            }
        }
        if (menu.cheeky.available) {
            add({MenuItem::HeadCheeky, MenuItem::CheekyProcessing, MenuItem::CheekyOuterDlaa, MenuItem::CheekyOuterScale,
                 MenuItem::CheekyOuterPreset, MenuItem::CheekyFocusPreset, MenuItem::CheekySupersampling,
                 MenuItem::CheekyMotionFix, MenuItem::CheekyNr, MenuItem::CheekyNrPreset, MenuItem::CheekyNrPasses});
            if (l.fgPage)
                add({MenuItem::HeadFg, MenuItem::FgOn, MenuItem::FgMultiplier, MenuItem::FgSize, MenuItem::FgScale,
                     MenuItem::FgPeriphery, MenuItem::FgRelease, MenuItem::FgBorder});
        }
        if (l.eyeOff)  // a game with it (LMU): the Vision card
            add({MenuItem::HeadVision, MenuItem::EyeOff});
    }
    return items;
}

MenuItem menu_section(const SettingsMenu& menu) {
    const std::vector<MenuItem> items = menu_page_items(menu);
    const int at = std::clamp(menu.selected, 0, static_cast<int>(items.size()) - 1);
    for (int i = at; i >= 0; --i)
        if (menu_item_is_header(items[i]))
            return items[i];
    for (const MenuItem item : items)  // the page switch, the profile: the page's first section
        if (menu_item_is_header(item))
            return item;
    return MenuItem::HeadFoveation;
}

namespace {

std::wstring page_name(const SettingsMenu&, MenuPage page, const Words& w) {
    switch (page) {
    case MenuPage::Foveation: return w.t(I18N(L"Foveation"));
    case MenuPage::View: return w.t(I18N(L"View"));
    case MenuPage::Headset: return w.t(I18N(L"Headset"));
    case MenuPage::Game: return w.t(I18N(L"Game"));
    default: return w.t(I18N(L"Picture"));
    }
}

}  // namespace

std::vector<std::wstring> menu_tabs(const SettingsMenu& menu) {
    std::vector<std::wstring> tabs;
    for (const MenuPage p : menu_pages(menu))
        tabs.push_back(page_name(menu, p, Words{}));
    return tabs;
}

std::wstring menu_title(const SettingsMenu& menu) {
    // The launcher's name for the panel; the method where it isn't plain QVFR. The page: the tabs under it.
    return lang::tr(menu.launch.apiPath ? I18N(L"Headset menu \u00b7 QVFR (API path)")
                    : menu.launch.qvfr  ? I18N(L"Headset menu")
                                        : I18N(L"Headset menu \u00b7 Two-loop (legacy)"));
}

std::wstring menu_page_name(const SettingsMenu& menu, bool shown) {
    return page_name(menu, menu_page(menu), Words{shown});
}

std::wstring menu_subtitle(const SettingsMenu& menu) {
    if (!menu.notice.empty())
        return menu.notice;
    const MenuPage page = menu_page(menu);
    const MenuItem section = menu_section(menu);
    // The optimizer's detail on the Resolution card (it sets the focus size too, but it is started there).
    if (section == MenuItem::HeadResolution && !menu.optimizeDetail.empty())
        return menu.optimizeDetail;
    // The runtime composing the views (Pimax, Varjo): the focus area shows inside the runtime's own focus region.
    if (page == MenuPage::Foveation && menu.launch.runtimeComposes)
        return lang::tr(L"The runtime composes the views: the focus area shows inside its own focus region");
    if (page == MenuPage::Foveation || section == MenuItem::HeadResolution)
        return L"";
    if (section == MenuItem::HeadDlss && menu.launch.ownDlss && fsr_runs(menu))
        return lang::tr(L"AMD FSR, run by Sim-QVFR (experimental): changes apply at once");
    if (section == MenuItem::HeadDlss && menu.launch.ownDlss)
        return lang::tr(L"NVIDIA DLSS, run by Sim-QVFR through NVIDIA's NGX: changes apply at once");
    if (section == MenuItem::HeadNr)
        return lang::tr(L"NVIDIA DLSS neural rendering, your own nvngx_dlssnr.dll (experimental)");
    return section == MenuItem::HeadCheeky || section == MenuItem::HeadFg ? cheeky_version_label(menu.cheeky.version) : L"";
}

bool menu_key(SettingsMenu& menu, MenuKey key) {
    const int pages = static_cast<int>(menu_pages(menu).size());
    menu.page = std::clamp(menu.page, 0, pages - 1);
    const std::vector<MenuItem> items = menu_page_items(menu);
    const int n = static_cast<int>(items.size());
    menu.selected = std::clamp(menu.selected, 0, n - 1);
    if (key == MenuKey::Up || key == MenuKey::Down) {  // the next line that isn't a section's heading (wrapping)
        for (int tries = 0; tries < n; ++tries) {
            menu.selected = (menu.selected + (key == MenuKey::Down ? 1 : n - 1)) % n;
            if (!menu_item_is_header(items[menu.selected]))
                break;
        }
        return false;
    }
    const MenuItem item = items[menu.selected];
    if (item == MenuItem::Optimize) {  // the layer runs it; no value of the menu's changes
        menu.optimizeRequest = key == MenuKey::Right ? 1 : -1;
        return false;
    }
    if (item == MenuItem::Profile) {  // the next or previous profile; the layer uses it (its values follow from the ini)
        const int count = static_cast<int>(menu.profiles.size());
        menu.profile = menu.profile < 0 ? (key == MenuKey::Right ? 0 : count - 1)
                                        : (menu.profile + (key == MenuKey::Right ? 1 : count - 1)) % count;
        menu.profileChanged = false;
        menu.profileRequest = menu.profile;
        return false;
    }
    if (item == MenuItem::Page) {  // the next or previous page, with the switch still selected
        menu.page = (menu.page + (key == MenuKey::Right ? 1 : pages - 1)) % pages;
        menu.selected = 0;
        menu.scroll = 0;
        return false;
    }
    if (menu_item_is_header(item))  // never selected by the keys; a stale index lands here at most
        return false;
    CheekySettings& c = menu.cheeky.values;
    const int step = key == MenuKey::Right ? 1 : -1;
    DlssSettings& dl = menu.values.dlss;
    if (bool* flag = nr_flag(menu.values.nr, item)) {
        *flag = !*flag;
        return true;
    }
    if (int* number = nr_number(menu.values.nr, item)) {  // the ends hold
        const Spec& range = spec(item);
        const int before = *number;
        *number = std::clamp(*number + step * range.step, range.min, range.max);
        return *number != before;
    }
    switch (item) {
    case MenuItem::DlssOn: dl.on = !dl.on; return true;
    case MenuItem::DlssUpscaler: dl.upscaler = dl.upscaler == 1 ? 0 : 1; return true;
    case MenuItem::DlssFocusMode: {  // DLAA, then each smaller render (no wrap)
        const int before = dl.focusScale;
        dl.focusScale = dlss_choice_step(kDlssFocusModes, dl.focusScale, step);
        return dl.focusScale != before;
    }
    case MenuItem::DlssFocusPreset: {
        const int before = dl.focusPreset;
        dl.focusPreset = dlss_choice_step(kDlssFocusPresets, dl.focusPreset, step);
        return dl.focusPreset != before;
    }
    case MenuItem::DlssEyePreset: {
        const int before = dl.eyePreset;
        dl.eyePreset = dlss_choice_step(kDlssEyePresets, dl.eyePreset, step);
        return dl.eyePreset != before;
    }
    case MenuItem::DlssSharpening: {  // off, the focus views, the eyes too (no wrap)
        const int before = dl.textureSharpening;
        dl.textureSharpening = dlss_choice_step(kDlssTextureSharpening, dl.textureSharpening, step);
        return dl.textureSharpening != before;
    }
    case MenuItem::CheekyProcessing: c.processing = !c.processing; return true;
    case MenuItem::CheekyOuterDlaa: c.outerDlaa = !c.outerDlaa; return true;
    case MenuItem::CheekyOuterScale: {
        const int before = c.outerScale;
        c.outerScale = std::clamp(c.outerScale + step * kCheekyScaleStep, kCheekyScaleMin, kCheekyScaleMax);
        return c.outerScale != before;
    }
    case MenuItem::CheekyOuterPreset: c.outerPreset = cheeky_preset_step(c.outerPreset, step, false); return true;
    case MenuItem::CheekyFocusPreset: c.focusPreset = cheeky_preset_step(c.focusPreset, step, true); return true;
    case MenuItem::CheekySupersampling: {
        const int before = c.supersampling;
        c.supersampling = std::clamp(c.supersampling + step * kCheekySupersamplingStep, kCheekySupersamplingMin,
                                     kCheekySupersamplingMax);
        return c.supersampling != before;
    }
    case MenuItem::CheekyMotionFix: c.motionFix = !c.motionFix; return true;
    case MenuItem::CheekyNr: c.nr = !c.nr; return true;
    case MenuItem::CheekyNrPreset: c.nrPreset = ((c.nrPreset + step) % (kCheekyNrPresetMax + 1) + kCheekyNrPresetMax + 1) %
                                                (kCheekyNrPresetMax + 1);
        return true;
    case MenuItem::CheekyNrPasses: {
        const int before = c.nrPasses;
        c.nrPasses = std::clamp(c.nrPasses + step, 1, kCheekyNrPassesMax);
        return c.nrPasses != before;
    }
    case MenuItem::HelmetDepth: {  // far (0), then 3 cm on: 1 cm a step up close, 5 cm from 20 cm
        int& d = menu.values.helmet.depth;
        const int before = d;
        if (step > 0)
            d = d == 0 ? kHelmetDepthMin : std::min(kHelmetDepthMax, d + (d >= 20 ? 5 : 1));
        else
            d = d <= kHelmetDepthMin ? 0 : d - (d > 20 ? 5 : 1);
        return d != before;
    }
    case MenuItem::Visor: {  // closed, half, open (no wrap)
        const int before = static_cast<int>(menu.values.helmet.visor);
        menu.values.helmet.visor = static_cast<VisorPosition>(std::clamp(before + step, 0, 2));
        return static_cast<int>(menu.values.helmet.visor) != before;
    }
    case MenuItem::FgRelease: {  // never, capture, generated (no wrap)
        const int before = cheeky_fg_release_index(c.fgRelease);
        c.fgRelease = kCheekyFgReleasePoints[std::clamp(before + step, 0, 2)];
        return cheeky_fg_release_index(c.fgRelease) != before;
    }
    case MenuItem::FgOn: c.fg = !c.fg; return true;
    case MenuItem::FgBorder: c.fgBorder = !c.fgBorder; return true;
    case MenuItem::FgMultiplier:
    case MenuItem::FgSize:
    case MenuItem::FgScale:
    case MenuItem::FgPeriphery: {
        const Spec& sp = spec(item);
        int& v = item == MenuItem::FgMultiplier ? c.fgMultiplier
                 : item == MenuItem::FgSize     ? c.fgSize
                 : item == MenuItem::FgScale    ? c.fgScale
                                                : c.fgPeriphery;
        const int before = v;
        v = std::clamp(v + step * sp.step, sp.min, sp.max);
        return v != before;
    }
    default: break;
    }
    if (item == MenuItem::Outline || item == MenuItem::EyeTracking || item == MenuItem::Overlay || item == MenuItem::Clock ||
        item == MenuItem::Crop ||
        item == MenuItem::Helmet || item == MenuItem::HelmetDebug || item == MenuItem::HelmetDetail ||
        item == MenuItem::Shape || item == MenuItem::HotkeyFooter || item == MenuItem::CpuIsolateMain) {
        bool& flag = item == MenuItem::Outline       ? menu.values.outline
                     : item == MenuItem::HotkeyFooter ? menu.values.hotkeyFooter
                     : item == MenuItem::CpuIsolateMain ? menu.values.cpuIsolateMain
                     : item == MenuItem::Shape       ? menu.values.shape.corners
                     : item == MenuItem::EyeTracking ? menu.values.eyeTracking
                     : item == MenuItem::Crop        ? menu.values.crop
                     : item == MenuItem::Helmet      ? menu.values.helmet.on
                     : item == MenuItem::HelmetDebug ? menu.values.helmet.debugLight
                     : item == MenuItem::HelmetDetail ? menu.values.helmet.detail
                     : item == MenuItem::Clock       ? menu.values.clock.enabled
                                                     : menu.values.overlay.enabled;
        flag = !flag;
        return true;
    }
    const Spec& s = spec(item);
    int* v = value_of(menu.values, item);
    const int before = *v;
    *v = std::clamp(*v + (key == MenuKey::Right ? s.step : -s.step), s.min, s.max);
    return *v != before;
}

MenuValues read_menu_values(const wchar_t* ini) {
    MenuValues v;
    v.width = ini_int(ini, kFocusPercent);
    v.height = ini_int(ini, kFocusHeightPercent, v.width);
    const int eyeScale = ini_raw(ini, kEyeScale, kEyeScale.def);
    const int periphery = ini_raw(ini, kPeripheryScale, 0);
    v.periphery = kPeripheryScale.clamp(periphery ? periphery : eyeScale);
    const int density = ini_raw(ini, kFocusDensity, 0);
    v.density = kFocusDensity.clamp(density ? density : static_cast<int>(std::lround(100.0 * kEyeScale.clamp(eyeScale) / v.width)));
    v.feather = ini_int(ini, kFeatherPercent);
    v.corners = ini_int(ini, kCornerRadiusPercent);
    v.outline = ini_flag(ini, kDebugBorder);
    v.eyeTracking = ini_flag(ini, kGaze);
    v.offset = read_focus_offset(ini);
    v.gazeDeadzone = read_gaze_deadzone(ini);
    v.shape = read_focus_shape(ini);
    v.focusMask = ini_flag(ini, kFocusMask);
    v.focusMaskMargin = ini_int(ini, kFocusMaskMargin);
    v.overlay = read_osd_placement(ini);
    v.clock = read_clock_placement(ini);
    v.placement = read_menu_placement(ini);
    v.hotkeyFooter = ini_flag(ini, kHotkeyFooter);
    v.cpuIsolateMain = ini_flag(ini, kCpuIsolateMain);
    v.crop = ini_flag(ini, kCropEnabled);
    v.cropTop = ini_int(ini, kCropTop);
    v.cropBottom = ini_int(ini, kCropBottom);
    v.cropOuter = ini_int(ini, kCropOuter);
    v.cropInner = ini_int(ini, kCropInner);
    v.helmet = read_helmet_settings(ini);
    v.objectDetail = ini_int(ini, kObjectDetail);
    v.sharpen = ini_int(ini, kSharpenPercent);
    v.eyeOff = ini_int(ini, kEyeOff);
    v.focusQuadStep = focus_quad_step(read_focus_quad_distance(ini));
    v.dlss = read_dlss_settings(ini);
    v.nr = read_nr_settings(ini);
    return v;
}

bool write_menu_values(const wchar_t* ini, const MenuValues& v, bool qvfr, bool objectDetail, bool focusQuads,
                       bool sharpening, bool eyeOff) {
    bool ok = true;
    if (eyeOff)  // QVFR or not: the layer shows the eye black either way
        ok &= write(ini, kEyeOff, kEyeOff.clamp(v.eyeOff));
    if (qvfr && sharpening)
        ok &= write(ini, kSharpenPercent, kSharpenPercent.clamp(v.sharpen));
    if (qvfr && focusQuads)
        ok &= write(ini, L"experiment", L"focus_quad_distance", focus_quad_metres(v.focusQuadStep));
    if (qvfr && objectDetail)
        ok &= write(ini, kObjectDetail, kObjectDetail.clamp(v.objectDetail));
    if (qvfr) {
        ok &= write(ini, kPeripheryScale, v.periphery);
        ok &= write(ini, kFocusDensity, v.density);
        ok &= write(ini, kCropEnabled, v.crop ? 1 : 0);
        ok &= write(ini, kCropTop, v.cropTop);
        ok &= write(ini, kCropBottom, v.cropBottom);
        ok &= write(ini, kCropOuter, v.cropOuter);
        ok &= write(ini, kCropInner, v.cropInner);
        ok &= write_helmet_settings(ini, v.helmet);
    }
    ok &= write(ini, kFocusPercent, v.width);
    ok &= write(ini, kFocusHeightPercent, v.height);
    ok &= write(ini, kFeatherPercent, v.feather);
    ok &= write(ini, kCornerRadiusPercent, v.corners);
    ok &= write(ini, kDebugBorder, v.outline ? 1 : 0);
    ok &= write(ini, kGaze, v.eyeTracking ? 1 : 0);
    ok &= write_focus_offset(ini, v.offset);
    ok &= write_gaze_deadzone(ini, v.gazeDeadzone);
    ok &= write_focus_shape(ini, v.shape);
    ok &= write_osd_placement(ini, v.overlay);
    ok &= write_clock_placement(ini, v.clock);
    ok &= write(ini, L"menu", L"yaw", v.placement.yaw);
    ok &= write(ini, L"menu", L"pitch", v.placement.pitch);
    ok &= write(ini, L"menu", L"width", v.placement.width);
    ok &= write(ini, kHotkeyFooter, v.hotkeyFooter ? 1 : 0);
    ok &= write(ini, kCpuIsolateMain, v.cpuIsolateMain ? 1 : 0);
    return ok;
}

// The launch layout from the launch sizes (percent of the native eye), as the layer has it.
Views4Layout launch_layout(const MenuLaunch& l) {
    return views4_layout(l.outer / 100, l.focusWidth / 100, l.focusHeight / 100, l.cropWidth / 100, l.cropHeight / 100);
}

MenuFit menu_fit(const SettingsMenu& menu) {
    const MenuValues& v = menu.values;
    MenuFit f;
    // The focus views' size against the native eye: density times the box (the layer's views4_sizes).
    f.wantFocusWidth = v.density * v.width / 100.0;
    f.wantFocusHeight = v.density * v.height / 100.0;
    f.periphery = v.periphery;
    f.focusWidth = f.wantFocusWidth;
    f.focusHeight = f.wantFocusHeight;
    f.densityX = f.densityY = v.density;
    if (!menu.launch.qvfr)
        return f;
    // The layer's own rule (views4_fit), in percent.
    const Views4Fit fit = views4_fit(launch_layout(menu.launch), v.periphery / 100.0, f.wantFocusWidth / 100,
                                     f.wantFocusHeight / 100, v.crop_width() / 100, v.crop_height() / 100);
    f.periphery = fit.outer * 100;
    f.focusWidth = fit.focus * 100;
    f.focusHeight = fit.focusHeight * 100;
    f.peripheryCapped = fit.outerCapped;
    f.peripheryByRoom = fit.outerByRoom;
    f.focusCapped = fit.focusCapped;
    f.densityX = f.focusWidth * 100 / v.width;
    f.densityY = f.focusHeight * 100 / v.height;
    return f;
}

bool menu_over_launch(const SettingsMenu& menu) {
    const MenuFit f = menu_fit(menu);
    return f.peripheryCapped || f.focusCapped;
}

namespace {

// "12° right", "4° down", "0°".
// `plus` and `minus` are whole texts with {degrees} ("{degrees} right"), marked with I18N where they are passed (as
// the launcher's Headset menu pane).
std::wstring degrees(const Words& w, int v, const wchar_t* plus, const wchar_t* minus) {
    return v == 0 ? L"0\u00b0" : w.f(v > 0 ? plus : minus, {{L"degrees", std::to_wstring(v < 0 ? -v : v) + L"\u00b0"}});
}

std::wstring percent(double v) { return std::to_wstring(static_cast<int>(std::lround(v))) + L"%"; }

int px(int native, double percentOfNative) { return static_cast<int>(std::lround(native * percentOfNative / 100)); }

// A size in percent of the native eye, in pixels ("2232x1751"), or empty when the native size is unknown.
std::wstring pixels(const MenuLaunch& l, double width, double height) {
    if (l.nativeWidth <= 0 || l.nativeHeight <= 0)
        return L"";
    return std::to_wstring(px(l.nativeWidth, width)) + L"x" + std::to_wstring(px(l.nativeHeight, height));
}

// ACE's double-wide target for views of these sizes (percent of the native eye; `outer` the
// periphery's density, over `cropWidth` x `cropHeight` percent of the eye): the row of four views, as
// tall as the taller pair.
std::wstring target(const MenuLaunch& l, double outer, double focusWidth, double focusHeight, double cropWidth,
                    double cropHeight) {
    if (l.nativeWidth <= 0 || l.nativeHeight <= 0)
        return L"?";
    const int w = 2 * (px(l.nativeWidth, outer * cropWidth / 100) + px(l.nativeWidth, focusWidth));
    const int h = std::max(px(l.nativeHeight, outer * cropHeight / 100), px(l.nativeHeight, focusHeight));
    wchar_t mpx[16];
    swprintf_s(mpx, L"%.1f", static_cast<double>(w) * h / 1e6);
    return std::to_wstring(w) + L"x" + std::to_wstring(h) + L" (" + mpx + L" MP)";
}

}  // namespace

MenuResolution menu_resolution(const SettingsMenu& menu) {
    MenuResolution r;
    const MenuLaunch& l = menu.launch;
    const MenuPage page = menu_page(menu);
    const MenuItem section = menu_section(menu);
    if (!l.qvfr || (section != MenuItem::HeadResolution && page != MenuPage::Foveation && section != MenuItem::HeadView))
        return r;
    const MenuFit f = menu_fit(menu);
    const MenuValues& v = menu.values;
    r.shown = true;
    r.max = target(l, l.outer, l.focusWidth, l.focusHeight, l.cropWidth, l.cropHeight);
    r.current = target(l, f.periphery, f.focusWidth, f.focusHeight, v.crop_width(), v.crop_height());
    r.capped = f.peripheryCapped || f.focusCapped;
    if (r.capped)
        r.asked = target(l, v.periphery, f.wantFocusWidth, f.wantFocusHeight, v.crop_width(), v.crop_height());
    return r;
}

MenuStats menu_stats(const SettingsMenu& menu) {
    MenuStats s;
    const MenuLaunch& l = menu.launch;
    const MenuFit f = menu_fit(menu);
    // Density in effect (QVFR: the fit's, horizontal; Two-loop: eye_scale over the box's share).
    const double density = l.qvfr ? f.densityX : l.twoLoopScale * 100 / menu.values.width;
    if (l.nativePpd > 0)
        s.centerPpd = static_cast<int>(std::lround(l.nativePpd * (l.outputCap > 0 ? std::min(density, l.outputCap) : density) / 100));
    // Rendered against both native eyes, in pixels when the native size is known (as the launcher).
    // The focus mask (a game with it, QVFR): the share of the focus views' pixels it skips, at their size in pixels (a
    // nominal 2000-wide native eye while the native size is unknown: only the margin depends on it).
    double masked = 0;
    if (l.qvfr && l.focusMask && menu.values.focusMask) {
        const int nw = l.nativeWidth > 0 ? l.nativeWidth : 2000, nh = l.nativeHeight > 0 ? l.nativeHeight : 2000;
        masked = focus_mask_share(focus_area_of(menu.values.shape, menu.values.corners),
                                  static_cast<unsigned>(std::max(px(nw, f.focusWidth), 1)),
                                  static_cast<unsigned>(std::max(px(nh, f.focusHeight), 1)),
                                  static_cast<unsigned>(std::max(menu.values.focusMaskMargin, 0)));
    }
    double rendered, native;
    if (l.nativeWidth > 0 && l.nativeHeight > 0) {
        const auto area = [&](double w, double h) {
            return static_cast<double>(px(l.nativeWidth, w)) * px(l.nativeHeight, h);
        };
        rendered = l.qvfr ? 2 * (area(f.periphery * menu.values.crop_width() / 100, f.periphery * menu.values.crop_height() / 100) +
                                 area(f.focusWidth, f.focusHeight) * (1 - masked))
                          : 4 * area(l.twoLoopScale, l.twoLoopScale);
        native = 2.0 * l.nativeWidth * l.nativeHeight;
    } else {
        rendered = l.qvfr ? 2 * (f.periphery * f.periphery * menu.values.crop_width() * menu.values.crop_height() / 1e4 +
                                 f.focusWidth * f.focusHeight * (1 - masked))
                          : 4 * l.twoLoopScale * l.twoLoopScale;
        native = 2 * 100.0 * 100.0;
    }
    s.savedPercent = static_cast<int>(std::lround((1 - rendered / native) * 100));
    return s;
}

std::vector<std::wstring> menu_notes(const SettingsMenu& menu) {
    std::vector<std::wstring> notes;
    const MenuPage page = menu_page(menu);
    const MenuItem section = menu_section(menu);
    if (page == MenuPage::Headset || page == MenuPage::Game)
        return notes;
    if (section == MenuItem::HeadFg) {
        if (menu.cheeky.values.fg)
            notes.push_back(lang::tr(L"Needs OpenXR Toolkit's Turbo mode and other frame generation off"));
        return notes;
    }
    if (section == MenuItem::HeadDlss && menu.launch.ownDlss && fsr_runs(menu)) {
        if (menu.values.dlss.on && menu.values.dlss.textureSharpening != 0 && !menu.launch.sharpeningReady)
            notes.push_back(lang::tr(L"Texture sharpening was off at launch: it starts at the next launch"));
        notes.push_back(lang::tr(L"Each change rebuilds FSR: a short stutter (not texture sharpening)"));
        return notes;
    }
    if (section == MenuItem::HeadDlss && menu.launch.ownDlss) {
        if (menu.values.dlss.on)
            notes.push_back(lang::tr(menu.values.dlss.eyePreset == 6
                                         ? I18N(L"Periphery F: cheaper, more aliasing; suits eye tracking")
                                         : I18N(L"Periphery K: steadier; F is cheaper and suits eye tracking")));
        if (menu.values.dlss.on && menu.values.dlss.textureSharpening != 0 && !menu.launch.sharpeningReady)
            notes.push_back(lang::tr(L"Texture sharpening was off at launch: it starts at the next launch"));
        notes.push_back(lang::tr(L"Each change rebuilds DLSS: a short stutter (not texture sharpening)"));
        return notes;
    }
    if (section == MenuItem::HeadNr) {
        const NrSettings& n = menu.values.nr;
        if (n.on && !menu.values.dlss.on)
            notes.push_back(lang::tr(L"Runs with DLSS on (Use DLSS, above): it takes DLSS's depth and motion"));
        if (n.on)
            notes.push_back(lang::tr(L"Each view: 2 ms of GPU at least, about 2 ms and 130 MB a megapixel worked on"));
        if (n.on)
            notes.push_back(lang::tr(L"Preset, working sizes, coverage and order rebuild it: a short stutter"));
        return notes;
    }
    if (section == MenuItem::HeadCheeky || section == MenuItem::HeadVision ||
        (section == MenuItem::HeadDlss && !menu.launch.ownDlss))
        return notes;
    if (section == MenuItem::HeadFoveation) {
        // On the shape's lines: what Custom corners does (the resolution's notes stay with the Resolution section).
        const std::vector<MenuItem> items = menu_page_items(menu);
        const MenuItem at = items[std::clamp(menu.selected, 0, static_cast<int>(items.size()) - 1)];
        const bool shapeLine = at == MenuItem::Shape || (at >= MenuItem::ShapeTopLeftIn && at <= MenuItem::ShapeBottomRightUp);
        if (shapeLine) {
            const bool masks = menu.launch.focusMask && menu.values.focusMask;
            notes.push_back(lang::tr(
                !menu.values.shape.corners
                    ? I18N(L"Custom corners pulls each corner of the focus area in, e.g. the top ones for a trapezoid")
                : masks ? I18N(L"The cut corners aren't rendered: the shape saves pixels too")
                        : I18N(L"The focus views still render the whole box: the shape picks what stays sharp")));
            return notes;
        }
    }
    if (section == MenuItem::HeadView) {
        // The FOV follows at once; the views' pixels only without DLSS (and up to the launch size).
        if (menu.launch.dlssHeld)
            notes.push_back(lang::tr(L"DLSS is on: the view crops now, its pixel savings apply at the next launch"));
        if (menu.values.crop_width() > menu.launch.cropWidth + 0.5 || menu.values.crop_height() > menu.launch.cropHeight + 0.5)
            notes.push_back(lang::tr(L"Less crop than at launch: the periphery is softer until the next launch"));
        return notes;
    }
    if (!menu.dlssState.empty())  // in the player's language already (layer/menu.cpp)
        notes.push_back(menu.dlssState);
    if (menu.launch.qvfr && menu.launch.dlssHeld)
        notes.push_back(lang::tr(L"DLSS is on: resolution changes apply at the next launch"));
    else if (menu.launch.apiPath) {
        // The API path's periphery is live from its launch value down to a quarter of it (the game's eye targets are
        // made once, at launch); above, at the next launch.
        const double top = menu.launch.outer, floor = menu.launch.outer / 4;
        if (menu.values.periphery > top + 0.5)
            notes.push_back(lang::trf(L"Periphery above its launch value ({percent}) applies at the next launch",
                                      {{L"percent", percent(top)}}));
        else if (menu.values.periphery < floor - 0.5)
            notes.push_back(
                lang::trf(L"Periphery stays at {percent} (a quarter of its launch value) until the next launch",
                          {{L"percent", percent(floor)}}));
        else
            notes.push_back(
                lang::trf(L"Periphery applies now up to its launch value ({percent}); focus density and mode too",
                          {{L"percent", percent(top)}}));
    }
    else if (menu.launch.qvfr && !menu.launch.liveResolution)
        notes.push_back(lang::tr(L"Resolution changes apply at the next launch in this game"));
    const MenuFit f = menu_fit(menu);
    const MenuLaunch& l = menu.launch;
    const bool known = l.nativeWidth > 0 && l.nativeHeight > 0;
    if (f.focusCapped)
        notes.push_back(
            known ? lang::trf(L"Focus area needs {needed} px, {fits} fits: lower Focus density, width or height",
                              {{L"needed", pixels(l, f.wantFocusWidth, f.wantFocusHeight)},
                               {L"fits", pixels(l, f.focusWidth, f.focusHeight)}})
                  : std::wstring(
                        lang::tr(L"Focus area is too large for this launch: lower Focus density, width or height")));
    // The periphery's views at a density, over the FOV crop's share of the eye.
    const auto periphery = [&](double density) {
        return pixels(l, density * menu.values.crop_width() / 100, density * menu.values.crop_height() / 100);
    };
    if (f.peripheryCapped && f.peripheryByRoom)
        notes.push_back(
            known ? lang::trf(L"Periphery needs {needed} px, the focus area leaves {left}: lower Focus density or "
                              L"size to make room",
                              {{L"needed", periphery(menu.values.periphery)}, {L"left", periphery(f.periphery)}})
                  : std::wstring(lang::tr(L"Periphery needs more room: lower Focus density or size to make room")));
    else if (f.peripheryCapped)
        notes.push_back(lang::trf(L"Periphery above {percent} does not fit this launch: relaunch to use it",
                                  {{L"percent", percent(f.periphery)}}));
    return notes;
}

std::vector<MenuLine> menu_lines(const SettingsMenu& menu, bool shown) {
    std::vector<MenuLine> lines;
    const std::vector<MenuItem> items = menu_page_items(menu);
    MenuValues v = menu.values;
    const CheekySettings& c = menu.cheeky.values;
    const MenuFit fit = menu_fit(menu);
    const Words w{shown};  // the panel's language, or English for the log (layer/menu.cpp)
    const auto on_off = [&w](bool on) { return std::wstring(w.t(on ? I18N(L"On") : I18N(L"Off"))); };
    for (size_t i = 0; i < items.size(); ++i) {
        MenuLine line;
        line.label = items[i] == MenuItem::DlssOn && fsr_runs(menu)        ? w.t(I18N(L"Use FSR"))
                     : items[i] == MenuItem::HeadDlss && fsr_runs(menu)    ? L"FSR"
                     : items[i] == MenuItem::FocusOffset && !v.eyeTracking ? w.t(I18N(L"Vertical position"))
                                                                           : w.t(spec(items[i]).label);
        line.selected = static_cast<int>(i) == menu.selected;
        line.header = menu_item_is_header(items[i]);
        if (line.header) {
            line.selected = false;
            lines.push_back(line);
            continue;
        }
        if (items[i] == MenuItem::Page) {
            const int pages = static_cast<int>(menu_pages(menu).size());
            line.value = w.f(I18N(L"{page}  {number} of {count}"),
                             {{L"page", page_name(menu, menu_page(menu), w)},
                              {L"number", std::to_wstring(std::clamp(menu.page, 0, pages - 1) + 1)},
                              {L"count", std::to_wstring(pages)}});
        } else if (items[i] == MenuItem::Optimize)
            line.value = std::wstring(menu.optimizeStatus.empty() ? w.t(I18N(L"Right: start · Left: undo"))
                                                                  : w.t(menu.optimizeStatus.c_str()));  // optimizer.cpp's
        else if (items[i] == MenuItem::Profile) {
            const bool known = menu.profile >= 0 && menu.profile < static_cast<int>(menu.profiles.size());
            // A preset's name in the player's language (profiles.cpp marks them); a saved one's as written.
            const std::wstring name = known ? std::wstring(w.t(menu.profiles[menu.profile].c_str())) : std::wstring();
            line.value = !known                ? std::wstring(w.t(I18N(L"Your own")))
                         : menu.profileChanged ? w.f(I18N(L"{profile} (changed)"), {{L"profile", name}})
                                               : name;
        }
        else if (items[i] == MenuItem::NrStyle)
            line.value = w.t(dlss_choice_name(kNrStyles, v.nr.style));
        else if (items[i] == MenuItem::NrPreset)
            line.value = w.t(dlss_choice_name(kNrPresets, v.nr.preset));
        else if (items[i] == MenuItem::NrOrder)
            line.value = w.t(v.nr.beforeDlss ? I18N(L"Before DLSS") : I18N(L"After DLSS"));
        else if (items[i] == MenuItem::NrLimit)
            line.value = w.f(I18N(L"{megapixels} MP a view"),
                             {{L"megapixels", std::to_wstring(v.nr.workingLimit / 10) + L"." +
                                                  std::to_wstring(v.nr.workingLimit % 10)}});
        else if (const bool* flag = nr_flag(v.nr, items[i]))
            line.value = on_off(*flag);
        else if (const int* number = nr_number(v.nr, items[i]))
            line.value = std::to_wstring(*number) + L"%";
        else if (items[i] == MenuItem::DlssOn)
            line.value = on_off(v.dlss.on);
        else if (items[i] == MenuItem::DlssUpscaler)
            line.value = v.dlss.upscaler == 1 ? L"AMD FSR" : L"NVIDIA DLSS";
        else if (items[i] == MenuItem::DlssFocusMode && fsr_runs(menu) && v.dlss.focusScale >= 100)
            line.value = w.t(I18N(L"Native AA"));
        else if (items[i] == MenuItem::DlssFocusMode)
            line.value = std::wstring(w.t(dlss_choice_name(kDlssFocusModes, v.dlss.focusScale))) +
                         (v.dlss.focusScale < 100 ? L" (" + std::to_wstring(v.dlss.focusScale) + L"%)" : L"");
        else if (items[i] == MenuItem::DlssFocusPreset)
            line.value = v.dlss.focusPreset ? w.t(dlss_choice_name(kDlssFocusPresets, v.dlss.focusPreset))
                         : menu.launch.apiDlss ? w.t(I18N(L"The game's own"))
                         : v.dlss.focusScale == 50 ? w.t(I18N(L"Default (M)"))
                                                   : w.t(I18N(L"Default (K)"));
        else if (items[i] == MenuItem::DlssEyePreset)
            line.value = w.t(dlss_choice_name(kDlssEyePresets, v.dlss.eyePreset));
        else if (items[i] == MenuItem::DlssSharpening)
            line.value = w.t(dlss_choice_name(kDlssTextureSharpening, v.dlss.textureSharpening));
        else if (items[i] == MenuItem::FgOn)
            line.value = on_off(c.fg);
        else if (items[i] == MenuItem::FgMultiplier)
            line.value = std::to_wstring(c.fgMultiplier) + L"x";
        else if (items[i] == MenuItem::FgSize)
            line.value = std::to_wstring(c.fgSize) + L"%";
        else if (items[i] == MenuItem::FgScale)
            line.value = std::to_wstring(c.fgScale) + L"%";
        else if (items[i] == MenuItem::FgPeriphery)
            line.value = std::to_wstring(c.fgPeriphery) + L"%";
        else if (items[i] == MenuItem::FgBorder)
            line.value = on_off(c.fgBorder);
        else if (items[i] == MenuItem::FgRelease)
            line.value = w.t(kCheekyFgReleaseNames[cheeky_fg_release_index(c.fgRelease)]);
        else if (items[i] == MenuItem::CheekyProcessing)
            line.value = on_off(c.processing);
        else if (items[i] == MenuItem::CheekyOuterDlaa)
            line.value = on_off(c.outerDlaa);
        else if (items[i] == MenuItem::CheekyOuterScale)
            line.value = std::to_wstring(c.outerScale) + L"%";
        else if (items[i] == MenuItem::CheekyOuterPreset)
            line.value = w.t(cheeky_preset_name(c.outerPreset));
        else if (items[i] == MenuItem::CheekyFocusPreset)
            line.value = w.t(cheeky_preset_name(c.focusPreset));
        else if (items[i] == MenuItem::CheekySupersampling)
            line.value = std::to_wstring(c.supersampling) + L"%";
        else if (items[i] == MenuItem::CheekyMotionFix)
            line.value = on_off(c.motionFix);
        else if (items[i] == MenuItem::CheekyNr)
            line.value = on_off(c.nr);
        else if (items[i] == MenuItem::CheekyNrPreset)
            line.value = w.t(kCheekyNrPresets[std::clamp(c.nrPreset, 0, kCheekyNrPresetMax)]);
        else if (items[i] == MenuItem::CheekyNrPasses)
            line.value = std::to_wstring(c.nrPasses);
        else if (items[i] == MenuItem::Outline)
            line.value = on_off(v.outline);
        else if (items[i] == MenuItem::Shape)
            line.value = w.t(v.shape.corners ? I18N(L"Custom corners") : I18N(L"Rectangle"));
        else if (items[i] == MenuItem::EyeTracking)
            line.value = w.t(v.eyeTracking ? I18N(L"Eye tracked") : I18N(L"Fixed"));
        else if (items[i] == MenuItem::Overlay)
            line.value = on_off(v.overlay.enabled);
        else if (items[i] == MenuItem::Clock)
            line.value = on_off(v.clock.enabled);
        else if (items[i] == MenuItem::HotkeyFooter)
            line.value = on_off(v.hotkeyFooter);
        else if (items[i] == MenuItem::CpuIsolateMain)
            line.value = on_off(v.cpuIsolateMain);
        else if (items[i] == MenuItem::Crop)
            line.value = on_off(v.crop);
        else if (items[i] == MenuItem::Helmet)
            line.value = on_off(v.helmet.on);
        else if (items[i] == MenuItem::HelmetDebug)
            line.value = on_off(v.helmet.debugLight);
        else if (items[i] == MenuItem::Visor)
            line.value = w.t(kVisorPositionNames[static_cast<int>(v.helmet.visor)]);
        else if (items[i] == MenuItem::HelmetSize)
            line.value = std::to_wstring(v.helmet.size) + L"°";
        else if (items[i] == MenuItem::HelmetHeight)
            line.value = degrees(w, v.helmet.height, I18N(L"{degrees} up"), I18N(L"{degrees} down"));
        else if (items[i] == MenuItem::FocusOffset)
            line.value = degrees(w, v.offset.for_mode(v.eyeTracking), I18N(L"{degrees} up"), I18N(L"{degrees} down"));
        else if (items[i] == MenuItem::GazeDeadzone)
            line.value = !v.eyeTracking        ? std::wstring(w.t(I18N(L"Eye tracking off")))
                         : v.gazeDeadzone == 0 ? std::wstring(w.t(I18N(L"Off (follows every sample)")))
                                               : gaze_deadzone_text(v.gazeDeadzone) + L"°";
        else if (items[i] == MenuItem::FocusQuadDistance)
            line.value = std::to_wstring(focus_quad_metres(v.focusQuadStep)) + L" m";
        else if (items[i] == MenuItem::Sharpening && v.sharpen == 0)
            line.value = w.t(I18N(L"Off"));
        else if (items[i] == MenuItem::EyeOff)
            line.value = w.t(v.eyeOff == 1   ? I18N(L"Left (right eye only)")
                             : v.eyeOff == 2 ? I18N(L"Right (left eye only)")
                                             : I18N(L"None"));
        else if (items[i] == MenuItem::HelmetDetail)
            line.value = w.t(kHelmetDetailNames[v.helmet.detail ? 1 : 0]);
        else if (items[i] == MenuItem::HelmetDepth)
            line.value = v.helmet.depth ? std::to_wstring(v.helmet.depth) + L" cm" : std::wstring(w.t(I18N(L"Far")));
        else if (items[i] == MenuItem::OverlayYaw)
            line.value = degrees(w, v.overlay.yaw, I18N(L"{degrees} right"), I18N(L"{degrees} left"));
        else if (items[i] == MenuItem::OverlayPitch)
            line.value = degrees(w, v.overlay.pitch, I18N(L"{degrees} up"), I18N(L"{degrees} down"));
        else if (items[i] == MenuItem::OverlayWidth)
            line.value = std::to_wstring(v.overlay.width) + L"°";
        else if (items[i] == MenuItem::ClockYaw)
            line.value = degrees(w, v.clock.yaw, I18N(L"{degrees} right"), I18N(L"{degrees} left"));
        else if (items[i] == MenuItem::ClockPitch)
            line.value = degrees(w, v.clock.pitch, I18N(L"{degrees} up"), I18N(L"{degrees} down"));
        else if (items[i] == MenuItem::ClockWidth)
            line.value = std::to_wstring(v.clock.width) + L"°";
        else if (items[i] == MenuItem::MenuYaw)
            line.value = degrees(w, v.placement.yaw, I18N(L"{degrees} right"), I18N(L"{degrees} left"));
        else if (items[i] == MenuItem::MenuPitch)
            line.value = degrees(w, v.placement.pitch, I18N(L"{degrees} up"), I18N(L"{degrees} down"));
        else if (items[i] == MenuItem::MenuWidth)
            line.value = std::to_wstring(v.placement.width) + L"°";
        else
            line.value = std::to_wstring(*value_of(v, items[i])) + L"%";
        // What is in effect when the asked value does not fit the launch size.
        if (items[i] == MenuItem::Periphery && fit.peripheryCapped) {
            line.capped = true;
            line.value += L"  " + w.f(I18N(L"({percent} now)"), {{L"percent", percent(fit.periphery)}});
        } else if (items[i] == MenuItem::Density && fit.focusCapped) {
            line.capped = true;
            line.value += L"  " + (std::lround(fit.densityX) == std::lround(fit.densityY)
                                       ? w.f(I18N(L"({percent} now)"), {{L"percent", percent(fit.densityX)}})
                                       : w.f(I18N(L"({width} x {height} now)"),
                                             {{L"width", percent(fit.densityX)}, {L"height", percent(fit.densityY)}}));
        }
        lines.push_back(line);
    }
    return lines;
}

MenuPlacement read_menu_placement(const wchar_t* ini) {
    const auto read = [ini](const wchar_t* key, int fallback) { return ini_signed(ini, L"menu", key, fallback); };
    MenuPlacement p;
    p.yaw = std::clamp(read(L"yaw", p.yaw), -kMenuYawMax, kMenuYawMax);
    p.pitch = std::clamp(read(L"pitch", p.pitch), -kMenuPitchMax, kMenuPitchMax);
    p.width = std::clamp(read(L"width", p.width), kMenuWidthMin, kMenuWidthMax);
    return p;
}

MenuPose menu_pose(const MenuPlacement& p, float aspect) {
    constexpr double kRad = 3.14159265358979 / 180;
    const double yaw = p.yaw * kRad, pitch = p.pitch * kRad;
    MenuPose pose;
    pose.position[0] = static_cast<float>(kMenuDistance * std::sin(yaw) * std::cos(pitch));
    pose.position[1] = static_cast<float>(kMenuDistance * std::sin(pitch));
    pose.position[2] = static_cast<float>(-kMenuDistance * std::cos(yaw) * std::cos(pitch));
    // Turn about Y by -yaw, then about X by pitch: the quad's +Z (its front) points back at the eye.
    const double hy = -yaw / 2, hp = pitch / 2;
    pose.orientation[0] = static_cast<float>(std::cos(hy) * std::sin(hp));
    pose.orientation[1] = static_cast<float>(std::sin(hy) * std::cos(hp));
    pose.orientation[2] = static_cast<float>(-std::sin(hy) * std::sin(hp));
    pose.orientation[3] = static_cast<float>(std::cos(hy) * std::cos(hp));
    pose.width = static_cast<float>(2 * kMenuDistance * std::tan(p.width * kRad / 2));
    pose.height = pose.width * aspect;
    return pose;
}

}  // namespace simqvfr
