#include "adapter.h"

#include <windows.h>

#include <atomic>
#include <string>

#include "../../engine/code_patch.h"
#include "../../log.h"
#include "../diag_switches.h"
#include "abi.h"
#include "four_views.h"
#include "game.h"

namespace xplane {
namespace {

using simqvfr::Views4Frame;
using simqvfr::Views4Layout;

// [experiment] xplane_layers = 3 or 4: the render size query reports that many layers instead of 2, so
// REN_graph_info and the per-eye targets get them. A measurement for QVFR: a frame capture then shows which counts,
// targets and draws follow (docs/recon-xplane.md). 0 or 2: X-Plane as it is. True: the count was patched.
bool apply_layer_count(const wchar_t* ini, int forced = 0) {
    const int layers = forced ? forced : GetPrivateProfileIntW(L"experiment", L"xplane_layers", 0, ini);
    if (layers == 0 || layers == 2)
        return false;
    if (layers < 1 || layers > 4) {
        simqvfr::logf("xplane: xplane_layers=%d ignored (1-4)", layers);
        return false;
    }
    if (!simqvfr::code_matches(kVrLayerCount, kVrLayerCountAnchor, sizeof(kVrLayerCountAnchor))) {
        simqvfr::logf("xplane: xplane_layers=%d not applied: the layer count at 0x%llx isn't this build's", layers,
                      static_cast<unsigned long long>(kVrLayerCount));
        return false;
    }
    const auto value = static_cast<std::uint8_t>(layers);
    const bool ok = simqvfr::code_write(kVrLayerCount + kVrLayerCountImm, &value, 1);
    simqvfr::logf("xplane: VR layer count 2 -> %d %s (render size query, 0x%llx)", layers, ok ? "patched" : "FAILED",
                  static_cast<unsigned long long>(kVrLayerCount));
    return ok;
}

// [experiment] xplane_context_layers = 3 or 4: the VR render context is made with that many layers, its stereo test
// becomes layers >= 2 (it is layers == 2, which would turn the context mono), the camera still takes two views'
// tangents (its room), the 3D world subgraph gives its targets that many layers, and the two VR-context resizes keep
// that many (they passed 2, which also rebuilt its lit-image miptree with two layers). 0 or 2: X-Plane as it is. All
// patches or none (true: all went in).
bool apply_context_layers(const wchar_t* ini, int forced = 0) {
    const int layers = forced ? forced : GetPrivateProfileIntW(L"experiment", L"xplane_context_layers", 0, ini);
    if (layers == 0 || layers == 2)
        return false;
    if (layers < 3 || layers > 4) {
        simqvfr::logf("xplane: xplane_context_layers=%d ignored (3-4)", layers);
        return false;
    }
    if (!simqvfr::code_matches(kVrContextLayers, kVrContextLayersAnchor, sizeof(kVrContextLayersAnchor)) ||
        !simqvfr::code_matches(kStereoTestCmp, kStereoTestCmpAnchor, sizeof(kStereoTestCmpAnchor)) ||
        !simqvfr::code_matches(kStereoTestSet, kStereoTestSetAnchor, sizeof(kStereoTestSetAnchor)) ||
        !simqvfr::code_matches(kTangentCopyBound, kTangentCopyBoundAnchor, sizeof(kTangentCopyBoundAnchor)) ||
        !simqvfr::code_matches(kWorldSubgraphLayers, kWorldSubgraphLayersAnchor, sizeof(kWorldSubgraphLayersAnchor)) ||
        !simqvfr::code_matches(kVrContextResizeA, kVrContextResizeAnchor, sizeof(kVrContextResizeAnchor)) ||
        !simqvfr::code_matches(kVrContextResizeB, kVrContextResizeAnchor, sizeof(kVrContextResizeAnchor))) {
        simqvfr::logf("xplane: xplane_context_layers=%d not applied: the VR context code isn't this build's", layers);
        return false;
    }
    const auto value = static_cast<std::uint8_t>(layers);
    const bool ok = simqvfr::code_write(kTangentCopyBound, kTangentCopyBoundTwo, sizeof(kTangentCopyBoundTwo)) &&
                    simqvfr::code_write(kStereoTestSet, kStereoTestSetGe, sizeof(kStereoTestSetGe)) &&
                    simqvfr::code_write(kVrContextLayers + kVrContextLayersImm, &value, 1) &&
                    simqvfr::code_write(kWorldSubgraphLayers + kWorldSubgraphLayersImm, &value, 1) &&
                    simqvfr::code_write(kVrContextResizeA + kVrContextResizeImm, &value, 1) &&
                    simqvfr::code_write(kVrContextResizeB + kVrContextResizeImm, &value, 1);
    simqvfr::logf("xplane: VR context (made and resized) and world subgraph layers 2 -> %d, stereo test layers >= 2, "
                  "camera tangents kept at 2: %s",
                  layers,
                  ok ? "patched" : "FAILED");
    return ok;
}

// [experiment] xplane_copies = 1: X-Plane draws four views, slots 2-3 copies of the eyes, and its final resolve shows
// layers 2-3 (four_views.h). Includes every four-layer patch (xplane_layers and xplane_context_layers at 4). A normal
// headset image: four views rendered. The stage gate before the focus cameras. True: a four-layer patch went in
// (X-Plane then makes four-layer targets).
bool apply_copies(const wchar_t* ini) {
    if (GetPrivateProfileIntW(L"experiment", L"xplane_copies", 0, ini) != 1)
        return false;
    const bool layers = apply_layer_count(ini, 4);
    const bool context = apply_context_layers(ini, 4);
    four_views_copies_start();
    // [experiment] xplane_focus = 1: views 2-3 render focus boxes of focus_percent (default 20) of each eye.
    if (GetPrivateProfileIntW(L"experiment", L"xplane_focus", 0, ini) == 1) {
        const int percent = GetPrivateProfileIntW(L"experiment", L"focus_percent", 20, ini);
        const double fraction = (percent > 0 && percent <= 100 ? percent : 20) / 100.0;
        four_views_set_focus(true, fraction, fraction);
    }
    return layers || context;
}

// True when the exe's `rva` holds a float inside its image, in committed read-write (or write-copy) data.
bool writable_image_float(std::uintptr_t rva) {
    const std::uintptr_t base = simqvfr::module_base();
    if (!base)
        return false;
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE)
        return false;
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE || rva + sizeof(float) > nt->OptionalHeader.SizeOfImage)
        return false;
    const std::uintptr_t at = base + rva;
    MEMORY_BASIC_INFORMATION mbi = {};
    if (VirtualQuery(reinterpret_cast<const void*>(at), &mbi, sizeof(mbi)) != sizeof(mbi))
        return false;
    const DWORD protect = mbi.Protect & 0xFF;
    const auto end = reinterpret_cast<std::uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
    return mbi.State == MEM_COMMIT && mbi.Type == MEM_IMAGE && !(mbi.Protect & PAGE_GUARD) &&
           (protect == PAGE_READWRITE || protect == PAGE_WRITECOPY) && at + sizeof(float) <= end;
}

// QVFR as the ini asks for it: [experiment] mode=views4 with views4_compose=1 (the launcher's QVFR). Read at the early
// load, before X-Plane creates its shaders: the final resolve's edit depends on it.
bool qvfr_wanted(const wchar_t* ini) {
    wchar_t mode[32] = {};
    GetPrivateProfileStringW(L"experiment", L"mode", L"off", mode, 32, ini);
    return _wcsicmp(mode, L"views4") == 0 && GetPrivateProfileIntW(L"experiment", L"views4_compose", 0, ini) == 1 &&
           GetPrivateProfileIntW(L"experiment", L"baseline", 0, ini) == 0;
}

// QVFR: four views (layers, contexts, multiplier, copies stage), focus cameras at focus_percent, the final resolve and
// view heights in eye columns. False: some part isn't this build's (logged); X-Plane may then render oddly, so the
// layer stays off and the headset shows what X-Plane submits. `fourLayers`: a four-layer patch went in.
bool apply_qvfr(const wchar_t* ini, bool& fourLayers) {
    // Stage 1 of per-view sizes: one viewport per view, every rect the whole layer (the image must not change).
    four_views_set_view_rects(GetPrivateProfileIntW(L"experiment", L"xplane_view_rects", 1, ini) == 1);
    four_views_set_focus_full_rate(GetPrivateProfileIntW(L"experiment", L"xplane_focus_full_rate", 1, ini) == 1);
    four_views_set_bloom_from_eye(GetPrivateProfileIntW(L"experiment", L"xplane_bloom_from_eye", 1, ini) == 1);
    four_views_set_periphery_alpha_lod(GetPrivateProfileIntW(L"experiment", L"xplane_periphery_alpha_lod", 1, ini) == 1);
    four_views_set_ssr_from_eye(GetPrivateProfileIntW(L"experiment", L"xplane_ssr_from_eye", 1, ini) == 1);
    four_views_set_ao_edges_from_eye(GetPrivateProfileIntW(L"experiment", L"xplane_ao_edges_from_eye", 1, ini) == 1);
    four_views_set_focus_lod(GetPrivateProfileIntW(L"experiment", L"xplane_focus_lod", 1, ini) == 1);
    // The launcher's Object detail (live from then on: qvfr_set_object_detail).
    four_views_set_object_detail(static_cast<int>(GetPrivateProfileIntW(L"experiment", L"object_detail", 100, ini)));
    // [experiment] xplane_diag=1: SSR, CACAO and the sun's shadow switchable live (Ctrl+Alt+1/2/3, or the ini's
    // xplane_diag_ssr_off, _ao_off, _shadow_off).
    four_views_set_diag_edits(GetPrivateProfileIntW(L"experiment", L"xplane_diag", 0, ini) == 1);
    four_views_set_gaze_follow(GetPrivateProfileIntW(L"experiment", L"xplane_gaze", 1, ini) == 1);
    const bool layers = apply_layer_count(ini, 4);
    const bool context = apply_context_layers(ini, 4);
    fourLayers = layers || context;
    // The draw path only on four layers, and taken out again if the eye columns fail: a half-patched QVFR would show
    // the focus views (layers 2-3) as the whole image.
    bool ok = layers && context && four_views_copies_start();
    if (ok && !four_views_eye_columns_start()) {
        four_views_copies_undo();
        ok = false;
    }
    // Before X-Plane makes its shaders: the final resolve's edit takes it.
    four_views_set_eye_columns(ok);
    const int percent = GetPrivateProfileIntW(L"experiment", L"focus_percent", 20, ini);
    const double fraction = (percent > 0 && percent <= 100 ? percent : 20) / 100.0;
    four_views_set_focus(true, fraction, fraction);
    simqvfr::logf("xplane: QVFR %s (eye columns, D3D11 compositor)", ok ? "patched in" : "NOT fully patched in");
    return ok;
}

class XPlaneAdapter final : public simqvfr::games::GameAdapter {
public:
    const simqvfr::games::GameInfo* info() const override { return &kGame; }
    // QVFR runs only when the early load patched it in (qvfr_start); otherwise the layer stays off.
    bool qvfr_composable() const override { return false; }    // not by the D3D12 compositor:
    bool qvfr_compose_d3d11() const override { return true; }  // X-Plane submits D3D11 (--d3d11_vr)
    bool qvfr_eye_columns() const override { return true; }    // one side-by-side image, each eye a column
    bool qvfr_one_view_size() const override { return true; }  // four instanced layers, one viewport
    // A flight load draws no frame for 30-60 s (the hang watch fired on every one, 2026-11-06).
    unsigned hang_watch_s() const override { return 120; }
    bool qvfr_start(const simqvfr::games::QvfrStart& s) override {
        if (!qvfr_) {
            simqvfr::logf("xplane: QVFR not started: it needs the launcher's early load with mode=views4 and "
                          "views4_compose=1 (the shaders are edited as X-Plane starts)");
            return false;
        }
        if (s.focusFraction > 0)
            four_views_set_focus(true, s.focusFraction, s.focusFractionY > 0 ? s.focusFractionY : s.focusFraction);
        const Views4Layout& l = s.layout;
        // Per-view sizes (on unless [experiment] xplane_view_sizes=0): each kind of view renders at its own size inside
        // the layers, which live resolution needs. Some screen-space passes still read whole layers
        // (docs/backlog/xplane-bugs.md).
        viewSizes_ = !s.ini || GetPrivateProfileIntW(L"experiment", L"xplane_view_sizes", 1, s.ini) == 1;
        four_views_set_view_sizes(viewSizes_, l.outer_width(), l.outer_height(), l.focus, l.focusHeight);
        simqvfr::logf("xplane: QVFR started; launch layout outer %.2f x %.2f, focus %.2f x %.2f of the native eye (X-Plane "
                      "renders every view at half the column: the records say so)",
                      l.outer_width(), l.outer_height(), l.focus, l.focusHeight);
        return true;
    }
    // The boxes move with it from the next frame (four_views_latch_frame at xrEndFrame).
    void qvfr_set_gaze(const float (&tan)[2][2], const bool (&valid)[2], bool) override { four_views_set_gaze(tan, valid); }
    // Live resolution (views4_live_one_size): each kind of view's rect, half its share of the layers (the target is two
    // layers wide and high), from the next frame's latch. The layers keep their launch size, so nothing is remade.
    void qvfr_set_live_layout(const Views4Layout& live) override {
        if (!four_views_set_view_shares(2 * live.width[0], 2 * live.height[0], 2 * live.width[2], 2 * live.height[2]))
            simqvfr::logf("xplane: live resolution needs per-view sizes (xplane_view_sizes=1): the views keep the whole "
                          "layer");
        else
            simqvfr::logf("xplane: live view rects: outer views %.0f%% x %.0f%% of their layers, focus views %.0f%% x "
                          "%.0f%% (from the next frame)",
                          200 * live.width[0], 200 * live.height[0], 200 * live.width[2], 200 * live.height[2]);
    }
    // Views resize live only inside the launch layers, with per-view sizes on.
    bool qvfr_live_resolution() const override { return viewSizes_; }
    void qvfr_set_live(double focus, double focusY, double, bool) override {
        if (focus > 0 && (focus != focus_ || focusY != focusY_)) {
            focus_ = focus;
            focusY_ = focusY;
            four_views_set_focus(true, focus, focusY > 0 ? focusY : focus);
        }
    }
    void qvfr_set_object_detail(int percent) override { four_views_set_object_detail(percent); }
    bool qvfr_dlss_held() override { return false; }
    void qvfr_target_sizes(int (&render)[2], int (&output)[2]) override {
        render[0] = render[1] = output[0] = output[1] = 0;
    }
    // Each frame's record: the tangents last drawn with, every view half the column (eye columns, frame.live).
    bool qvfr_take_frame(Views4Frame& frame) override { return record(frame); }
    bool qvfr_latest_frame(Views4Frame& frame) override { return record(frame); }
    void qvfr_note_views(const XrView*, uint32_t) override {}
    // The final resolve's whole-column viewport, counted once the flight's final resolve exists: 300 frames after it,
    // then every 1800, with the viewports X-Plane set (diagnostics).
    void qvfr_frame_end() override {
        diag_poll();
        four_views_latch_frame();
        // Eyes only: no four-view draw since the last frame end (X-Plane's menu draws its eyes straight into the
        // image).
        const std::uint64_t draws = four_views_draws();
        const bool eyesOnly = draws == lastDraws_;
        lastDraws_ = draws;
        if (eyesOnly != eyesOnly_.load())
            simqvfr::logf("xplane: %s", eyesOnly ? "frames with the eyes only (the menu): submitted uncomposed"
                                                 : "four views drawn again: composed");
        eyesOnly_ = eyesOnly;
        if (!four_views_final_resolve_made())
            return;
        ++frames_;
        if (frames_ == 300 || frames_ % 1800 == 0) {
            const int n = four_views_column_viewports();
            simqvfr::logf("xplane: final resolve viewport given the whole column %d times over %d frames since the flight's "
                          "final resolve was made%s",
                          n, frames_, n > 0 ? "" : " (NOT seen: the lower row, the focus views, stays undrawn)");
            four_views_viewport_log();
            four_views_vegetation_log();
            simqvfr::logf("xplane: set viewports given one rect per view %d times so far", four_views_view_rect_sets());
            four_views_gaze_log();
        }
    }

    // Loaded early (the launcher, GameInfo::earlyLoad): the patches go in at X-Plane's first vkCreateInstance, before
    // it makes any shader module, so the shader wrapper sees every module. xrCreateInstance then finds them in.
    void early_start(const wchar_t* ini) override {
        simqvfr::logf("xplane: layer loaded early (at X-Plane's Vulkan start): patching now");
        patched_ = true;
        if (qvfr_wanted(ini)) {
            bool fourLayers = false;
            qvfr_ = apply_qvfr(ini, fourLayers);
            // Diagnostic switches (xplane_diag=1): SSR, AO and the sun's shadow (four_views_set_diagnostics).
            if (diag_.start(ini, L"xplane", {L"ssr", L"ao", L"shadow"}))
                simqvfr::logf("xplane: diagnostic switches read from %ls", diag_.ini().c_str());
            fourViews_ = fourLayers;
            return;
        }
        fourViews_ = apply_all(ini);
    }
    // Every xrEndFrame: with four views, exposure fusion stays off (it builds its pyramids for two layers only; abi.h).
    // X-Plane's static initialisers set the control after an early load, so it is checked here, not once. Only after a
    // four-layer patch went in (its anchors matched: this build), at an address checked once as the exe's writable data.
    void frame_end() override {
        if (!fourViews_)
            return;
        if (!fusionChecked_) {
            fusionChecked_ = true;
            fusionOk_ = writable_image_float(kExposureFusionControl);
            if (!fusionOk_)
                simqvfr::logf("xplane: exposure fusion control at 0x%llx isn't writable data of this exe: left alone",
                              static_cast<unsigned long long>(kExposureFusionControl));
        }
        if (!fusionOk_)
            return;
        auto* control = reinterpret_cast<volatile float*>(simqvfr::module_base() + kExposureFusionControl);
        if (*control != 0.0f) {
            simqvfr::logf("xplane: exposure fusion %.2f -> 0 (two-layer pyramids; four views)", static_cast<double>(*control));
            *control = 0.0f;
        }
    }
    // At xrCreateInstance: X-Plane builds its render graph later, when the flight loads.
    void instance_created(const wchar_t* ini, const wchar_t*) override {
        if (patched_) {
            simqvfr::logf("xplane: already patched (at the early load or an earlier instance)");
            return;
        }
        simqvfr::logf("xplane: layer loaded at xrCreateInstance (not early): shaders X-Plane made at startup are not edited");
        // Once per process: a later instance would find the sites patched (anchors gone) and lose the first result.
        patched_ = true;
        fourViews_ = apply_all(ini);
    }

private:
    // Diagnostics (xplane_diag=1; games/diag_switches.h): Ctrl+Alt+1 SSR's reflections, Ctrl+Alt+2 CACAO's occlusion,
    // Ctrl+Alt+3 the sun's cascaded shadows, from the next frame.
    void diag_poll() {
        if (diag_.poll())
            four_views_set_diagnostics(diag_.off(0), diag_.off(1), diag_.off(2));
    }

    bool patched_ = false;
    bool fourViews_ = false;      // a four-layer patch went in: exposure fusion kept off
    bool fusionChecked_ = false;  // the control's address validated (writable_image_float), once
    bool fusionOk_ = false;
    bool qvfr_ = false;
    bool viewSizes_ = false;  // per-view sizes on (xplane_view_sizes=1, at QVFR's start)
    simqvfr::games::DiagSwitches diag_;  // xplane_diag=1
    double focus_ = 0, focusY_ = 0;
    int frames_ = 0;
    std::uint64_t lastDraws_ = 0;
    static inline std::atomic<bool> eyesOnly_{true};
    static bool record(Views4Frame& frame) {
        frame = {};
        if (eyesOnly_.load()) {
            frame.eyesOnly = true;
            return true;
        }
        if (!four_views_tangents(frame.outerTan, frame.focusTan))
            return false;
        for (int e = 0; e < 2; ++e)
            frame.turn[e] = {0, 0, 0, 1};
        frame.live = true;  // views4_rect_eye_columns: each view half the target wide, each row half its height
        for (int v = 0; v < 4; ++v) {
            frame.width[v] = frame.renderWidth[v] = 0.5;
            frame.height[v] = frame.renderHeight[v] = 0.5;
        }
        return true;
    }
    // The experiment stages (no QVFR composition). True: X-Plane draws four views (xplane_copies=1). A baseline run
    // (baseline=1, the launcher's "Next launch without QVFR") patches nothing: X-Plane's own two views, to compare with.
    static bool apply_all(const wchar_t* ini) {
        if (GetPrivateProfileIntW(L"experiment", L"baseline", 0, ini) != 0) {
            simqvfr::logf("xplane: baseline=1: nothing patched, X-Plane renders its own two views");
            return false;
        }
        if (GetPrivateProfileIntW(L"experiment", L"xplane_copies", 0, ini) == 1)
            return apply_copies(ini);
        apply_layer_count(ini);
        apply_context_layers(ini);
        return false;
    }
};

}  // namespace

simqvfr::games::GameAdapter& adapter() {
    static XPlaneAdapter a;
    return a;
}

}  // namespace xplane
