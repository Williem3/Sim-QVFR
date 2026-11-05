#include "adapter.h"

#include <windows.h>

#include "../../engine/code_patch.h"
#include "../../log.h"
#include "abi.h"
#include "four_views.h"
#include "game.h"

namespace xplane {
namespace {

using simqvfr::Views4Frame;
using simqvfr::Views4Layout;

// [experiment] xplane_layers = 3 or 4: the render size query reports that many layers instead of 2, so
// REN_graph_info and the per-eye targets get them. A measurement for QVFR: a frame capture then shows which counts,
// targets and draws follow (README.md). 0 or 2: X-Plane as it is.
void apply_layer_count(const wchar_t* ini, int forced = 0) {
    const int layers = forced ? forced : GetPrivateProfileIntW(L"experiment", L"xplane_layers", 0, ini);
    if (layers == 0 || layers == 2)
        return;
    if (layers < 1 || layers > 4) {
        simqvfr::logf("xplane: xplane_layers=%d ignored (1-4)", layers);
        return;
    }
    if (!simqvfr::code_matches(kVrLayerCount, kVrLayerCountAnchor, sizeof(kVrLayerCountAnchor))) {
        simqvfr::logf("xplane: xplane_layers=%d not applied: the layer count at 0x%llx isn't this build's", layers,
                      static_cast<unsigned long long>(kVrLayerCount));
        return;
    }
    const auto value = static_cast<std::uint8_t>(layers);
    const bool ok = simqvfr::code_write(kVrLayerCount + kVrLayerCountImm, &value, 1);
    simqvfr::logf("xplane: VR layer count 2 -> %d %s (render size query, 0x%llx)", layers, ok ? "patched" : "FAILED",
                  static_cast<unsigned long long>(kVrLayerCount));
}

// [experiment] xplane_context_layers = 3 or 4: the VR render context is made with that many layers, its stereo test
// becomes layers >= 2 (it is layers == 2, which would turn the context mono), the camera still takes two views'
// tangents (its room), the 3D world subgraph gives its targets that many layers, and the two VR-context resizes keep
// that many (they passed 2, which also rebuilt its lit-image miptree with two layers). 0 or 2: X-Plane as it is. All
// patches or none.
void apply_context_layers(const wchar_t* ini, int forced = 0) {
    const int layers = forced ? forced : GetPrivateProfileIntW(L"experiment", L"xplane_context_layers", 0, ini);
    if (layers == 0 || layers == 2)
        return;
    if (layers < 3 || layers > 4) {
        simqvfr::logf("xplane: xplane_context_layers=%d ignored (3-4)", layers);
        return;
    }
    if (!simqvfr::code_matches(kVrContextLayers, kVrContextLayersAnchor, sizeof(kVrContextLayersAnchor)) ||
        !simqvfr::code_matches(kStereoTestCmp, kStereoTestCmpAnchor, sizeof(kStereoTestCmpAnchor)) ||
        !simqvfr::code_matches(kStereoTestSet, kStereoTestSetAnchor, sizeof(kStereoTestSetAnchor)) ||
        !simqvfr::code_matches(kTangentCopyBound, kTangentCopyBoundAnchor, sizeof(kTangentCopyBoundAnchor)) ||
        !simqvfr::code_matches(kWorldSubgraphLayers, kWorldSubgraphLayersAnchor, sizeof(kWorldSubgraphLayersAnchor)) ||
        !simqvfr::code_matches(kVrContextResizeA, kVrContextResizeAnchor, sizeof(kVrContextResizeAnchor)) ||
        !simqvfr::code_matches(kVrContextResizeB, kVrContextResizeAnchor, sizeof(kVrContextResizeAnchor))) {
        simqvfr::logf("xplane: xplane_context_layers=%d not applied: the VR context code isn't this build's", layers);
        return;
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
}

// [experiment] xplane_copies = 1: X-Plane draws four views, slots 2-3 copies of the eyes, and its final resolve shows
// layers 2-3 (four_views.h). Includes every four-layer patch (xplane_layers and xplane_context_layers at 4). A normal
// headset image: four views rendered. The stage gate before the focus cameras.
void apply_copies(const wchar_t* ini) {
    if (GetPrivateProfileIntW(L"experiment", L"xplane_copies", 0, ini) != 1)
        return;
    apply_layer_count(ini, 4);
    apply_context_layers(ini, 4);
    four_views_copies_start();
}

class XPlaneAdapter final : public simqvfr::games::GameAdapter {
public:
    const simqvfr::games::GameInfo* info() const override { return &kGame; }
    bool has_qvfr() const override { return false; }  // identity only: the layer logs
    bool qvfr_start(const simqvfr::games::QvfrStart&) override { return false; }
    void qvfr_set_gaze(const float (&)[2][2], const bool (&)[2], bool) override {}
    void qvfr_set_live_layout(const Views4Layout&) override {}
    void qvfr_set_live(double, double, double, bool) override {}
    bool qvfr_dlss_held() override { return false; }
    void qvfr_target_sizes(int (&render)[2], int (&output)[2]) override {
        render[0] = render[1] = output[0] = output[1] = 0;
    }
    bool qvfr_take_frame(Views4Frame&) override { return false; }
    bool qvfr_latest_frame(Views4Frame&) override { return false; }
    void qvfr_note_views(const XrView*, uint32_t) override {}
    void qvfr_frame_end() override {}

    // At xrCreateInstance: X-Plane builds its render graph later, when the flight loads.
    void instance_created(const wchar_t* ini, const wchar_t*) override {
        if (GetPrivateProfileIntW(L"experiment", L"xplane_copies", 0, ini) == 1) {
            apply_copies(ini);
            return;
        }
        apply_layer_count(ini);
        apply_context_layers(ini);
    }
};

}  // namespace

simqvfr::games::GameAdapter& adapter() {
    static XPlaneAdapter a;
    return a;
}

}  // namespace xplane
