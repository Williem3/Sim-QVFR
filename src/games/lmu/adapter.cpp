#include "adapter.h"

#include <windows.h>

#include <algorithm>

#include "../../log.h"
#include "../identity_adapter.h"
#include "four_passes.h"
#include "game.h"
#include "mvr_probe.h"
#include "view_probe.h"

namespace lmu {
namespace {

// QVFR in LMU is in development: on only with [experiment] lmu_qvfr=1 in the launch's ini (SIM_QVFR_INI, which the
// launcher sets) and mode=views4. Otherwise identity only, the layer logging, with the gates and probes below.
bool qvfr_wanted() {
    static const bool wanted = [] {
        wchar_t ini[MAX_PATH] = {};
        const DWORD n = GetEnvironmentVariableW(L"SIM_QVFR_INI", ini, MAX_PATH);
        return n > 0 && n < MAX_PATH && GetPrivateProfileIntW(L"experiment", L"lmu_qvfr", 0, ini) != 0;
    }();
    return wanted;
}

// Identity only unless lmu_qvfr=1: the layer logs, QVFR doesn't start. Its experiments run only when their
// [experiment] key asks, and never in a launch the anti-cheat protects (adapter_for hands out the plain identity
// adapter there): the view probe (log only), the copies and focus gates (four passes, docs/recon-lmu.md), and QVFR.
class LmuAdapter final : public simqvfr::games::IdentityAdapter {
public:
    LmuAdapter() : IdentityAdapter(&kGame) {}

    // ---- QVFR (lmu_qvfr=1): the eye loop four times, the focus views composed from an atlas by the D3D11 compositor ----
    bool has_qvfr() const override { return analysis() && qvfr_wanted(); }
    bool qvfr_composable() const override { return false; }     // not by the D3D12 compositor:
    bool qvfr_compose_d3d11() const override { return true; }   // LMU is D3D11
    bool qvfr_separate_views() const override { return true; }  // every view an image of its own, one size
    // LMU uses no OpenXR input (no attach in a whole session, 2026-11-07 runs): the gaze set is attached alone.
    bool attaches_action_sets() const override { return false; }
    bool qvfr_start(const simqvfr::games::QvfrStart& s) override {
        if (!has_qvfr())
            return false;
        FourPassesRun run;
        run.qvfr = true;
        run.fraction = s.focusFraction > 0 ? s.focusFraction : 0.3;
        run.fractionY = s.focusFractionY;
        run.holdExposure = !s.ini || GetPrivateProfileIntW(L"experiment", L"lmu_copies_hold_exposure", 1, s.ini) != 0;
        // Menus (frames without the eye loop) at the native eye, the UI drawn at that size (four_passes.h).
        run.menuNative = !s.ini || GetPrivateProfileIntW(L"experiment", L"lmu_menu_native", 1, s.ini) != 0;
        run.layerWidth = s.layout.layer_width();
        run.layerHeight = s.layout.layer_height();
        // lmu_mvr=1 (gate C of multi-view rendering): passes 0-1 draw their focus views too, compared with passes 2-3's
        // at the gate samples (mvr_pass.h); the layer made room (Views4Layout::secondViewRoom).
        const UINT mvrLevel = s.ini ? GetPrivateProfileIntW(L"experiment", L"lmu_mvr", 0, s.ini) : 0;
        run.mvr = mvrLevel != 0 && s.layout.secondViewRoom;
        run.mvrSkip = run.mvr && mvrLevel >= 2;  // gate D: passes 2-3 skip the scene
        if (run.mvr) {
            run.first = static_cast<int>(GetPrivateProfileIntW(L"experiment", L"gate_first", 300, s.ini));
            run.every = static_cast<int>(GetPrivateProfileIntW(L"experiment", L"gate_every", 300, s.ini));
            run.count = static_cast<int>(GetPrivateProfileIntW(L"experiment", L"gate_samples", 3, s.ini));
        }
        if (s.ini)  // diagnostic: the render scale changed live for 120 frames (four_passes.h)
            run.renderScaleProbe =
                std::clamp(static_cast<int>(GetPrivateProfileIntW(L"experiment", L"lmu_render_scale_probe", 0, s.ini)), 0, 100);
        qvfr_ = four_passes_start(run);
        // The views' launch sizes inside the layer (larger with the menus' floor, views4_menu_scale): from the first
        // frame, as a live change would set them.
        if (qvfr_)
            set_scales(simqvfr::views4_live_one_size(s.layout, s.layout.outer, s.layout.focus, s.layout.focusHeight));
        return qvfr_;
    }
    void qvfr_set_gaze(const float (&tan)[2][2], const bool (&valid)[2], bool) override {
        if (qvfr_)
            four_passes_set_gaze(tan, valid);
    }
    void qvfr_set_live(double focus, double focusY, double, bool) override {
        if (qvfr_)
            four_passes_set_box(focus, focusY);
    }
    // Live resolution: each pass at its own render scale inside the launch-size targets (four_passes.h). On for QVFR
    // runs; GameFeatures::liveResolution stays off until it passes its card.
    bool qvfr_live_resolution() const override { return has_qvfr(); }
    // `live`: views4_live_one_size's layout (each view's share of its layer, halved). LMU scales both axes together, so
    // each view takes the larger share: at least the density asked on both axes.
    void qvfr_set_live_layout(const simqvfr::Views4Layout& live) override {
        if (qvfr_)
            set_scales(live);
    }
    void qvfr_target_sizes(int (&render)[2], int (&output)[2]) override {
        four_passes_view_size(render);
        output[0] = render[0];
        output[1] = render[1];
    }
    // The FOV crop reaches LMU through the located FOV; its lens mask is built with the FOV located when it fetches the
    // mesh, so a live crop change leaves the mask off until the FOV matches again (four_passes.h).
    void qvfr_note_views(const XrView* views, uint32_t count) override {
        if (qvfr_)
            four_passes_note_views(views, count);
    }
    void visibility_mask_taken(uint32_t viewIndex) override {
        if (qvfr_)
            four_passes_mask_taken(viewIndex);
    }
    bool qvfr_take_frame(simqvfr::Views4Frame& frame) override { return qvfr_ && four_passes_record(frame); }
    bool qvfr_latest_frame(simqvfr::Views4Frame& frame) override { return qvfr_ && four_passes_record(frame); }
    ID3D11Texture2D* qvfr_views_atlas(ID3D11Texture2D* const (&eyes)[2], float (&uv)[4][4]) override {
        return qvfr_ ? four_passes_atlas(eyes, uv) : nullptr;
    }
    void qvfr_frame_end() override {
        if (qvfr_)
            four_passes_frame_end();
    }

    void instance_created(const wchar_t* ini, const wchar_t* mode) override {
        IdentityAdapter::instance_created(ini, mode);
        if (!analysis())
            return;
        viewProbe_ = GetPrivateProfileIntW(L"experiment", L"lmu_view_probe", 0, ini) != 0 && view_probe_start();
        // lmu_mvr_probe=1: every vertex shader converted for NVIDIA's two-view pass and created through NVAPI, log only.
        // lmu_mvr=1 (with lmu_qvfr): the probe keeps the converted shaders, attached to the game's (mvr_pass.h).
        const bool mvr = has_qvfr() && GetPrivateProfileIntW(L"experiment", L"lmu_mvr", 0, ini) != 0;
        mvrProbe_ = (mvr || GetPrivateProfileIntW(L"experiment", L"lmu_mvr_probe", 0, ini) != 0) && mvr_probe_install(ini, mvr);
        const bool focus = GetPrivateProfileIntW(L"experiment", L"lmu_focus", 0, ini) != 0;
        const bool copies = GetPrivateProfileIntW(L"experiment", L"lmu_copies", 0, ini) != 0;
        if (has_qvfr()) {
            if (focus || copies)
                simqvfr::log_line("lmu: lmu_qvfr=1: the copies and focus gates don't run (lmu_copies, lmu_focus ignored)");
            return;
        }
        // lmu_focus=1: the focus gate (focus views); lmu_copies=1: the copies gate. Both run the eye loop four times.
        FourPassesRun run;
        run.focus = focus;
        run.holdExposure = GetPrivateProfileIntW(L"experiment", L"lmu_copies_hold_exposure", 1, ini) != 0;
        run.fraction = GetPrivateProfileIntW(L"experiment", L"focus_percent", 30, ini) / 100.0;
        run.first = static_cast<int>(GetPrivateProfileIntW(L"experiment", L"gate_first", 300, ini));
        run.every = static_cast<int>(GetPrivateProfileIntW(L"experiment", L"gate_every", 300, ini));
        run.count = static_cast<int>(GetPrivateProfileIntW(L"experiment", L"gate_samples", 3, ini));
        run.warmup = static_cast<int>(GetPrivateProfileIntW(L"experiment", L"gate_warmup", 60, ini));
        run.exitWhenDone = GetPrivateProfileIntW(L"experiment", L"exit_when_done", 0, ini) != 0;
        gates_ = (focus || copies) && four_passes_start(run);
    }
    void d3d11_session(ID3D11Device* device) override {
        if (mvrProbe_)
            mvr_probe_device(device);
    }
    void frame_end() override {
        if (mvrProbe_)
            mvr_probe_frame_end();
        if (viewProbe_)
            view_probe_frame_end();
        if (gates_)
            four_passes_frame_end();
    }

private:
    static void set_scales(const simqvfr::Views4Layout& live) {
        const auto share = [&](int v) { return std::max(2 * live.width[v], 2 * live.height[v]); };
        four_passes_set_live_scales(share(0), share(2));
    }
    bool viewProbe_ = false;
    bool mvrProbe_ = false;
    bool gates_ = false;
    bool qvfr_ = false;
};

}  // namespace

simqvfr::games::GameAdapter& adapter() {
    static LmuAdapter a;
    return a;
}

}  // namespace lmu
