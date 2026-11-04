#include "adapter.h"

#include <windows.h>

#include <cwchar>
#include <iterator>

#include "../../log.h"
#include "batch_watch.h"
#include "double_render.h"
#include "game.h"
#include "views3.h"
#include "views4.h"

namespace ace {
namespace {

using aceqv::Views4Frame;
using aceqv::Views4Layout;

class AceAdapter final : public aceqv::games::GameAdapter {
public:
    const aceqv::games::GameInfo* info() const override { return &kGame; }

    // views4 (views4.h), with ACE's own diagnostics from the ini (manifest/ace-quadview.ini documents them).
    bool qvfr_start(const aceqv::games::QvfrStart& s) override {
        const wchar_t* ini = s.ini ? s.ini : L"";
        wchar_t callers[512] = L"init", borrow[256] = L"";
        GetPrivateProfileStringW(L"experiment", L"views4_callers", L"init", callers, static_cast<DWORD>(std::size(callers)), ini);
        GetPrivateProfileStringW(L"experiment", L"views4_borrow", L"", borrow, static_cast<DWORD>(std::size(borrow)), ini);
        const bool eyesOnlyAdvance = GetPrivateProfileIntW(L"experiment", L"views4_eyes_only_advance", 0, ini) != 0;
        const bool shadowView1Plain = GetPrivateProfileIntW(L"experiment", L"views4_shadow_view1_plain", 0, ini) != 0;
        const bool screenSizeLegacy = GetPrivateProfileIntW(L"experiment", L"views4_screen_size_legacy", 0, ini) != 0;
        aceqv::views4_set_borrow(borrow);
        aceqv::views4_set_layout(s.layout);
        aceqv::views4_set_screen_size_legacy(screenSizeLegacy);
        aceqv::views4_set_dlss_live(s.dlssLive);
        aceqv::views4_set_focus_cull(s.focusCull, s.focusCullScale, s.focusCullGaze);
        if (!aceqv::views4_start(callers, eyesOnlyAdvance, s.focusFraction, s.focusFractionY))
            return false;
        if (shadowView1Plain)
            aceqv::views4_shadow_view1_plain();
        return true;
    }
    void qvfr_set_gaze(const float (&tan)[2][2], const bool (&valid)[2], bool gazeSample) override {
        aceqv::views4_set_gaze(tan, valid, gazeSample);
    }
    void qvfr_set_live_layout(const Views4Layout& live) override { aceqv::views4_set_live_layout(live); }
    void qvfr_set_live(double focusFraction, double focusFractionY, double cullScale, bool gaze) override {
        aceqv::views4_set_live(focusFraction, focusFractionY, cullScale, gaze);
    }
    bool qvfr_dlss_held() override { return aceqv::views4_dlss_held(); }
    void qvfr_target_sizes(int (&render)[2], int (&output)[2]) override { aceqv::views4_target_sizes(render, output); }
    bool qvfr_take_frame(Views4Frame& frame) override { return aceqv::views4_take_frame(frame); }
    bool qvfr_latest_frame(Views4Frame& frame) override { return aceqv::views4_latest_frame(frame); }
    void qvfr_note_views(const XrView* views, uint32_t count) override { aceqv::views4_note_views(views, count); }
    void qvfr_frame_end() override { aceqv::views4_frame_end(); }

    // The research experiments (mode=views3, mode=double_render) and the batch watch.
    void instance_created(const wchar_t* ini, const wchar_t* mode) override {
        views3_ = _wcsicmp(mode, L"views3") == 0;
        doubleRender_ = _wcsicmp(mode, L"double_render") == 0;
        batchWatch_ = GetPrivateProfileIntW(L"experiment", L"batch_watch", 1, ini) != 0;
        if (views3_)  // before ACE creates its renderer, so renderer init sees the patched mode
            aceqv::views3_patch();
    }
    void session_created(bool probed) override {
        if (probed) {
            if (doubleRender_)
                aceqv::double_render_start();
            if (views3_)
                aceqv::views3_watch();
        }
        if (batchWatch_)
            aceqv::batch_watch_start();
    }
    void session_destroyed() override { aceqv::batch_watch_stop(); }
    void frame_end() override { aceqv::batch_watch_frame_end(); }

private:
    bool views3_ = false, doubleRender_ = false;
    bool batchWatch_ = true;  // batch_watch=1 by default
};

}  // namespace

aceqv::games::GameAdapter& adapter() {
    static AceAdapter a;
    return a;
}

}  // namespace ace
