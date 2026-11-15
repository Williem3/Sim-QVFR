#include "adapter.h"

#include <windows.h>

#include <atomic>
#include <algorithm>
#include <cmath>

#include "../../log.h"
#include "../identity_adapter.h"
#include "../lmu/mvr_probe.h"
#include "corner.h"
#include "game.h"
#include "pass_probe.h"

namespace ats {
namespace {

// Identity only: the layer logs and QVFR doesn't start. Two log-only probes on the game's D3D11 device, each on unless
// its [experiment] key is 0:
//   - ats_pass_probe: the eye passes found from the render-target binds, their CPU time and draws (pass_probe.h);
//   - ats_shader_probe: every vertex shader converted to draw a second view and created through NVAPI (gate MV2 of the
//     multi-view playbook), then released. LMU's probe for now (its log lines say "lmu mvr probe"); it moves to
//     src/engine when ATS needs more of it than counting.
// And stage 1 of its QVFR, off unless asked: ats_corner = N (percent): each eye drawn into the top-left N% of its own
// targets on each axis, the layer submitting that corner (corner.h). The pass probe stays off with it (one set of
// context hooks).
class AtsAdapter final : public simqvfr::games::IdentityAdapter {
public:
    AtsAdapter() : IdentityAdapter(&kGame) {}

    void instance_created(const wchar_t* ini, const wchar_t* mode) override {
        IdentityAdapter::instance_created(ini, mode);
        passProbe_ = analysis() && ini && GetPrivateProfileIntW(L"experiment", L"ats_pass_probe", 1, ini) != 0;
        shaderProbe_ = analysis() && ini && GetPrivateProfileIntW(L"experiment", L"ats_shader_probe", 1, ini) != 0;
        // QVFR at the D3D11 level: on by default, with the launcher's QVFR switch ([experiment] mode other than off) and
        // [experiment] ats_qvfr (default 1). The focus view's scene draws by NVIDIA's multi-view rendering where the
        // device has it, by Sim-QVFR's own (instancing) on every other GPU: the corner picks at xrCreateSession.
        // ats_corner / ats_focus: the launch's sizes in percent (70 / 30; 0: that part off); the headset menu's sizes
        // ([quad] periphery_scale, focus_percent) take over at the first live read.
        wchar_t iniMode[32] = L"";
        if (ini)
            GetPrivateProfileStringW(L"experiment", L"mode", L"off", iniMode, 32, ini);
        const bool qvfr = analysis() && ini && _wcsicmp(iniMode, L"off") != 0 &&
                          GetPrivateProfileIntW(L"experiment", L"ats_qvfr", 1, ini) != 0;
        cornerPercent_ = qvfr ? static_cast<int>(GetPrivateProfileIntW(L"experiment", L"ats_corner", 70, ini)) : 0;
        if (cornerPercent_ <= 0 || cornerPercent_ >= 100)
            cornerPercent_ = 0;
        focusPercent_ = cornerPercent_ ? static_cast<int>(GetPrivateProfileIntW(L"experiment", L"ats_focus", 30, ini)) : 0;
        if (focusPercent_ < 0 || focusPercent_ + cornerPercent_ > 100)
            focusPercent_ = cornerPercent_ <= 90 ? std::min(30, 100 - cornerPercent_) : 0;
        if (focusPercent_ > 0)
            shaderProbe_ = true;  // the scene draws need the shaders' twins
        if (shaderProbe_)  // with the focus view on, each shader keeps its twin (the scene draws bind it)
            shaderProbe_ = lmu::mvr_probe_install(ini, focusPercent_ > 0);
        if (cornerPercent_)
            passProbe_ = false;
        simqvfr::logf("ats: QVFR at the D3D11 level %s (mode=%ls, ats_qvfr): corner %d%%, focus box %d%%; probes: pass %s, "
                      "shader %s",
                      cornerPercent_ ? "on" : "off", iniMode, cornerPercent_, focusPercent_, passProbe_ ? "on" : "off",
                      shaderProbe_ ? "on" : "off");
    }
    bool observes_d3d11() const override { return passProbe_ || shaderProbe_ || cornerPercent_; }
    // The focus box follows the gaze: where it points on each eye's screen (x right, y down, 0..1), from the eye's
    // field of view. Untracked: straight ahead (not the screen's middle: the eyes' fields of view are lopsided).
    bool wants_gaze() const override { return cornerPercent_ > 0 && focusPercent_ > 0; }
    // The overlay and the headset menu, and the menu's sizes live ([quad] periphery_scale and focus_percent: the menu
    // shows what runs, so they apply from the first read on; [experiment] ats_corner / ats_focus size the launch and
    // switch the path on).
    bool shows_overlay() const override { return cornerPercent_ > 0; }
    // The overlay's badge: MVR (NVIDIA's multi-view rendering) or SMVR (Sim-QVFR's own, instancing), as drawn this frame.
    simqvfr::games::QvfrMultiView qvfr_multi_view() const override {
        using simqvfr::games::QvfrMultiView;
        switch (corner_way()) {
            case 1: return QvfrMultiView::nvidia;
            case 2: return QvfrMultiView::software;
            case 3: return QvfrMultiView::eyesOnly;
            default: return QvfrMultiView::none;
        }
    }
    void live_view(const LiveView& v) override {
        const int corner = static_cast<int>(std::lround(v.periphery * 100)), fx = static_cast<int>(std::lround(v.focusX * 100)),
                  fy = static_cast<int>(std::lround(v.focusY * 100)), density = static_cast<int>(std::lround(v.density * 100));
        gaze_ = v.gaze;
        if (!liveSeen_ || corner != liveCorner_ || fx != liveFocus_ || fy != liveFocusY_ || density != liveDensity_) {
            simqvfr::logf("ats: the menu's sizes: periphery %d%%, focus box %d%% x %d%% at %d%% density", corner, fx, fy, density);
            corner_set_live(corner, fx, fy, density);
        }
        corner_set_look(static_cast<float>(v.feather), static_cast<float>(v.cornerRadius), v.border, v.borderColor);
        liveSeen_ = true;
        liveCorner_ = corner;
        liveFocus_ = fx;
        liveFocusY_ = fy;
        liveDensity_ = density;
    }
    void qvfr_note_views(const XrView* views, uint32_t count) override {
        for (uint32_t e = 0; e < count && e < 2; ++e) {
            fov_[e][0] = std::tan(views[e].fov.angleLeft);
            fov_[e][1] = std::tan(views[e].fov.angleRight);
            fov_[e][2] = std::tan(views[e].fov.angleDown);
            fov_[e][3] = std::tan(views[e].fov.angleUp);
        }
    }
    void qvfr_set_gaze(const float (&tan)[2][2], const bool (&valid)[2], bool) override {
        float screen[2][2];
        for (int e = 0; e < 2; ++e) {
            const bool tracked = valid[e] && gaze_;  // eye tracking off in the menu: straight ahead
            const float x = tracked ? tan[e][0] : 0, y = tracked ? tan[e][1] : 0;
            const float w = fov_[e][1] - fov_[e][0], h = fov_[e][3] - fov_[e][2];
            screen[e][0] = w > 0 ? (x - fov_[e][0]) / w : 0.5f;
            screen[e][1] = h > 0 ? (fov_[e][3] - y) / h : 0.5f;
        }
        corner_set_gaze(screen);
    }
    void d3d11_session(ID3D11Device* device) override {
        if (shaderProbe_)
            lmu::mvr_probe_device(device);
        if (cornerPercent_)
            corner_install(device, cornerPercent_, shaderProbe_ ? focusPercent_ : 0);
        if (passProbe_)
            pass_probe_install(device);
    }
    void session_destroyed() override {
        pass_probe_session_end();
        corner_session_end();
    }
    void swapchain_acquired(XrSwapchain chain, uint32_t index, PFN_xrEnumerateSwapchainImages enumerate) override {
        corner_acquired(chain, index, enumerate);
    }
    void swapchain_releasing(XrSwapchain chain) override { corner_releasing(chain); }
    const XrFrameEndInfo* adjust_frame_end(const XrFrameEndInfo* info, PFN_xrEnumerateSwapchainImages enumerate) override {
        return corner_frame_end(info, enumerate);
    }
    void frame_end() override {
        if (passProbe_)
            pass_probe_frame_end();
        if (shaderProbe_)
            lmu::mvr_probe_frame_end();
    }

private:
    bool passProbe_ = false, shaderProbe_ = false;
    int cornerPercent_ = 0, focusPercent_ = 0;
    bool liveSeen_ = false;
    std::atomic<bool> gaze_{true};
    int liveCorner_ = 0, liveFocus_ = 0, liveFocusY_ = 0, liveDensity_ = 0;
    float fov_[2][4] = {};  // each eye's tangents: left, right, down, up
};

}  // namespace

simqvfr::games::GameAdapter& adapter() {
    static AtsAdapter a;
    return a;
}

}  // namespace ats
