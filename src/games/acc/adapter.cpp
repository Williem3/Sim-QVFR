#include "adapter.h"

#include <windows.h>

#include "../../log.h"
#include "../diag_switches.h"
#include "game.h"
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
    bool qvfr_eye_columns() const override { return true; }  // instanced stereo; OpenComposite copies each half

    bool qvfr_start(const simqvfr::games::QvfrStart& s) override {
        return views_start(s.layout, s.focusFraction, s.focusFractionY);
    }
    // No focus cull yet. Live resolution: the views laid out again in the launch target (views.cpp).
    void qvfr_set_gaze(const float (&tan)[2][2], const bool (&valid)[2], bool) override { views_set_gaze(tan, valid); }
    void qvfr_set_live_layout(const Views4Layout& live) override { views_set_live_layout(live); }
    void qvfr_set_live(double focus, double focusY, double, bool) override { views_set_focus(focus, focusY); }
    bool qvfr_dlss_held() override { return false; }
    void qvfr_target_sizes(int (&render)[2], int (&output)[2]) override {
        views_target(render);
        output[0] = render[0];
        output[1] = render[1];
    }
    bool qvfr_take_frame(Views4Frame& frame) override { return views_take_frame(frame); }
    bool qvfr_latest_frame(Views4Frame& frame) override { return views_latest_frame(frame); }
    void qvfr_note_views(const XrView*, uint32_t) override {}
    void qvfr_frame_end() override {
        diag_poll();
        views_frame_end();
        view_share_frame_end();
    }
    void session_running(bool running) override { views_session_running(running); }
    void instance_created(const wchar_t* ini, const wchar_t*) override {
        view_share_configure(GetPrivateProfileIntW(L"experiment", L"acc_ssr_share", 1, ini) != 0,
                             GetPrivateProfileIntW(L"experiment", L"acc_ao_share", 1, ini) != 0);
        // Diagnostic switches (acc_diag=1): Ctrl+Alt+1 SSR, 2 ambient occlusion, 3 the sun's shadows, 4 the focus
        // views' sharing of their eye's SSR and AO.
        if (diag_.start(ini, L"acc", {L"ssr", L"ao", L"shadow", L"share"}) && views_diag_start())
            simqvfr::logf("acc diagnostics: switches on (Ctrl+Alt+1 SSR, 2 AO, 3 sun shadows, 4 view share), the ini's "
                          "read from %ls",
                          diag_.ini().c_str());
    }
    void d3d11_session(ID3D11Device* device) override { view_share_start(device); }
    void session_destroyed() override { view_share_stop(); }

private:
    simqvfr::games::DiagSwitches diag_;  // acc_diag=1
    void diag_poll() {
        if (!diag_.poll())
            return;
        for (int k = 0; k < 3; ++k)
            views_set_diag(k, diag_.off(k));
        view_share_set_off(diag_.off(3));
        simqvfr::logf("acc diagnostics: SSR %s, AO %s, sun shadows %s, view share %s (from the next frame)",
                      diag_.off(0) ? "OFF" : "on", diag_.off(1) ? "OFF" : "on", diag_.off(2) ? "OFF" : "on",
                      diag_.off(3) ? "OFF" : "on");
    }
};

}  // namespace

simqvfr::games::GameAdapter& adapter() {
    static AccAdapter a;
    return a;
}

}  // namespace acc
