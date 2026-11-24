// ACE's focus views at the D3D12 level (quad/focus_images.h) by the replay probe (replay_probe.h). ACE's own replay
// switches ([experiment] replay_*: which passes are replayed, the diagnostics) are read here, not by the layer: they
// default on with the API path, and diagnostics that write pictures or time the GPU run only in a Detailed logs launch.
#include "focus_images.h"

#include <windows.h>

#include <algorithm>
#include <cstring>
#include <cwchar>

#include "../../log.h"
#include "../../probe/focus_ngx.h"
#include "replay_probe.h"
#include "stereo_flag.h"

namespace ace {
namespace {

using simqvfr::FocusImage;
using simqvfr::FocusImagesStart;

UINT read_uint(const wchar_t* ini, const wchar_t* key, UINT def) { return GetPrivateProfileIntW(L"experiment", key, def, ini); }

// The replay's own switches, as the layer's config read them before they moved here (layer/config.cpp).
simqvfr::ReplayFocusSettings read_switches(const FocusImagesStart& s) {
    const wchar_t* ini = s.ini ? s.ini : L"";
    const UINT api = s.apiPath ? 1u : 0u;
    simqvfr::ReplayFocusSettings f;
    f.boxFraction = s.boxFraction;
    f.boxFractionY = s.boxFractionY;
    f.pixelScale = s.pixelScale;
    f.pixelScaleY = s.pixelScaleY;
    f.post = s.post;
    f.dlss = s.dlss;
    f.dlssScale = s.dlssScale;
    f.saveFirst = read_uint(ini, L"replay_save_first", 1800);
    f.saveEvery = std::max(1u, read_uint(ini, L"replay_save_every", 900));
    f.saveCount = read_uint(ini, L"replay_save_count", 0);
    wchar_t targets[16] = {};
    GetPrivateProfileStringW(L"experiment", L"replay_targets", api ? L"all" : L"depth", targets, 16, ini);
    f.allTargets = _wcsicmp(targets, L"all") == 0;
    f.tiles = read_uint(ini, L"replay_tiles", api) != 0;
    f.fog = static_cast<int>(read_uint(ini, L"replay_fog", api * 2));
    f.shadows = static_cast<int>(read_uint(ini, L"replay_shadows", api));
    f.transparents = static_cast<int>(read_uint(ini, L"replay_transparents", api));
    f.costAb = read_uint(ini, L"replay_cost_ab", 0) != 0;
    f.sky = read_uint(ini, L"replay_sky", api) != 0;
    f.tyres = read_uint(ini, L"replay_tyres", 1) != 0;
    f.flagsWidth = read_uint(ini, L"replay_flags_width", 1) != 0;
    f.depthFade = read_uint(ini, L"replay_depth_fade", 1) != 0;
    f.fixation = read_uint(ini, L"replay_fixation", 1) != 0;
    f.diagMode = static_cast<int>(read_uint(ini, L"replay_diag", 0));
    f.diag = f.diagMode == 1;
    f.gpuSteps = read_uint(ini, L"replay_gpu_steps", 0) != 0;
    f.maskAb = read_uint(ini, L"replay_mask_ab", 0) != 0 && f.gpuSteps;
    f.recordTiming = read_uint(ini, L"replay_record_timing", 0) != 0;
    f.cullProbe = read_uint(ini, L"replay_cull_probe", 0) != 0;
    f.cull = static_cast<int>(read_uint(ini, L"replay_cull", api * 2));
    f.atmosphere = static_cast<int>(read_uint(ini, L"replay_atmosphere", api * 4));
    // Diagnostics that write pictures by themselves, time the GPU or change the picture to measure it run only in a
    // Detailed logs launch, whatever the ini says.
    if (!simqvfr::log_debug()) {
        f.saveCount = 0;
        f.gpuSteps = f.cullProbe = f.costAb = f.maskAb = f.recordTiming = false;
        if (f.cull == 3)  // the alternating A/B: the culling simply on
            f.cull = 2;
    }
    return f;
}

class AceFocusImages final : public simqvfr::FocusImages {
public:
    void early(const FocusImagesStart& s) override {
        const wchar_t* ini = s.ini ? s.ini : L"";
        const UINT api = s.apiPath ? 1u : 0u;
        // ACE's API path draws its focus views from ACE's single-pass stereo lists: on before ACE makes its renderer.
        if (s.compose)
            simqvfr::ace_single_pass_stereo_on();
        if (s.mode == 3)
            simqvfr::replay_probe_early_swap_chain();  // before the game makes its window swap chain
        const bool sky = read_uint(ini, L"replay_sky", api) != 0, tiles = read_uint(ini, L"replay_tiles", api) != 0;
        const bool tyres = read_uint(ini, L"replay_tyres", 1) != 0;
        if (s.mode == 3 && ((sky && tiles) || tyres))
            simqvfr::replay_probe_early_sky();  // before the game makes its device and loads its pipelines
        if (s.mode == 3 && s.dlss) {
            // ACE's own DLSS switches: the share of ACE's x jitter NGX is told, and the eyes at DLAA in every mode.
            const double jitterX = std::clamp(read_uint(ini, L"replay_dlss_jitter_x_percent", 50), 10u, 200u) / 100.0;
            const bool dlaa = read_uint(ini, L"replay_dlss_periphery_dlaa", 0) != 0;
            // A live eye preset change remakes ACE's feature only with replay_dlss_eye_remake=1 (a remade K feature
            // smeared the right eye, 2026-11-24); otherwise it applies when ACE next makes its DLSS.
            const bool remake = read_uint(ini, L"replay_dlss_eye_remake", 0) != 0;
            simqvfr::focus_ngx::set_options(static_cast<float>(jitterX), dlaa, remake);
            simqvfr::logf("experiment: API path DLSS: NGX told %.0f%% of ACE's horizontal jitter; periphery DLAA in every "
                          "in-game mode %s; a live eye preset %s", jitterX * 100, dlaa ? "on" : "off",
                          remake ? "remakes ACE's feature" : "waits for ACE's next feature");
            simqvfr::focus_ngx::preload();  // NGX's exports detoured before the game makes its DLSS features
        }
    }
    bool start(const FocusImagesStart& s) override {
        return simqvfr::replay_probe_start(s.device, s.mode, s.queue, read_switches(s));
    }
    void frame_end() override { simqvfr::replay_probe_frame_end(); }
    void set_gaze(const float (&ndc)[2][2]) override { simqvfr::replay_probe_set_gaze(ndc); }
    void set_size(double k, double kY, double renderScale) override { simqvfr::replay_probe_set_size(k, kY, renderScale); }
    void set_box(double box, double boxY) override { simqvfr::replay_probe_set_box(box, boxY); }
    void set_eye_scale(double scale) override { simqvfr::replay_probe_set_eye_scale(scale); }
    void set_stock(bool on) override { simqvfr::replay_probe_set_stock(on); }
    void set_eye_texture_match(double share, double mips) override { simqvfr::replay_probe_set_eye_texture_match(share, mips); }
    bool focus_image(FocusImage& out) override {
        ID3D12Resource* image = nullptr;
        D3D12_RESOURCE_STATES state{};
        double box = 0, boxY = 0;
        float offset[2][2] = {};
        if (!simqvfr::replay_probe_focus_image(image, state, box, offset, &boxY))
            return false;
        out.image = image;
        out.state = static_cast<int>(state);
        out.box = box;
        out.boxY = boxY;
        std::memcpy(out.offset, offset, sizeof(offset));
        return true;
    }
    bool eye_region(float& fx, float& fy) override { return simqvfr::replay_probe_eye_region(fx, fy); }
    void eye_allow(bool composed) override { simqvfr::replay_probe_eye_allow(composed); }
    void gpu_scope(bool compositor) override { simqvfr::replay_probe_gpu_scope(compositor); }
    bool census(unsigned frames) override { return simqvfr::replay_probe_census(frames); }
    void set_live_diag(unsigned bits) override { simqvfr::replay_probe_set_live_diag(bits); }
    void set_dlss_presets(int eye, int focus) override { simqvfr::focus_ngx::set_presets(eye, focus); }
    bool dlss_state(simqvfr::FocusDlssState& out) override {
        const simqvfr::focus_ngx::State d = simqvfr::focus_ngx::state();
        out.seen = d.seen;
        out.forcedDlaa = d.forcedDlaa;
        out.gameQuality = d.gameQuality;
        out.width = d.width;
        out.height = d.height;
        out.outWidth = d.outWidth;
        out.outHeight = d.outHeight;
        out.eyePreset = d.eyePreset;
        out.focusPreset = d.focusPreset;
        out.focusRunning = d.focusRunning;
        return true;
    }
};

}  // namespace

simqvfr::FocusImages& focus_images() {
    static AceFocusImages images;
    return images;
}

}  // namespace ace
