#include "adapter.h"

#include <windows.h>

#include <atomic>
#include <algorithm>
#include <cmath>
#include <cstdlib>

#include "../../dev_build.h"
#include "../../log.h"
#include "../identity_adapter.h"
#include "../lmu/mvr_probe.h"
#include "corner.h"
#include "ats/game.h"
#include "ets2/game.h"
#include "pass_probe.h"

namespace scs {
namespace {

// QVFR at the D3D11 level (corner.h), on unless mode is off or [experiment] scs_qvfr=0. Its parts:
//   - scs_shader_probe: every vertex shader given its twins (NVIDIA's two views and the instanced one) at creation.
//     LMU's probe for now (its log lines say "lmu mvr probe"); it moves to src/engine with the rest of LMU's multi-view;
//   - the corner and the focus view (scs_corner / scs_focus, percent);
//   - scs_pass_probe: log-only, the eye passes found from the render-target binds (pass_probe.h); dev builds only, off
//     while the corner runs (one set of context hooks).
class ScsAdapter final : public simqvfr::games::IdentityAdapter {
public:
    explicit ScsAdapter(const simqvfr::games::GameInfo* game) : IdentityAdapter(game) {}

    void instance_created(const wchar_t* ini, const wchar_t* mode) override {
        IdentityAdapter::instance_created(ini, mode);
        // The pass probe only logs: a diagnostic, in development builds alone (dev_build.h).
        passProbe_ = simqvfr::kDevBuild && analysis() && ini &&
                     GetPrivateProfileIntW(L"experiment", L"scs_pass_probe", 1, ini) != 0;
        shaderProbe_ = analysis() && ini && GetPrivateProfileIntW(L"experiment", L"scs_shader_probe", 1, ini) != 0;
        // QVFR at the D3D11 level: on by default, with the launcher's QVFR switch ([experiment] mode other than off) and
        // [experiment] scs_qvfr (default 1). The focus view's scene draws by NVIDIA's multi-view rendering where the
        // device has it, by Sim-QVFR's own (instancing) on every other GPU: the corner picks at xrCreateSession.
        // scs_corner / scs_focus: the launch's sizes in percent (70 / 30; 0: that part off); the headset menu's sizes
        // ([experiment] periphery_scale, focus_percent) take over at the first live read.
        wchar_t iniMode[32] = L"";
        cornerPercent_ = launch_corner(ini, iniMode);
        const simqvfr::games::GameInfo::Defaults& sizes = info()->defaults;
        focusPercent_ = cornerPercent_ ? static_cast<int>(GetPrivateProfileIntW(L"experiment", L"scs_focus",
                                                                                 sizes.focusWidth > 0 ? sizes.focusWidth : 30, ini))
                                       : 0;
        if (focusPercent_ < 0 || focusPercent_ + cornerPercent_ > 100)
            focusPercent_ = cornerPercent_ <= 90 ? std::min(30, 100 - cornerPercent_) : 0;
        if (focusPercent_ > 0)
            shaderProbe_ = true;  // the scene draws need the shaders' twins
        if (shaderProbe_)  // with the focus view on, each shader keeps its twin (the scene draws bind it)
            shaderProbe_ = lmu::mvr_probe_install(ini, focusPercent_ > 0);
        if (cornerPercent_)
            passProbe_ = false;
        if (focusPercent_ > 0)
            read_diag(ini);
        simqvfr::logf("scs: QVFR at the D3D11 level %s (mode=%ls, scs_qvfr): corner %d%%, focus box %d%%; probes: pass %s, "
                      "shader %s",
                      cornerPercent_ ? "on" : "off", iniMode, cornerPercent_, focusPercent_, passProbe_ ? "on" : "off",
                      shaderProbe_ ? "on" : "off");
    }
    // The layer's own reports and CPU settings: QVFR runs here whenever the corner will (read from the ini at load).
    bool own_qvfr(const wchar_t* ini) const override {
        wchar_t iniMode[32] = L"";
        return launch_corner(ini, iniMode) > 0;
    }
    bool observes_d3d11() const override { return passProbe_ || shaderProbe_ || cornerPercent_; }
    // The focus box follows the gaze: where it points on each eye's screen (x right, y down, 0..1), from the eye's
    // field of view. Untracked: straight ahead (not the screen's middle: the eyes' fields of view are lopsided).
    bool wants_gaze() const override { return cornerPercent_ > 0 && focusPercent_ > 0; }
    // The overlay and the headset menu, and the menu's sizes live ([experiment] periphery_scale and focus_percent: the menu
    // shows what runs, so they apply from the first read on; [experiment] scs_corner / scs_focus size the launch and
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
            simqvfr::logf("scs: the menu's sizes: periphery %d%%, focus box %d%% x %d%% at %d%% density", corner, fx, fy, density);
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
    void swapchain_destroyed(XrSwapchain chain) override { corner_swapchain_destroyed(chain); }
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
    // The corner's launch share in percent, 0 when QVFR is off: the launcher's QVFR switch ([experiment] mode other than
    // off), [experiment] scs_qvfr (default 1) and a corner inside 1..99 ([experiment] scs_corner, default the game's
    // periphery size). `iniMode`: the ini's mode, for the log.
    int launch_corner(const wchar_t* ini, wchar_t (&iniMode)[32]) const {
        if (ini)
            GetPrivateProfileStringW(L"experiment", L"mode", L"off", iniMode, 32, ini);
        if (!analysis() || !ini || _wcsicmp(iniMode, L"off") == 0 ||
            GetPrivateProfileIntW(L"experiment", L"scs_qvfr", 1, ini) == 0)
            return 0;
        const simqvfr::games::GameInfo::Defaults& sizes = info()->defaults;
        const int corner = static_cast<int>(
            GetPrivateProfileIntW(L"experiment", L"scs_corner", sizes.periphery > 0 ? sizes.periphery : 70, ini));
        return corner > 0 && corner < 100 ? corner : 0;
    }

    // The GPU-hang bisection keys (corner_set_diag), read at launch: scs_focus_draws=0, scs_focus_replays=0, and
    // scs_replay_skip=<the frame's replays to skip, 1-based, comma-separated, e.g. 3,7,12>.
    static void read_diag(const wchar_t* ini) {
        const bool noDraws = GetPrivateProfileIntW(L"experiment", L"scs_focus_draws", 1, ini) == 0;
        const bool noReplays = GetPrivateProfileIntW(L"experiment", L"scs_focus_replays", 1, ini) == 0;
        wchar_t list[256] = L"";
        GetPrivateProfileStringW(L"experiment", L"scs_replay_skip", L"", list, 256, ini);
        uint64_t skip = 0;
        for (const wchar_t* p = list; *p;) {
            wchar_t* end = nullptr;
            const unsigned long n = wcstoul(p, &end, 10);
            if (end == p) {
                ++p;
                continue;
            }
            if (n >= 1 && n <= 64)
                skip |= 1ull << (n - 1);
            p = end;
        }
        corner_set_diag(noDraws, noReplays, skip);
        if (noDraws || noReplays || skip)
            simqvfr::logf("scs focus: bisection: the focus view's scene draws %s, its replayed passes %s%s%ls",
                          noDraws ? "OFF (scs_focus_draws=0)" : "on", noReplays ? "OFF (scs_focus_replays=0)" : "on",
                          skip ? ", skipping the frame's replays " : "", skip ? list : L"");
    }

    bool passProbe_ = false, shaderProbe_ = false;
    int cornerPercent_ = 0, focusPercent_ = 0;
    bool liveSeen_ = false;
    std::atomic<bool> gaze_{true};
    int liveCorner_ = 0, liveFocus_ = 0, liveFocusY_ = 0, liveDensity_ = 0;
    float fov_[2][4] = {};  // each eye's tangents: left, right, down, up
};

}  // namespace

simqvfr::games::GameAdapter& adapter(const simqvfr::games::GameInfo* game) {
    static ScsAdapter ats(&::ats::kGame), ets2(&::ets2::kGame);
    return game == &::ets2::kGame ? ets2 : ats;
}

}  // namespace scs
