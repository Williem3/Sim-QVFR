// Offline check of the shared QVFR settings table (quad/qvfr_settings.h): the launcher (load_settings), the headset
// menu (read_menu_values) and the layer (read_live, views4_sizes) read the same values from an empty ini and clamp
// out-of-range values alike; [experiment] mode parses one way everywhere.
#include <cmath>
#include <cstring>
#include <string>

#include <windows.h>

#include "launcher/settings.h"
#include "layer/config.h"
#include "quad/qvfr_settings.h"
#include "quad/settings_menu.h"
#include "test_util.h"

using namespace simqvfr;

namespace {

void write_ini(const std::wstring& path, const char* text) {
    DeleteFileW(path.c_str());
    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    DWORD written = 0;
    WriteFile(f, text, static_cast<DWORD>(strlen(text)), &written, nullptr);
    CloseHandle(f);
}

int percent(double fraction) { return static_cast<int>(std::lround(fraction * 100)); }

// The three readers agree on the core keys of `ini`; `what` names the case.
void check_agree(const std::wstring& ini, const char* what, int width, int height, int outer, int density, int feather,
                 int corner, launcher::OutlineColor color) {
    const launcher::Settings s = launcher::load_settings(ini);
    const MenuValues m = read_menu_values(ini.c_str());
    const layer::LiveSettings l = layer::read_live(ini.c_str());
    const layer::Views4Sizes v = layer::views4_sizes(ini.c_str(), l.focusFraction, l.focusFractionY);
    const int layerHeight = l.focusFractionY > 0 ? percent(l.focusFractionY) : percent(l.focusFraction);
    const bool green = l.borderColor[0] == 0 && l.borderColor[1] == 1 && l.borderColor[2] == 0;
    const bool magenta = l.borderColor[0] == 1 && l.borderColor[1] == 0 && l.borderColor[2] == 1;
    const bool layerColor = color == launcher::OutlineColor::Green ? green : color == launcher::OutlineColor::Magenta ? magenta : !green && !magenta;
    std::printf("     %s: launcher %d x %d, outer %d, density %d, feather %d, corner %d; menu %d x %d, %d, %d, %d, %d; "
                "layer %d x %d, %d, %d, %d, %d\n", what, s.width, s.height, s.outer, s.inner, s.feather, s.corner, m.width,
                m.height, m.periphery, m.density, m.feather, m.corners, percent(l.focusFraction), layerHeight,
                percent(v.outer), percent(v.density), percent(l.feather), percent(l.cornerRadius));
    check(s.width == width && m.width == width && percent(l.focusFraction) == width && s.height == height && m.height == height &&
              layerHeight == height,
          (std::string(what) + ": the same focus box").c_str());
    check(s.outer == outer && m.periphery == outer && percent(v.outer) == outer && s.inner == density && m.density == density &&
              percent(v.density) == density,
          (std::string(what) + ": the same periphery and focus density").c_str());
    check(s.feather == feather && m.feather == feather && percent(l.feather) == feather && s.corner == corner &&
              m.corners == corner && percent(l.cornerRadius) == corner,
          (std::string(what) + ": the same feather and corners").c_str());
    check(s.color == color && layerColor, (std::string(what) + ": the same outline colour").c_str());
}

}  // namespace

int SIM_QVFR_TEST_MAIN() {
    const std::wstring ini = temp_path(L"sim-qvfr-qvfr-settings-test.ini");

    // An empty ini: the table's defaults everywhere (eye_scale 50, the periphery at half, so the density is 50 / 35 = 143%).
    write_ini(ini, "");
    check_agree(ini, "empty ini", 35, 35, 50, 143, 10, 0, launcher::OutlineColor::Green);
    check(kFocusPercent.def == 35 && kFeatherPercent.def == 10 && launcher::Settings{}.width == 35 &&
              launcher::Settings{}.feather == 10 && launcher::Settings{}.color == launcher::OutlineColor::Green &&
              MenuValues{}.width == 35 && MenuValues{}.feather == 10,
          "defaults: focus 35%, feather 10%, outline green in the launcher's and the menu's own structs");

    // Out of range: clamped alike (periphery and eye_scale at 100, density at 50..400, the box at 10..100).
    write_ini(ini, "[experiment]\r\nmode=views4\r\nfocus_percent=150\r\nfocus_height_percent=5\r\nperiphery_scale=180\r\n"
                   "focus_density=20\r\neye_scale=190\r\n[quad]\r\nfeather_percent=90\r\ncorner_radius_percent=300\r\n"
                   "debug_border_color=purple\r\n");
    check_agree(ini, "too high / too low", 100, 10, 100, 50, 50, 100, launcher::OutlineColor::Green);
    write_ini(ini, "[experiment]\r\nmode=views4\r\nfocus_percent=40\r\nperiphery_scale=4\r\nfocus_density=900\r\n"
                   "[quad]\r\nfeather_percent=0\r\ndebug_border_color=Magenta\r\n");
    check_agree(ini, "periphery under, density over", 40, 40, 10, 400, 0, 0, launcher::OutlineColor::Magenta);
    // eye_scale past 100 (the layer took up to 200 before the shared table): 100 for the periphery and the density.
    write_ini(ini, "[experiment]\r\nmode=views4\r\nfocus_percent=50\r\neye_scale=150\r\n[quad]\r\ndebug_border_color=teal\r\n");
    check_agree(ini, "eye_scale 150", 50, 50, 100, 200, 10, 0, launcher::OutlineColor::Teal);

    // [experiment] mode: one parse. two_loops is Two-loop in the launcher too (it showed as QVFR before).
    check(parse_render_mode(L"views4") == IniRenderMode::Qvfr && parse_render_mode(L"quad") == IniRenderMode::TwoLoop &&
              parse_render_mode(L"Two_Loops") == IniRenderMode::TwoLoop && parse_render_mode(L"") == IniRenderMode::Off &&
              parse_render_mode(L"off") == IniRenderMode::Off && parse_render_mode(L"views3") == IniRenderMode::Other &&
              parse_render_mode(nullptr) == IniRenderMode::Off,
          "mode: views4 QVFR, quad and two_loops Two-loop, empty or off Off, the research modes Other");
    write_ini(ini, "[experiment]\r\nmode=two_loops\r\neye_scale=60\r\n");
    const launcher::Settings loop = launcher::load_settings(ini);
    check(loop.mode == launcher::RenderMode::TwoLoop && loop.outer == 60, "mode=two_loops reads as Two-loop in the launcher");
    const std::wstring runtimeIni = temp_path(L"sim-qvfr-qvfr-settings-runtime.ini");
    write_ini(runtimeIni, "[runtime]\r\nlaunch_mode=two_loops\r\nlaunch_outer=60\r\n");
    check(launcher::load_launch_sizes(runtimeIni).mode == launcher::RenderMode::TwoLoop,
          "a two_loops launch is Two-loop in the launcher's live fit");
    DeleteFileW(ini.c_str());
    DeleteFileW(runtimeIni.c_str());
    return test_result();
}
