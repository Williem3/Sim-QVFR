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
#include "mod_api/mod_trust.h"
#include "sha256_file.h"
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

// The trust rules (mod_api/mod_trust.h, option A): official only by the pinned certificate; a third-party mod only
// with the user's approval of that exact file; online (an anti-cheat running) only with its author's own trusted
// signature and the user's Allow online; nothing in an unsigned build lends a signature.
void trust_rules() {
    using simqvfr::mod_api::decide_mod_trust;
    using simqvfr::mod_api::ModTrust;
    using simqvfr::mod_api::TrustInput;
    const std::string hash(64, 'a'), other(64, 'b');
    const std::wstring official = L"4683DA63C881A76D5A1E803751B481F621ADF9C1";
    const std::wstring author = L"0123456789ABCDEF0123456789ABCDEF01234567";

    check(simqvfr::mod_api::official_thumbprint(official) &&
              simqvfr::mod_api::official_thumbprint(L"4683da63c881a76d5a1e803751b481f621adf9c1") &&
              !simqvfr::mod_api::official_thumbprint(author) && !simqvfr::mod_api::official_thumbprint(L""),
          "trust: only the pinned certificate is official");

    TrustInput in;
    in.mod.sha256 = hash;
    in.mod.signatureTrusted = true;
    in.mod.thumbprint = official;
    in.coreOfficial = true;
    check(decide_mod_trust(in).trust == ModTrust::Official, "trust: the maintainer's signature is official");
    in.onlineLaunch = true;
    check(decide_mod_trust(in).trust == ModTrust::Official, "trust: and official online too, without an approval");

    // Someone else's DLL, even in the official mod's folder of a signed release: never official.
    TrustInput c;
    c.mod.sha256 = hash;
    c.coreOfficial = true;
    c.firstPartyPath = true;
    check(decide_mod_trust(c).trust == ModTrust::Refused, "trust: an unsigned DLL in a signed release isn't first-party");
    c.mod.signatureTrusted = true;
    c.mod.thumbprint = author;
    c.mod.signer = L"Some Author";
    check(decide_mod_trust(c).trust == ModTrust::Refused, "trust: another signer, not approved: refused");
    // A trust check that Windows failed can't pass for official because of a copied thumbprint.
    TrustInput forged = c;
    forged.mod.signatureTrusted = false;
    forged.mod.thumbprint = official;
    check(decide_mod_trust(forged).trust == ModTrust::Refused, "trust: the pinned thumbprint without a trusted signature isn't official");

    c.approval.sha256 = other;
    check(decide_mod_trust(c).trust == ModTrust::Refused &&
              decide_mod_trust(c).why.find("changed") != std::string::npos,
          "trust: a file changed since it was approved is refused");
    c.approval.sha256 = hash;
    const auto community = decide_mod_trust(c);
    check(community.trust == ModTrust::Community && community.why.find("not made by Sim-QVFR") != std::string::npos &&
              community.why.find("Some Author") != std::string::npos,
          "trust: approved, it loads as third-party, named by its author");

    c.onlineLaunch = true;
    check(decide_mod_trust(c).trust == ModTrust::Refused, "trust: online needs the user's Allow online");
    c.approval.online = true;
    check(decide_mod_trust(c).trust == ModTrust::Community, "trust: author-signed + Allow online: it loads online");
    c.mod.signatureTrusted = false;
    c.mod.signer.clear();
    c.mod.thumbprint.clear();
    check(decide_mod_trust(c).trust == ModTrust::Refused, "trust: unsigned never loads online, whatever the user allowed");
    c.onlineLaunch = false;
    check(decide_mod_trust(c).trust == ModTrust::Community, "trust: unsigned but approved loads offline");

    // An unsigned build (development, nightly): the package's own mods, offline only.
    TrustInput dev;
    dev.mod.sha256 = hash;
    dev.firstPartyPath = true;
    check(decide_mod_trust(dev).trust == ModTrust::FirstPartyUnsignedBuild, "trust: an unsigned build loads its own mods");
    dev.onlineLaunch = true;
    check(decide_mod_trust(dev).trust == ModTrust::Refused, "trust: but not while an anti-cheat runs");
    dev.onlineLaunch = false;
    dev.firstPartyPath = false;
    check(decide_mod_trust(dev).trust == ModTrust::Refused, "trust: and only its own (another path needs approval)");
    dev.mod.sha256.clear();
    dev.firstPartyPath = true;
    check(decide_mod_trust(dev).trust == ModTrust::Refused, "trust: a file that can't be read is refused");

    // Approvals in [mods].
    wchar_t temp[MAX_PATH] = {};
    GetTempPathW(MAX_PATH, temp);
    const std::wstring ini = std::wstring(temp) + L"sim-qvfr-mod-trust-test.ini";
    DeleteFileW(ini.c_str());
    simqvfr::mod_api::ModApproval a;
    a.sha256 = hash;
    a.online = true;
    check(simqvfr::mod_api::write_mod_approval(ini, L"demo", a), "trust: an approval is written");
    const auto back = simqvfr::mod_api::read_mod_approval(ini, L"demo");
    check(back.sha256 == hash && back.online, "trust: and read back");
    check(simqvfr::mod_api::read_mod_approval(ini, L"other").sha256.empty(), "trust: another mod isn't approved");
    WritePrivateProfileStringW(L"mods", L"bad_sha256", L"not-a-hash", ini.c_str());
    check(simqvfr::mod_api::read_mod_approval(ini, L"bad").sha256.empty(), "trust: a malformed hash approves nothing");
    a.sha256.clear();
    simqvfr::mod_api::write_mod_approval(ini, L"demo", a);
    const auto gone = simqvfr::mod_api::read_mod_approval(ini, L"demo");
    check(gone.sha256.empty() && !gone.online, "trust: an approval taken away takes Allow online with it");
    DeleteFileW(ini.c_str());

    // This test program: unsigned, readable.
    wchar_t self[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    const auto facts = simqvfr::mod_api::read_mod_facts(self);
    check(facts.sha256.size() == 64 && !facts.signatureTrusted && facts.thumbprint.empty(),
          "trust: an unsigned file's facts: its hash, no signature");
    check(!simqvfr::mod_api::module_officially_signed(reinterpret_cast<const void*>(&trust_rules)),
          "trust: a test build isn't officially signed");
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
    // Through the trust check: the package's own path in this unsigned build loads; the same file taken for a
    // third-party mod needs the user's approval of exactly it, and never loads online unsigned.
    {
        wchar_t temp[MAX_PATH] = {};
        GetTempPathW(MAX_PATH, temp);
        const std::wstring ini = std::wstring(temp) + L"sim-qvfr-mod-loader-test.ini";
        DeleteFileW(ini.c_str());
        simqvfr::mod_api::TrustVerdict verdict;
        using simqvfr::mod_api::ModTrust;
        check(simqvfr::mod_api::load_checked_mod(path, L"scs", ini, &::ats::kGame, true, false, verdict, why) &&
                  verdict.trust == ModTrust::FirstPartyUnsignedBuild,
              "checked: the package's own mod loads in an unsigned build");
        check(!simqvfr::mod_api::load_checked_mod(path, L"scs", ini, &::ats::kGame, false, false, verdict, why) &&
                  verdict.trust == ModTrust::Refused,
              "checked: as a third-party mod, not approved: refused");
        simqvfr::mod_api::ModApproval a;
        a.sha256 = simqvfr::sha256_file(path.c_str());
        simqvfr::mod_api::write_mod_approval(ini, L"scs", a);
        check(simqvfr::mod_api::load_checked_mod(path, L"scs", ini, &::ats::kGame, false, false, verdict, why) &&
                  verdict.trust == ModTrust::Community,
              "checked: approved, it loads as third-party");
        a.online = true;
        simqvfr::mod_api::write_mod_approval(ini, L"scs", a);
        check(!simqvfr::mod_api::load_checked_mod(path, L"scs", ini, &::ats::kGame, false, true, verdict, why) &&
                  verdict.trust == ModTrust::Refused,
              "checked: unsigned, it never loads online");
        DeleteFileW(ini.c_str());
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
              h.car_lighting_bits && h.mod_setting,
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

// The SDK's example mod (sdk/examples/hello), built by sdk/cmake/SqvfrMod.cmake: it loads through the loader under its
// id, answers for the game it is asked for, and leaves the game alone.
void sdk_example_loads() {
    std::string why;
    std::wstring path = SQVFR_HELLO_DLL;
    for (wchar_t& c : path)
        if (c == L'/')
            c = L'\\';
    auto a = simqvfr::mod_api::load_mod_dll(path, &::ats::kGame, why, "hello");
    check(a != nullptr, "the SDK's example mod loads");
    if (a) {
        check(!a->has_qvfr() && std::strcmp(a->table().gameId, "ats") == 0,
              "the example answers for ATS and leaves its rendering alone");
        a->instance_created(L"", L"");
    }
    wchar_t folder[MAX_PATH] = {};
    wcscpy_s(folder, path.c_str());
    *wcsrchr(folder, L'\\') = L'\0';
    check(GetFileAttributesW((std::wstring(folder) + L"\\mod.json").c_str()) != INVALID_FILE_ATTRIBUTES &&
              GetFileAttributesW((std::wstring(folder) + L"\\settings.json").c_str()) != INVALID_FILE_ATTRIBUTES,
          "sqvfr_add_mod puts mod.json and settings.json beside the DLL");
}

// A game a community mod adds (games/game.h mod games): listed after Sim-QVFR's own, found by id and exe, tied to its
// mod; a mod can't take a game Sim-QVFR has, nor give an exe with a folder.
void mod_adds_a_game() {
    namespace games = simqvfr::games;
    wchar_t temp[MAX_PATH] = {};
    GetTempPathW(MAX_PATH, temp);
    const std::wstring mods = std::wstring(temp) + L"sqvfr-mod-games-test";
    const std::wstring folder = mods + L"\\newgame";
    CreateDirectoryW(mods.c_str(), nullptr);
    CreateDirectoryW(folder.c_str(), nullptr);
    const std::string manifest = R"({"id":"newgame","dll":"sim-qvfr-newgame.dll","games":[
        {"id":"spacesim","name":"Space Sim","short":"SPC","exe":"SpaceSim.exe","steamFolder":"Space Sim","steamAppId":"123456",
         "launchArgs":"-openxr","features":{"liveResolution":true,"helmet":true,"carLighting":true},"image":"separateViews",
         "antiCheat":["EasyAntiCheat.exe"]},
        {"id":"ats"},
        {"id":"lmu2","exe":"Le Mans Ultimate.exe"},
        {"id":"evil","exe":"..\\x.exe"},
        {"id":"farm","exe":"Farm.exe","steamFolder":"C:\\Windows","steamAppId":"12ab"}]})";
    HANDLE f = CreateFileW((folder + L"\\mod.json").c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    DWORD written = 0;
    WriteFile(f, manifest.data(), static_cast<DWORD>(manifest.size()), &written, nullptr);
    CloseHandle(f);

    std::size_t before = 0;
    games::all_games(before);
    check(games::add_mod_games_from(mods) == 2, "a mod adds its new games (not one Sim-QVFR has, nor a bad exe)");
    check(games::add_mod_games_from(mods) == 0, "read again, nothing is added twice");
    std::size_t after = 0;
    const games::GameInfo* const* all = games::all_games(after);
    check(after == before + 2 && std::strcmp(all[before]->id, "spacesim") == 0, "listed after Sim-QVFR's own games");
    const games::GameInfo* g = games::game_for_exe(L"spacesim.EXE");
    check(g && g == games::game_by_id("spacesim") && std::wcscmp(g->name, L"Space Sim") == 0 &&
              std::wcscmp(g->steamAppId, L"123456") == 0 && g->features.liveResolution && g->features.helmet &&
              !g->features.carLighting && g->qvfrImage == games::QvfrImage::SeparateViews && g->anyBuild &&
              g->antiCheat && std::wcscmp(g->antiCheat[0], L"EasyAntiCheat.exe") == 0 && !g->antiCheat[1],
          "the game's entry as mod.json gives it (a feature of one game's own code isn't offered)");
    const games::ModGame* m = games::mod_game(g);
    check(m && m->modId == L"newgame" && m->dll == L"sim-qvfr-newgame.dll", "the game is tied to its mod");
    check(!games::mod_game(&::ats::kGame) && games::game_for_exe(L"amtrucks.exe") == &::ats::kGame,
          "Sim-QVFR's own games stay its own");
    const games::GameInfo* farm = games::game_by_id("farm");
    check(farm && std::wcslen(farm->steamFolder) == 0 && std::wcslen(farm->steamAppId) == 0,
          "an absolute folder or an app id that isn't a number is dropped");

    DeleteFileW((folder + L"\\mod.json").c_str());
    RemoveDirectoryW(folder.c_str());
    RemoveDirectoryW(mods.c_str());
}

// A mod reads its own settings ([mod_<id>] of the ini SIM_QVFR_INI names) through its copy of the host table, and
// only its own: the section comes from the table's modId, never from the mod.
void mod_reads_its_settings() {
    wchar_t temp[MAX_PATH] = {};
    GetTempPathW(MAX_PATH, temp);
    const std::wstring ini = std::wstring(temp) + L"sqvfr-mod-settings-test.ini";
    DeleteFileW(ini.c_str());
    WritePrivateProfileStringW(L"mod_cool", L"sharp", L"1", ini.c_str());
    WritePrivateProfileStringW(L"mod_cool", L"look", L"warm", ini.c_str());
    WritePrivateProfileStringW(L"mods", L"cool_sha256", L"abc", ini.c_str());
    SetEnvironmentVariableW(L"SIM_QVFR_INI", ini.c_str());

    SqvfrHost host = simqvfr::mod_api::host_table();
    check(host.mod_setting && host.modId && *host.modId == '\0', "the shared host table names no mod");
    host.modId = "cool";
    char out[32] = {};
    check(host.mod_setting(&host, "sharp", out, sizeof(out)) == 1 && std::strcmp(out, "1") == 0, "a mod reads its toggle");
    check(sqvfr::setting_on(&host, "sharp", false) && sqvfr::setting_choice(&host, "look", "cool") == "warm",
          "mod.hpp's helpers read the toggle and the choice");
    check(!sqvfr::setting_on(&host, "missing", false) && sqvfr::setting_choice(&host, "missing", "def") == "def",
          "a setting not set gives the mod's default");
    check(host.mod_setting(&host, "cool_sha256", out, sizeof(out)) == -1, "a mod can't read the approvals");
    check(host.mod_setting(&host, "../x", out, sizeof(out)) == -1, "a key that isn't a setting name is refused");
    check(host.mod_setting(&host, "look", out, 3) == -1, "a buffer too small is refused");
    host.modId = "other";
    check(host.mod_setting(&host, "sharp", out, sizeof(out)) == -1, "another mod doesn't see them");
    host.modId = "";
    check(host.mod_setting(&host, "sharp", out, sizeof(out)) == -1, "a mod loaded without an id has no settings");
    SqvfrHost old = host;
    old.size = static_cast<uint32_t>(offsetof(SqvfrHost, modId));
    old.modId = "cool";
    check(!sqvfr::setting_on(&old, "sharp", false), "an older core without the service: the default");

    SetEnvironmentVariableW(L"SIM_QVFR_INI", nullptr);
    DeleteFileW(ini.c_str());
}

}  // namespace

int SIM_QVFR_TEST_MAIN() {
    refuses_other_tables();
    defaults_beyond_the_table();
    structs_round_trip();
    host_names_the_game();
    scs_through_the_tables();
    trust_rules();
    scs_dll_through_the_loader();
    host_d3d11_hooks_on_warp();
    host_table_filled();
    mod_reads_its_settings();
    sdk_example_loads();
    mod_adds_a_game();
    if (failures) {
        std::printf("mod_api_test: %d failure(s)\n", failures);
        return 1;
    }
    std::printf("mod_api_test: ok\n");
    return 0;
}
