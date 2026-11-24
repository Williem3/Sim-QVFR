// Offline check of the game-mod ABI (src/mod_api): PluginAdapter refuses tables of another ABI, too short or for
// another game; an entry beyond a table's size, a null one, or a query answered below 0 gets GameAdapter's default;
// every struct survives the trip core -> C table -> C++ mod -> core (AdapterMod over a recording GameAdapter); the mod
// is told which game it is in; and SCS Software's two games, still built in, reach the layer through one mod's tables.
#include <windows.h>
#include <d3d11.h>
#include <wrl/client.h>

#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <string>

#include "games/scssoftware/adapter.h"
#include "games/scssoftware/ats/game.h"
#include "games/scssoftware/ets2/game.h"
#include "games/adapter.h"
#include "quad/eye_off.h"
#include "quad/showcase.h"
#include "mod_api/adapter_mod.h"
#include "mod_api/host_services.h"
#include "mod_api/mod_loader.h"
#include "mod_api/mod.hpp"
#include "mod_api/plugin_adapter.h"

namespace {

using simqvfr::DlssSettings;
using simqvfr::FocusArea;
using simqvfr::NrSettings;
using simqvfr::Views4Frame;
using simqvfr::Views4Layout;
using simqvfr::games::GameAdapter;
using simqvfr::games::GameInfo;
using simqvfr::games::QvfrStart;
using simqvfr::mod_api::PluginAdapter;

int failures = 0;

void check(bool ok, const char* what) {
    if (!ok) {
        std::printf("FAIL: %s\n", what);
        ++failures;
    }
}

// A game for the tests: Foveated DLSS and live resolution on, so the defaults that read GameInfo are visible.
const GameInfo& test_game() {
    static const GameInfo g = [] {
        GameInfo i = {"testgame", L"Test Game", L"TG", L"test.exe", L"Test", L"", L"1", L"-vr"};
        i.features.foveatedDlss = true;
        i.features.liveResolution = true;
        return i;
    }();
    return g;
}

// A mod answering every query with `answer` (0, 1 or kDefault), counting its calls.
class FakeMod final : public sqvfr::Mod {
public:
    int32_t answer = sqvfr::Mod::kDefault;
    int starts = 0, frameEnds = 0;
    int32_t has_qvfr() override { return answer; }
    int32_t qvfr_eye_columns() override { return answer; }
    int32_t loads_cheeky() override { return answer; }
    int32_t qvfr_live_resolution() override { return answer; }
    int32_t qvfr_composable() override { return answer; }
    uint32_t hang_watch_s() override { return answer > 0 ? 77u : 0u; }
    bool qvfr_start(const SqvfrQvfrStart&) override {
        ++starts;
        return true;
    }
    void qvfr_frame_end() override { ++frameEnds; }
};

void refuses_other_tables() {
    std::string why;
    check(!PluginAdapter::create(&test_game(), nullptr, why) && !why.empty(), "a null table is refused");

    FakeMod mod;
    SqvfrMod t = sqvfr::mod_table(mod, "testgame");
    check(PluginAdapter::create(&test_game(), &t, why) != nullptr && why.empty(), "a good table is taken");

    SqvfrMod other = t;
    other.abi = SQVFR_ABI_VERSION + 1;
    check(!PluginAdapter::create(&test_game(), &other, why) && why.find("ABI") != std::string::npos,
          "another ABI version is refused");

    other = t;
    other.size = 8;  // size and abi only
    check(!PluginAdapter::create(&test_game(), &other, why), "a table shorter than its header is refused");

    other = t;
    other.gameId = "othergame";
    check(!PluginAdapter::create(&test_game(), &other, why) && why.find("othergame") != std::string::npos,
          "another game's table is refused");
    other.gameId = nullptr;
    check(!PluginAdapter::create(&test_game(), &other, why), "a table naming no game is refused");

    // A mod refusing the core (its entry returns null).
    check(!PluginAdapter::load(&test_game(), [](const SqvfrHost*) -> const SqvfrMod* { return nullptr; }, why),
          "an entry returning no table is refused");
    check(!PluginAdapter::load(&test_game(), nullptr, why), "no entry point is refused");
}

void defaults_beyond_the_table() {
    FakeMod mod;
    mod.answer = 0;  // every query "no": what the core must ignore where the table doesn't reach
    SqvfrMod t = sqvfr::mod_table(mod, "testgame");
    t.size = offsetof(SqvfrMod, has_qvfr);  // the header alone: an "older" mod with no entries
    std::string why;
    auto a = PluginAdapter::create(&test_game(), &t, why);
    check(a != nullptr, "a header-only table is taken");
    if (!a)
        return;
    check(a->info() == &test_game(), "the identity is the core's");
    check(a->has_qvfr(), "has_qvfr beyond the table: GameAdapter's true");
    check(a->qvfr_composable(), "qvfr_composable beyond the table: has_qvfr's");
    check(a->loads_cheeky(), "loads_cheeky beyond the table: from GameInfo (Foveated DLSS on)");
    check(a->qvfr_live_resolution(), "qvfr_live_resolution beyond the table: from GameInfo");
    check(!a->qvfr_eye_columns(), "qvfr_eye_columns beyond the table: false");
    check(a->hang_watch_s() == 30, "hang_watch_s beyond the table: 30");
    check(!a->qvfr_start(QvfrStart{}), "qvfr_start beyond the table: refused (QVFR off)");
    check(mod.starts == 0, "an entry beyond the table is never called");
    a->qvfr_frame_end();
    check(mod.frameEnds == 0, "a void entry beyond the table is never called");
    int render[2] = {5, 5}, output[2] = {5, 5};
    a->qvfr_target_sizes(render, output);
    check(render[0] == 0 && output[1] == 0, "qvfr_target_sizes beyond the table: zeros");
    Views4Frame f;
    check(!a->qvfr_take_frame(f), "qvfr_take_frame beyond the table: no frame");

    // Cut in the middle: has_qvfr reached (answers 0), qvfr_start not.
    t.size = offsetof(SqvfrMod, qvfr_start);
    a = PluginAdapter::create(&test_game(), &t, why);
    check(a && !a->has_qvfr(), "an entry within the table is the mod's answer");
    check(a && !a->qvfr_composable(), "qvfr_composable reached: the mod's 0");
    check(a && !a->qvfr_start(QvfrStart{}) && mod.starts == 0, "qvfr_start just past the size is not called");

    // A null entry inside the table: the default too.
    t = sqvfr::mod_table(mod, "testgame");
    t.has_qvfr = nullptr;
    a = PluginAdapter::create(&test_game(), &t, why);
    check(a && a->has_qvfr(), "a null entry: the default");

    // The whole table: kDefault answers are the core's, others the mod's.
    t = sqvfr::mod_table(mod, "testgame");
    mod.answer = sqvfr::Mod::kDefault;
    a = PluginAdapter::create(&test_game(), &t, why);
    check(a && a->has_qvfr() && a->loads_cheeky() && !a->qvfr_eye_columns() && a->hang_watch_s() == 30,
          "kDefault answers: GameAdapter's");
    mod.answer = 1;
    check(a && a->qvfr_eye_columns() && a->hang_watch_s() == 77, "1: the mod's");
    check(a && a->qvfr_start(QvfrStart{}) && mod.starts == 1, "qvfr_start in the table is called");
}

// A core-side adapter keeping what it was given, and handing back a frame and views to check the way back.
class Recorder final : public GameAdapter {
public:
    QvfrStart start;
    std::wstring startIni;
    Views4Layout live;
    DlssSettings dlss;
    NrSettings nr;
    FocusArea area;
    int margin = 0;
    bool maskOn = false;
    LiveView view;
    Views4Frame frame;
    float gaze[2][2] = {};
    bool valid[2] = {};
    XrSwapchain chain = XR_NULL_HANDLE;
    const XrFrameEndInfo* frameEndIn = nullptr;

    const GameInfo* info() const override { return &test_game(); }
    bool qvfr_eye_columns() const override { return true; }
    unsigned hang_watch_s() const override { return 45; }
    bool qvfr_start(const QvfrStart& s) override {
        start = s;
        startIni = s.ini ? s.ini : L"";
        return true;
    }
    void qvfr_set_gaze(const float (&tan)[2][2], const bool (&v)[2], bool) override {
        std::memcpy(gaze, tan, sizeof(gaze));
        valid[0] = v[0];
        valid[1] = v[1];
    }
    void qvfr_set_live_layout(const Views4Layout& l) override { live = l; }
    void qvfr_set_live(double, double, double, bool) override {}
    void qvfr_set_focus_mask(bool on, const FocusArea& a, int m) override {
        maskOn = on;
        area = a;
        margin = m;
    }
    void qvfr_set_dlss(const DlssSettings& d) override { dlss = d; }
    void qvfr_set_nr(const NrSettings& n) override { nr = n; }
    bool qvfr_dlss_held() override { return true; }
    void qvfr_target_sizes(int (&render)[2], int (&output)[2]) override {
        render[0] = 3000;
        render[1] = 1500;
        output[0] = 3100;
        output[1] = 1600;
    }
    bool qvfr_take_frame(Views4Frame& f) override {
        f = frame;
        return true;
    }
    bool qvfr_latest_frame(Views4Frame& f) override {
        f = frame;
        return true;
    }
    void qvfr_note_views(const XrView*, uint32_t) override {}
    void qvfr_jitter_views(XrView* views, uint32_t count) override {
        for (uint32_t i = 0; i < count; ++i)
            views[i].fov.angleLeft += 0.25f;
    }
    void qvfr_frame_end() override {}
    simqvfr::games::QvfrMultiView qvfr_multi_view() const override { return simqvfr::games::QvfrMultiView::software; }
    void live_view(const LiveView& v) override { view = v; }
    void swapchain_releasing(XrSwapchain c) override { chain = c; }
    const XrFrameEndInfo* adjust_frame_end(const XrFrameEndInfo* info, PFN_xrEnumerateSwapchainImages) override {
        frameEndIn = info;
        return info;
    }
};

bool same(const Views4Layout& a, const Views4Layout& b) {
    bool ok = a.outer == b.outer && a.focus == b.focus && a.focusHeight == b.focusHeight && a.cropWidth == b.cropWidth &&
              a.cropHeight == b.cropHeight && a.layerFloor == b.layerFloor && a.live == b.live &&
              a.sharedColumnWidth == b.sharedColumnWidth && a.secondViewRoom == b.secondViewRoom &&
              a.secondViewBeside == b.secondViewBeside;
    for (int v = 0; v < 4; ++v)
        ok = ok && a.width[v] == b.width[v] && a.height[v] == b.height[v];
    return ok;
}

bool same(const Views4Frame& a, const Views4Frame& b) {
    bool ok = a.live == b.live && a.eyesOnly == b.eyesOnly;
    for (int e = 0; e < 2; ++e) {
        for (int i = 0; i < 4; ++i)
            ok = ok && a.outerTan[e][i] == b.outerTan[e][i] && a.focusTan[e][i] == b.focusTan[e][i];
        ok = ok && a.turn[e].x == b.turn[e].x && a.turn[e].y == b.turn[e].y && a.turn[e].z == b.turn[e].z &&
             a.turn[e].w == b.turn[e].w;
    }
    for (int v = 0; v < 4; ++v)
        ok = ok && a.width[v] == b.width[v] && a.height[v] == b.height[v] && a.renderWidth[v] == b.renderWidth[v] &&
             a.renderHeight[v] == b.renderHeight[v];
    return ok;
}

Views4Layout odd_layout() {
    Views4Layout l;
    l.outer = 0.61;
    l.focus = 1.37;
    l.focusHeight = 1.21;
    l.cropWidth = 0.93;
    l.cropHeight = 0.87;
    for (int v = 0; v < 4; ++v) {
        l.width[v] = 0.1 + 0.05 * v;
        l.height[v] = 0.9 - 0.1 * v;
    }
    l.layerFloor = 0.44;
    l.live = true;
    l.sharedColumnWidth = true;
    l.secondViewRoom = true;
    l.secondViewBeside = true;
    return l;
}

void structs_round_trip() {
    Recorder rec;
    simqvfr::mod_api::AdapterMod mod(rec);
    const SqvfrMod t = sqvfr::mod_table(mod, "testgame");
    std::string why;
    auto a = PluginAdapter::create(&test_game(), &t, why);
    check(a != nullptr, "the forwarding table is taken");
    if (!a)
        return;

    check(a->qvfr_eye_columns() && a->hang_watch_s() == 45 && a->qvfr_dlss_held(), "queries forward the adapter's");
    check(a->qvfr_multi_view() == simqvfr::games::QvfrMultiView::software, "the multi-view badge round-trips");

    QvfrStart s;
    s.layout = odd_layout();
    s.focusFraction = 0.31;
    s.focusFractionY = 0.27;
    s.dlssLive = false;
    s.focusCull = true;
    s.focusCullScale = 1.6;
    s.focusCullGaze = true;
    s.ini = L"C:\\run\\sim-qvfr.ini";
    check(a->qvfr_start(s), "qvfr_start forwards");
    check(same(rec.start.layout, s.layout) && rec.start.focusFraction == s.focusFraction &&
              rec.start.focusFractionY == s.focusFractionY && rec.start.dlssLive == s.dlssLive &&
              rec.start.focusCull == s.focusCull && rec.start.focusCullScale == s.focusCullScale &&
              rec.start.focusCullGaze == s.focusCullGaze && rec.startIni == s.ini,
          "QvfrStart round-trips");

    a->qvfr_set_live_layout(odd_layout());
    check(same(rec.live, odd_layout()), "Views4Layout round-trips");

    const float tan[2][2] = {{0.1f, -0.2f}, {0.3f, 0.4f}};
    const bool valid[2] = {true, false};
    a->qvfr_set_gaze(tan, valid, true);
    check(std::memcmp(rec.gaze, tan, sizeof(tan)) == 0 && rec.valid[0] && !rec.valid[1], "the gaze round-trips");

    DlssSettings d;
    d.on = true;
    d.folder = L"D:\\dlss";
    d.focusScale = 67;
    d.focusPreset = 6;
    d.eyePreset = 13;
    d.textureSharpening = 2;
    d.carVectors = false;
    d.upscaler = 1;
    a->qvfr_set_dlss(d);
    check(rec.dlss == d, "DlssSettings round-trip");

    NrSettings n;
    n.on = true;
    n.folder = L"E:\\nr";
    n.preset = 2;
    n.style = 1;
    n.intensity = 55;
    n.localTone = 150;
    n.localStructure = 20;
    n.autoMask = true;
    n.periphery = true;
    n.focusScale = 80;
    n.peripheryScale = 40;
    n.workingLimit = 25;
    n.peripheryCoverage = 70;
    n.edgeBlend = 15;
    n.beforeDlss = true;
    n.exposure = 120;
    n.inputGamma = false;
    n.autoExposure = false;
    n.maxGain = 200;
    n.saveFrames = 3;
    a->qvfr_set_nr(n);
    check(rec.nr == n, "NrSettings round-trip");

    FocusArea area;
    for (int i = 0; i < 2 * simqvfr::kFocusPointsMax; ++i)
        area.point[i] = 0.01f * i;
    area.points = 7;
    area.radius = 0.2f;
    area.aspect = 1.3f;
    area.visible[0] = 0.1f;
    area.visible[3] = 0.9f;
    a->qvfr_set_focus_mask(true, area, 9);
    check(rec.maskOn && rec.area == area && rec.margin == 9, "FocusArea round-trips");

    GameAdapter::LiveView v;
    v.periphery = 0.6;
    v.focusX = 0.33;
    v.focusY = 0.29;
    v.density = 1.4;
    v.feather = 0.2;
    v.cornerRadius = 0.5;
    v.border = true;
    v.borderColor[2] = 0.5f;
    v.gaze = false;
    a->live_view(v);
    check(rec.view.periphery == v.periphery && rec.view.focusX == v.focusX && rec.view.focusY == v.focusY &&
              rec.view.density == v.density && rec.view.feather == v.feather &&
              rec.view.cornerRadius == v.cornerRadius && rec.view.border == v.border &&
              std::memcmp(rec.view.borderColor, v.borderColor, sizeof(v.borderColor)) == 0 && rec.view.gaze == v.gaze,
          "LiveView round-trips");

    for (int e = 0; e < 2; ++e) {
        for (int i = 0; i < 4; ++i) {
            rec.frame.outerTan[e][i] = 0.1f * (i + 1) + e;
            rec.frame.focusTan[e][i] = -0.05f * (i + 1) - e;
        }
        rec.frame.turn[e] = {0.01f * e, 0.02f, 0.03f, 0.99f};
    }
    rec.frame.live = true;
    rec.frame.eyesOnly = true;
    for (int i = 0; i < 4; ++i) {
        rec.frame.width[i] = 0.2 + i;
        rec.frame.height[i] = 0.3 + i;
        rec.frame.renderWidth[i] = 0.4 + i;
        rec.frame.renderHeight[i] = 0.5 + i;
    }
    Views4Frame got;
    check(a->qvfr_take_frame(got) && same(got, rec.frame), "Views4Frame round-trips (take)");
    Views4Frame latest;
    check(a->qvfr_latest_frame(latest) && same(latest, rec.frame), "Views4Frame round-trips (latest)");

    int render[2] = {}, output[2] = {};
    a->qvfr_target_sizes(render, output);
    check(render[0] == 3000 && render[1] == 1500 && output[0] == 3100 && output[1] == 1600, "target sizes round-trip");

    XrView views[2] = {{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};
    int marker = 0;
    views[1].next = &marker;
    for (int i = 0; i < 2; ++i) {
        views[i].pose.orientation = {0.1f, 0.2f, 0.3f, 0.9f};
        views[i].pose.position = {1.f, 2.f, 3.f};
        views[i].fov = {-0.8f, 0.7f, 0.6f, -0.5f};
    }
    a->qvfr_jitter_views(views, 2);
    check(views[0].fov.angleLeft == -0.8f + 0.25f && views[1].fov.angleRight == 0.7f &&
              views[1].pose.position.z == 3.f && views[1].type == XR_TYPE_VIEW && views[1].next == &marker,
          "XrView jitter comes back, type and next kept");

    const XrSwapchain chain = reinterpret_cast<XrSwapchain>(static_cast<uintptr_t>(0x1234567890ull));
    a->swapchain_releasing(chain);
    check(rec.chain == chain, "an XrSwapchain handle round-trips");

    XrFrameEndInfo fe = {XR_TYPE_FRAME_END_INFO};
    check(a->adjust_frame_end(&fe, nullptr) == &fe && rec.frameEndIn == &fe, "adjust_frame_end passes the frame");
}

// What a mod's entry was told: the game the core named in the host table.
std::string toldGame;

void host_names_the_game() {
    std::string why;
    auto a = PluginAdapter::load(
        &test_game(),
        [](const SqvfrHost* host) -> const SqvfrMod* {
            toldGame = host && host->gameId ? host->gameId : "";
            static FakeMod mod;
            static const SqvfrMod table = sqvfr::mod_table(mod, "testgame");
            return &table;
        },
        why);
    check(a != nullptr && toldGame == "testgame", "the entry is told the game in the host table");
}

void scs_through_the_tables() {
    const GameInfo* games[2] = {&::ats::kGame, &::ets2::kGame};
    GameAdapter* seen[2] = {};
    for (int g = 0; g < 2; ++g) {
        GameAdapter& a = simqvfr::games::adapter_for(games[g]);
        seen[g] = &a;
        check(dynamic_cast<PluginAdapter*>(&a) != nullptr, "the SCS game's adapter is a PluginAdapter");
        check(a.info() == games[g], "the SCS game's identity is its own");
        check(&simqvfr::games::adapter_for(games[g]) == &a, "one adapter per game for the process");
        GameAdapter& direct = scs::adapter(games[g]);
        const wchar_t* ini = nullptr;  // no ini: QVFR off, as the SCS adapter reads it
        check(a.has_qvfr() == direct.has_qvfr() && a.qvfr_composable() == direct.qvfr_composable() &&
                  a.qvfr_compose_d3d11() == direct.qvfr_compose_d3d11() &&
                  a.qvfr_eye_columns() == direct.qvfr_eye_columns() && a.loads_cheeky() == direct.loads_cheeky() &&
                  a.qvfr_live_resolution() == direct.qvfr_live_resolution() &&
                  a.attaches_action_sets() == direct.attaches_action_sets() &&
                  a.hang_watch_s() == direct.hang_watch_s() && a.observes_d3d11() == direct.observes_d3d11() &&
                  a.wants_gaze() == direct.wants_gaze() && a.shows_overlay() == direct.shows_overlay() &&
                  a.own_qvfr(ini) == direct.own_qvfr(ini) && a.qvfr_multi_view() == direct.qvfr_multi_view() &&
                  a.qvfr_fixation_m() == direct.qvfr_fixation_m(),
              "the SCS game answers through the table as directly");
        XrFrameEndInfo fe = {XR_TYPE_FRAME_END_INFO};
        check(a.adjust_frame_end(&fe, nullptr) == direct.adjust_frame_end(&fe, nullptr),
              "the SCS game's frame end through the table is the direct one");
    }
    check(seen[0] != seen[1], "ATS and ETS2 have adapters of their own");

    // The one mod picks the game from the host table, and refuses a game that isn't SCS Software's.
    SqvfrHost host = simqvfr::mod_api::host_table();
    host.gameId = "ets2";
    const SqvfrMod* t = scs::mod_entry(&host);
    check(t && std::strcmp(t->gameId, "ets2") == 0, "the SCS mod answers ETS2's table for ets2");
    host.gameId = "ATS";
    t = scs::mod_entry(&host);
    check(t && std::strcmp(t->gameId, "ats") == 0, "the SCS mod answers ATS's table for ats (any case)");
    host.gameId = "acc";
    check(scs::mod_entry(&host) == nullptr, "the SCS mod refuses another game");
    host.gameId = "ats";
    host.abi = SQVFR_ABI_VERSION + 1;
    check(scs::mod_entry(&host) == nullptr, "the SCS mod refuses a core of another ABI");
}

// The module an address is in.
HMODULE module_of(const void* address) {
    HMODULE m = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       static_cast<LPCWSTR>(address), &m);
    return m;
}

// The SCS games' mod DLL (SIM_QVFR_SCS_AS_MOD) through the core's real loader: mods\scs\sim-qvfr-scs.dll beside the
// core (here the test program, in build\<config>), LoadLibraryExW by full path, sqvfr_mod_entry, the tables.
void scs_dll_through_the_loader() {
    const std::wstring folder = simqvfr::mod_api::mods_folder();
    check(folder.size() > 5 && _wcsicmp(folder.c_str() + folder.size() - 5, L"\\mods") == 0, "the mods folder");
    std::string why;
    check(!simqvfr::mod_api::load_mod_dll(L"mods\\scs\\sim-qvfr-scs.dll", &::ats::kGame, why) &&
              why.find("full path") != std::string::npos,
          "a relative path is refused");
    check(!simqvfr::mod_api::load_mod_dll(folder + L"\\none\\sim-qvfr-none.dll", &::ats::kGame, why) &&
              why.find("no mod") != std::string::npos,
          "a missing mod is refused");
#ifdef SIM_QVFR_SCS_AS_MOD
    const std::wstring path = simqvfr::mod_api::mod_dll_path(L"scs", L"sim-qvfr-scs.dll");
    const GameInfo* games[2] = {&::ats::kGame, &::ets2::kGame};
    for (const GameInfo* game : games) {
        auto a = simqvfr::mod_api::load_mod_dll(path, game, why);
        check(a != nullptr, "the SCS mod DLL loads");
        if (!a) {
            std::printf("  why: %s\n", why.c_str());
            continue;
        }
        const SqvfrMod& t = a->table();
        const HMODULE dll = GetModuleHandleW(L"sim-qvfr-scs.dll");
        check(dll != nullptr && module_of(reinterpret_cast<const void*>(t.has_qvfr)) == dll &&
                  module_of(t.self) == dll,
              "the table and its mod are the DLL's");
        check(t.abi == SQVFR_ABI_VERSION && t.size == sizeof(SqvfrMod) && std::strcmp(t.gameId, game->id) == 0,
              "the DLL's table: ABI, size, its game");
        check(a->info() == game && !a->has_qvfr() && !a->own_qvfr(nullptr) && !a->wants_gaze() && !a->shows_overlay() &&
                  !a->observes_d3d11() && a->hang_watch_s() == 30 &&
                  a->qvfr_multi_view() == simqvfr::games::QvfrMultiView::none,
              "the DLL's SCS adapter answers as the built-in one before a launch");
        XrFrameEndInfo fe = {XR_TYPE_FRAME_END_INFO};
        check(a->adjust_frame_end(&fe, nullptr) == &fe, "the DLL passes a frame on when the corner is off");
    }
    // The layer's own path for the game goes through the DLL too.
    GameAdapter& layer = simqvfr::games::adapter_for(&::ats::kGame);
    auto* plugin = dynamic_cast<PluginAdapter*>(&layer);
    check(plugin && module_of(plugin->table().self) == GetModuleHandleW(L"sim-qvfr-scs.dll"),
          "adapter_for(ATS) is the DLL's");
#else
    std::printf("mod_api_test: SIM_QVFR_SCS_AS_MOD off, the SCS DLL isn't built (its load not tested)\n");
#endif
}

// The host's D3D11 hooks with a mod's C observers (what a mod DLL's context_hooks calls reach), on a WARP device: draws
// and render-target binds seen, a draw replaced, the bypass passing our own calls straight on.
int hookDraws = 0, hookTargets = 0, hookReplaced = 0;
void* hookContext = nullptr;

void host_d3d11_hooks_on_warp() {
    using Microsoft::WRL::ComPtr;
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &device,
                                 nullptr, &context))) {
        check(false, "a WARP device");
        return;
    }
    const SqvfrHost& h = simqvfr::mod_api::host_table();
    hookContext = context.Get();
    SqvfrD3D11Observers o = {};
    o.draw = [](void* c, int32_t, uint32_t, uint32_t) {
        if (c == hookContext)
            ++hookDraws;
    };
    o.targets = [](void* c, uint32_t, void* const*, void*) {
        if (c == hookContext)
            ++hookTargets;
    };
    o.draw_replace = [](void*, int32_t entry, const SqvfrDrawCall* call) -> int32_t {
        if (entry == SQVFR_D3D11_DRAW && call->count == 7) {
            ++hookReplaced;
            return 1;  // drawn in the game's place
        }
        return 0;
    };
    const uint32_t wanted = (1u << SQVFR_D3D11_DRAW) | (1u << SQVFR_D3D11_OM_SET_RENDER_TARGETS);
    check(h.d3d11_hooks_install(context.Get(), wanted, &o) == 1 && h.d3d11_hooks_installed() == 1,
          "the host's D3D11 hooks install with C observers");
    context->OMSetRenderTargets(0, nullptr, nullptr);
    context->Draw(3, 0);
    context->Draw(7, 0);
    check(hookDraws == 2 && hookTargets == 1, "draws and binds reach the C observers");
    check(hookReplaced == 1, "a draw replaced through the C observer");
    h.d3d11_hooks_bypass(1);
    context->Draw(3, 0);
    h.d3d11_hooks_bypass(0);
    check(hookDraws == 2, "a bypassed call goes straight on");
    h.d3d11_hooks_rescan();
    h.d3d11_hooks_uninstall();
    context->Draw(3, 0);
    check(hookDraws == 2 && h.d3d11_hooks_installed() == 0, "nothing observed after uninstall");
    SqvfrNvapiSupport nv = {-1, -1, -1, -1, -1};
    h.nvapi_query(device.Get(), &nv);
    check(nv.loaded == 0 || nv.loaded == 1, "nvapi_query answers (WARP: whatever the PC has)");
    check(h.nvapi_status_name(0) != nullptr, "nvapi_status_name");
}

void host_table_filled() {
    const SqvfrHost& h = simqvfr::mod_api::host_table();
    check(h.size == sizeof(SqvfrHost) && h.abi == SQVFR_ABI_VERSION, "the host table's header");
    check(h.log && h.ini_path && h.captures_dir && h.hook && h.unhook && h.module_base && h.code_matches &&
              h.code_write && h.code_redirect && h.vtable_slot && h.vtable_slot_replace && h.readable &&
              h.request_game_exit,
          "every host entry is filled");
    check(h.log_debug_on && h.log_dir && h.d3d11_hooks_install && h.d3d11_hooks_rescan && h.d3d11_hooks_uninstall &&
              h.d3d11_hooks_installed && h.d3d11_hooks_bypass && h.multi_view_shaders_install &&
              h.multi_view_shaders_device && h.multi_view_shaders_option && h.multi_view_shaders_frame_end &&
              h.multi_view_shaders_backend && h.nvapi_query && h.nvapi_set_two_views && h.nvapi_status_name &&
              h.aftermath_built && h.aftermath_enable_dumps && h.aftermath_attach && h.aftermath_mark &&
              h.aftermath_marking && h.aftermath_set_marker_names && h.aftermath_after_removal && h.aftermath_detach &&
              h.hang_watch_on_fire && h.stock_wanted && h.set_stock_active && h.note_scene_frame && h.eye_off &&
              h.car_lighting_bits,
          "every appended host entry is filled");
    // The shared switches are the core's one copy.
    simqvfr::set_stock_wanted(true);
    check(h.stock_wanted() == 1, "stock_wanted through the host is the core's");
    simqvfr::set_stock_wanted(false);
    simqvfr::set_eye_off(1);
    check(h.eye_off() == 1, "eye_off through the host is the core's");
    simqvfr::set_eye_off(-1);
    h.set_stock_active(1);
    check(simqvfr::stock_active(), "set_stock_active through the host reaches the core");
    h.set_stock_active(0);
    const uint64_t frames = simqvfr::scene_frames();
    h.note_scene_frame();
    check(simqvfr::scene_frames() == frames + 1, "note_scene_frame through the host counts in the core");
    // Nested bypasses balance (context_hooks.h's depth count).
    h.d3d11_hooks_bypass(1);
    h.d3d11_hooks_bypass(1);
    h.d3d11_hooks_bypass(0);
    h.d3d11_hooks_bypass(0);
    check(h.d3d11_hooks_installed() == 0, "no D3D11 hooks without an install");
    check(h.ini_path() != nullptr && h.module_base() != 0, "host entries answer");
    int local = 0;
    check(h.readable(&local, sizeof(local)) == 1 && h.readable(nullptr, 4) == 0, "readable");
    h.log(SQVFR_LOG_INFO, "mod_api_test: a line through the host table");
}

}  // namespace

int SIM_QVFR_TEST_MAIN() {
    refuses_other_tables();
    defaults_beyond_the_table();
    structs_round_trip();
    host_names_the_game();
    scs_through_the_tables();
    scs_dll_through_the_loader();
    host_d3d11_hooks_on_warp();
    host_table_filled();
    if (failures) {
        std::printf("mod_api_test: %d failure(s)\n", failures);
        return 1;
    }
    std::printf("mod_api_test: ok\n");
    return 0;
}
