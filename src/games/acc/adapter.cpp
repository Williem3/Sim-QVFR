#include "adapter.h"

#include <windows.h>

#include "game.h"
#include "view_share.h"
#include "views.h"

namespace acc {
namespace {

using aceqv::Views4Frame;
using aceqv::Views4Layout;

class AccAdapter final : public aceqv::games::GameAdapter {
public:
    const aceqv::games::GameInfo* info() const override { return &kGame; }
    bool qvfr_composable() const override { return false; }    // not by the D3D12 compositor:
    bool qvfr_compose_d3d11() const override { return true; }  // ACC is D3D11
    bool qvfr_eye_columns() const override { return true; }  // instanced stereo; OpenComposite copies each half

    bool qvfr_start(const aceqv::games::QvfrStart& s) override {
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
        views_frame_end();
        view_share_frame_end();
    }
    void session_running(bool running) override { views_session_running(running); }
    void instance_created(const wchar_t* ini, const wchar_t*) override {
        view_share_configure(GetPrivateProfileIntW(L"experiment", L"acc_ssr_share", 1, ini) != 0,
                             GetPrivateProfileIntW(L"experiment", L"acc_ao_share", 1, ini) != 0);
    }
    void d3d11_session(ID3D11Device* device) override { view_share_start(device); }
    void session_destroyed() override { view_share_stop(); }
};

}  // namespace

aceqv::games::GameAdapter& adapter() {
    static AccAdapter a;
    return a;
}

}  // namespace acc
