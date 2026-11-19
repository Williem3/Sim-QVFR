#include "fixation.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

#include <d3d11.h>
#include <wrl/client.h>

#include "../../engine/dlss_math.h"
#include "../../log.h"
#include "../../quad/d3d11_fixation.h"
#include "dlss_inputs.h"

namespace lmu {
namespace {

using Microsoft::WRL::ComPtr;

simqvfr::D3D11Fixation tracker;
bool started = false;
bool reversedDepth = true;
// The depth terms (depth = a + b / z) from the pass camera, looked up again every kTermsEvery passes: the near and far
// planes hardly change, and the camera's lookup isn't free.
constexpr std::uint64_t kTermsEvery = 90;
double terms[2] = {};
bool haveTerms = false;
std::uint64_t passes = 0;
bool queuedThisPass = false;  // read before the glass: the pass's post leaves it
int termsSaid = 0;

bool update_terms() {
    float projection[16];
    if (!lmu_pass_projection(0, projection))
        return haveTerms;
    namespace dlss = simqvfr::dlss;
    const dlss::M4 p = dlss::from_floats(projection);
    const dlss::M4 used = reversedDepth ? dlss::reversed_depth(p) : p;
    double nearM = 0, farM = 0;
    bool infinite = false;
    if (!dlss::near_far(used[10], used[14], nearM, farM, infinite))
        return haveTerms;
    if (!haveTerms || std::abs(used[10] - terms[0]) > 1e-6 || std::abs(used[14] - terms[1]) > 1e-6) {
        if (termsSaid < 4) {
            ++termsSaid;
            simqvfr::logf("lmu fixation: the scene's depth runs %.3f m to %s%.0f m (%s; depth = %.6f + %.6f / z)", nearM,
                          infinite ? "past " : "", farM, reversedDepth ? "reversed" : "0 near", used[10], used[14]);
        }
    }
    terms[0] = used[10];
    terms[1] = used[14];
    haveTerms = true;
    return true;
}

}  // namespace

void fixation_start(ID3D11Device* device, bool reversed) {
    reversedDepth = reversed;
    haveTerms = false;
    queuedThisPass = false;
    passes = 0;
    termsSaid = 0;
    started = tracker.start(device, "lmu fixation");
}

void fixation_stop() {
    started = false;
    tracker.stop();
}

void fixation_pass(ID3D11DepthStencilView* dsv, const simqvfr::FocusBox& box, unsigned eyeW, unsigned eyeH,
                   bool passEnd) {
    const bool already = queuedThisPass;
    queuedThisPass = !passEnd && queuedThisPass;
    if (already || !started || !dsv || !box.valid || !eyeW || !eyeH)
        return;
    if ((passes++ % kTermsEvery == 0 || !haveTerms) && !update_terms())
        return;
    ComPtr<ID3D11Resource> resource;
    dsv->GetResource(&resource);
    ComPtr<ID3D11Texture2D> depth;
    if (!resource || FAILED(resource.As(&depth)))
        return;
    // The box centre: in the eye's image (fractions, y down) and as the eye's tangents.
    const double fx = (box.rect[0] + box.rect[2]) / 2, fy = (box.rect[1] + box.rect[3]) / 2;
    const double tx = (box.focusTan[0] + box.focusTan[1]) / 2.0, ty = (box.focusTan[2] + box.focusTan[3]) / 2.0;
    const int rect[4] = {0, 0, static_cast<int>(eyeW), static_cast<int>(eyeH)};
    const int px = std::clamp(static_cast<int>(fx * eyeW), 0, rect[2] - 1);
    const int py = std::clamp(static_cast<int>(fy * eyeH), 0, rect[3] - 1);
    const bool queued =
        tracker.request(depth.Get(), rect, px, py, std::max(2, rect[2] / 150), tx, ty, terms[0], terms[1]);
    queuedThisPass = queued && !passEnd;
}

void fixation_frame_end() {
    if (started)
        tracker.frame_end();
}

double fixation_m() { return tracker.metres(); }  // 0 once stopped

}  // namespace lmu
