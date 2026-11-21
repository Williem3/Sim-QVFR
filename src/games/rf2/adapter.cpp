#include "adapter.h"
#include <windows.h>
#include "../../log.h"
#include "../../quad/regulator_settings.h"
#include "../identity_adapter.h"
#include "../openvr_bridge.h"
#include "../lmu/mvr_pass.h"
#include "../lmu/mvr_probe.h"
#include "game.h"

namespace rf2 {
namespace {
class Rf2Adapter final : public simqvfr::games::IdentityAdapter {
public:
    Rf2Adapter() : IdentityAdapter(&kGame) {}
    // RF2 renders QVFR itself in its D3D11 pass (src/games/rf2/). The layer adds its measurement only: the performance
    // overlay (frame and CPU time, GPU load, busiest thread, VRAM). No views4/quad method: the eyes stay the runtime's.
    bool has_qvfr() const override { return true; }
    bool measure_only() const override { return true; }
    // The ini's focus box (at the start and live): the share of the eye's field of view the focus view covers.
    void qvfr_set_live(double focusFraction, double focusFractionY, double, bool) override {
        if (analysis()) lmu::mvr_rf2_set_focus(focusFraction, focusFractionY);
    }
    // The ini's view sizes (periphery_scale, focus density): the outer and focus views' pixel counts in the scene target.
    void qvfr_set_density(double peripheryScale, double focusWidth, double focusHeight) override {
        if (analysis()) lmu::mvr_rf2_set_density(peripheryScale, focusWidth, focusHeight);
    }
    // The ini's [regulator] (at the start and live), and the set focus density it works down from.
    void qvfr_set_regulator(const simqvfr::GpuRegulatorSettings& s, double density) override {
        if (analysis())
            lmu::mvr_rf2_set_regulator(s.focus, s.periphery, s.decreaseAbove, s.increaseBelow, s.lowerFrames, s.raiseFrames,
                                       density);
    }
    // The regulator's present cuts (the overlay's row).
    simqvfr::games::QvfrRegulatorStatus qvfr_regulator_status() const override {
        simqvfr::games::QvfrRegulatorStatus r;
        if (analysis())
            r.active = lmu::mvr_rf2_regulator_status(r.focus, r.periphery, r.peripheryPercent, r.densityPercent,
                                                     r.cutPeriphery, r.cutDensity);
        return r;
    }
    // The ini's feather, corner radius, outline and the focus box's vertical offset (at the start and live).
    void qvfr_set_focus_look(double feather, double cornerRadius, bool outline, const float (&outlineColor)[4],
                             double offsetDegrees) override {
        if (analysis()) lmu::mvr_rf2_set_focus_look(feather, cornerRadius, outline, outlineColor, offsetDegrees);
    }
    void instance_created(const wchar_t* ini, const wchar_t* mode) override {
        IdentityAdapter::instance_created(ini, mode);
        if (!analysis()) return;
        const bool ok = lmu::mvr_probe_install(ini, true, false); // attach NVAPI + instanced twins
        simqvfr::logf("rf2: MVR probe %s (twins attached)", ok ? "installed" : "FAILED");
        // [experiment] gpu_timing: 0 (the default when the key is not in the ini) no timing log, 1 the log, 2 also the passes' statistics.
        lmu::mvr_rf2_set_gpu_timing(ini ? static_cast<int>(GetPrivateProfileIntW(L"experiment", L"gpu_timing", 0, ini)) : 0);
    }
    void d3d11_session(ID3D11Device* device) override {
        if (!analysis() || !device) return;
        lmu::mvr_probe_device(device);
        lmu::mvr_rf2_policy(true);
        const bool ok = lmu::mvr_pass_install(device);
        simqvfr::logf("rf2: real-eye MVR pass hooks %s device=%p", ok ? "installed" : "FAILED", device);
    }
    void frame_end() override {
        lmu::mvr_probe_frame_end();
        lmu::mvr_frame_end(frame_++);
        simqvfr::games::openvr_bridge::Submission submit{};
        if (simqvfr::games::openvr_bridge::submission_snapshot(submit) &&
            submit.sequence != lastSubmitSequence_) {
            lastSubmitSequence_ = submit.sequence;
            if (submit.sequence <= 8 || submit.sequence % 300 == 0) {
                simqvfr::logf(
                    "rf2 OpenVR submit: seq=%llu views=%u chains=%s "
                    "L chain=%p rect=%d,%d %dx%d; R chain=%p rect=%d,%d %dx%d",
                    static_cast<unsigned long long>(submit.sequence), submit.viewCount,
                    submit.sameSwapchain ? "same" : "separate",
                    reinterpret_cast<void*>(submit.swapchain[0]),
                    submit.rect[0].offset.x, submit.rect[0].offset.y,
                    submit.rect[0].extent.width, submit.rect[0].extent.height,
                    reinterpret_cast<void*>(submit.swapchain[1]),
                    submit.rect[1].offset.x, submit.rect[1].offset.y,
                    submit.rect[1].extent.width, submit.rect[1].extent.height);
            }
        }
        simqvfr::games::openvr_bridge::SubmittedImages images{};
        if (simqvfr::games::openvr_bridge::submitted_images_snapshot(images) &&
            images.sequence != lastImageSequence_) {
            lastImageSequence_ = images.sequence;
            if (images.sequence <= 8 || images.sequence % 300 == 0) {
                simqvfr::logf(
                    "rf2 OpenVR images: seq=%llu L index=%u texture=%p valid=%d; "
                    "R index=%u texture=%p valid=%d",
                    static_cast<unsigned long long>(images.sequence),
                    images.index[0], static_cast<void*>(images.texture[0]), images.valid[0] ? 1 : 0,
                    images.index[1], static_cast<void*>(images.texture[1]), images.valid[1] ? 1 : 0);
            }
        }
    }
    void qvfr_note_projection(const XrCompositionLayerProjection* projection) override {
        if (analysis())
            simqvfr::games::openvr_bridge::note_projection(projection);
    }
    void qvfr_note_swapchain_images(XrSwapchain chain, const XrSwapchainImageD3D11KHR* images,
                                      uint32_t count) override {
        if (analysis()) simqvfr::games::openvr_bridge::note_swapchain_images(chain, images, count);
    }
    void qvfr_note_swapchain_acquired(XrSwapchain chain, uint32_t index) override {
        if (analysis()) simqvfr::games::openvr_bridge::note_swapchain_acquired(chain, index);
    }
    void session_destroyed() override {
        simqvfr::games::openvr_bridge::reset();
        IdentityAdapter::session_destroyed();
    }
    void qvfr_note_views(const XrView* views, uint32_t count) override {
        if (!analysis())
            return;

        // RF2 and ACC both reach OpenXR through OpenComposite. Keep the runtime-facing
        // stereo data in one game-neutral bridge; the RF2 D3D11 backend only consumes it.
        simqvfr::games::openvr_bridge::note_views(views, count);
        simqvfr::games::openvr_bridge::StereoViews snapshot{};
        if (simqvfr::games::openvr_bridge::snapshot(snapshot))
            lmu::mvr_rf2_note_fov(snapshot.tanFov);
    }
private:
    unsigned long long frame_ = 0;
    uint64_t lastSubmitSequence_ = 0;
    uint64_t lastImageSequence_ = 0;
};
Rf2Adapter g_adapter;
}
simqvfr::games::GameAdapter& adapter() { return g_adapter; }
}
