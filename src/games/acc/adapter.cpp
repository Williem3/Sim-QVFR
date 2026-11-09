#include "adapter.h"

#include <windows.h>

#include <atomic>

#include "../../log.h"
#include "../diag_switches.h"
#include "fixation.h"
#include "game.h"
#include "gates.h"
#include "view_share.h"
#include "views.h"

namespace acc {
namespace {

using simqvfr::Views4Frame;
using simqvfr::Views4Layout;

class AccAdapter final : public simqvfr::games::GameAdapter {
public:
    const simqvfr::games::GameInfo* info() const override { return &kGame; }
    bool qvfr_composable() const override { return false; }    // not by the D3D12 compositor:
    bool qvfr_compose_d3d11() const override { return true; }  // ACC is D3D11
    bool qvfr_eye_columns() const override { return true; }  // instanced stereo pairs, each its own width

    bool qvfr_start(const simqvfr::games::QvfrStart& s) override {
        // The copies gate: views 2-3 the eyes' cameras (no box), at the eyes' size (qvfr_copies_layout).
        const double box = gates_.copies ? 0.0 : s.focusFraction;
        return views_start(s.layout, box, s.focusFractionY, fixes_);
    }
    // No focus cull yet. Live resolution: the views laid out again in the launch target (views.cpp).
    void qvfr_set_gaze(const float (&tan)[2][2], const bool (&valid)[2], bool) override { views_set_gaze(tan, valid); }
    // The copies gate keeps its launch layout (views 2-3 at the eyes' size): no live resolution then.
    bool qvfr_live_resolution() const override { return !gates_.copies && GameAdapter::qvfr_live_resolution(); }
    void qvfr_set_live_layout(const Views4Layout& live) override {
        if (gates_.copies) {
            if (!liveIgnoredLogged_)
                simqvfr::log_line("acc copies: live layouts ignored (the gate keeps views 2-3 at the eyes' size)");
            liveIgnoredLogged_ = true;
            return;
        }
        views_set_live_layout(live);
    }
    void qvfr_set_live(double focus, double focusY, double, bool) override {
        if (!gates_.copies)
            views_set_focus(focus, focusY);
    }
    bool qvfr_dlss_held() override { return false; }
    void qvfr_target_sizes(int (&render)[2], int (&output)[2]) override {
        views_target(render);
        output[0] = render[0];
        output[1] = render[1];
    }
    bool qvfr_take_frame(Views4Frame& frame) override { return views_take_frame(frame); }
    bool qvfr_latest_frame(Views4Frame& frame) override { return views_latest_frame(frame); }
    void qvfr_note_views(const XrView*, uint32_t) override {}
    bool qvfr_copies_layout(const wchar_t* ini) const override { return gate_run(ini).copies; }
    bool qvfr_wants_game_image() const override { return gates_on(); }
    void qvfr_game_image(ID3D11Texture2D* image, const int (&rects)[4][4]) override { gates_image(image, rects); }
    // The fixation distance (fixation.h), unless switch 6 (acc_diag) turned the convergence off: the boxes parallel.
    double qvfr_fixation_m() const override { return convergeOff_.load(std::memory_order_relaxed) ? 0.0 : fixation_m(); }
    void qvfr_frame_end() override {
        diag_poll();
        views_frame_end();
        view_share_frame_end();
        fixation_frame_end();
    }
    void session_running(bool running) override { views_session_running(running); }
    void instance_created(const wchar_t* ini, const wchar_t*) override {
        fixes_ = views_fixes(ini);
        gates_ = gate_run(ini);
        gates_start(gates_);
        view_share_configure(GetPrivateProfileIntW(L"experiment", L"acc_ssr_share", 1, ini) != 0,
                             GetPrivateProfileIntW(L"experiment", L"acc_ao_share", 1, ini) != 0,
                             GetPrivateProfileIntW(L"experiment", L"acc_bloom_share", 1, ini) != 0);
        // Diagnostic switches (acc_diag=1): Ctrl+Alt+1 SSR, 2 ambient occlusion, 3 the sun's shadows, 4 the focus
        // views' sharing of their eye's SSR, AO and bloom, 5 contact shadows, 6 the focus boxes' convergence, 7 the
        // bloom share alone.
        if (diag_.start(ini, L"acc", {L"ssr", L"ao", L"shadow", L"share", L"contact", L"converge", L"bloomshare"}) &&
            views_diag_start())
            simqvfr::logf("acc diagnostics: switches on (Ctrl+Alt+1 SSR, 2 AO, 3 sun shadows, 4 view share, 5 contact "
                          "shadows, 6 box convergence, 7 bloom share), the ini's read from %ls",
                          diag_.ini().c_str());
    }
    void d3d11_session(ID3D11Device* device) override {
        view_share_start(device);
        fixation_start(device);
    }
    void session_destroyed() override {
        fixation_stop();
        view_share_stop();
    }

private:
    ViewsFixes fixes_;                    // [experiment] acc_* fixes (views.h)
    GateRun gates_;                       // acc_copies / acc_focus (gates.h)
    bool liveIgnoredLogged_ = false;
    std::atomic<bool> convergeOff_{false};  // acc_diag switch 6
    simqvfr::games::DiagSwitches diag_;  // acc_diag=1
    void diag_poll() {
        if (!diag_.poll())
            return;
        for (int k = 0; k < 3; ++k)
            views_set_diag(k, diag_.off(k));
        view_share_set_off(diag_.off(3), diag_.off(6));
        // Contact shadows (abi.h kDiagCvars[3]): switch 5 flips the launch setting (held off by acc_contact_shadows_off).
        const bool contactOff = views_contact_shadows_held() != diag_.off(4);
        views_set_diag(3, contactOff);
        convergeOff_.store(diag_.off(5), std::memory_order_relaxed);
        simqvfr::logf("acc diagnostics: SSR %s, AO %s, sun shadows %s, view share %s, contact shadows %s, box convergence %s, "
                      "bloom share %s (from the next frame)",
                      diag_.off(0) ? "OFF" : "on", diag_.off(1) ? "OFF" : "on", diag_.off(2) ? "OFF" : "on",
                      diag_.off(3) ? "OFF" : "on", contactOff ? "OFF" : "on", diag_.off(5) ? "OFF (parallel)" : "on",
                      diag_.off(6) ? "OFF" : "on");
    }
};

}  // namespace

simqvfr::games::GameAdapter& adapter() {
    static AccAdapter a;
    return a;
}

}  // namespace acc
