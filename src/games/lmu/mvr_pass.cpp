// LMU's multi-view rendering in the eye passes (mvr_pass.h). Render thread only: the hooks act on the game's
// immediate context while a pass runs and pass everything else straight through. They are on every function the
// context's entries hold in d3d11's modes (MinHook; "How the hooks are reached" below).
#include "mvr_pass.h"
#include "mvr_pass_internal.h"

#include <windows.h>
#include <d3d11_4.h>
#include <intrin.h>
#include <wrl/client.h>

#include <MinHook.h>
#include <d3dcompiler.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdarg>
#include <cstring>
#include <limits>
#include <filesystem>
#include <string>
#include <tuple>
#include <type_traits>
#include <unordered_set>
#include <utility>
#include <vector>

#include "../../engine/code_patch.h"
#include "../../engine/nvapi_stereo.h"
#include "../../log.h"
#include "../../probe/d3d11_capture.h"
#include "../../engine/dlss_views.h"
#include "car_motion.h"
#include "dlss_inputs.h"
#include "fixation.h"
#include "focus_post.h"
#include "glare_math.h"
#include "mvr_probe.h"
#include "qvfr_frame.h"
#include "tag_viewport.h"
#include "view_pair.h"
#include "abi.h"
#include "yebis_shaders.h"

// This image's own base (mvr_pass_binds.h from_this_image: our RSGetViewports calls told from the game's).
extern "C" IMAGE_DOS_HEADER __ImageBase;

namespace lmu {

using namespace mvr_detail;  // the state and helpers shared with mvr_pass_*.cpp (mvr_pass_internal.h)

namespace {

using Microsoft::WRL::ComPtr;

using namespace context_slot;

#include "mvr_pass_hook_table.h"

}  // namespace

// The state shared with mvr_pass_trace.cpp, mvr_pass_census.cpp and mvr_pass_samples.cpp (mvr_pass_internal.h),
// defined here once.
namespace mvr_detail {

int nvapi_two_views(ID3D11DeviceContext* context, bool on) { return simqvfr::nvapi::set_two_views(context, on); }

State g;
Tracker tr;
PassTrace pt;
SizeCut sizeCut;
PostCensus census;
std::unordered_set<std::uint64_t> censusSaved;

}  // namespace mvr_detail

namespace {

// The pass trace (pt, pt_*): mvr_pass_trace.cpp.

#include "mvr_pass_tracker.h"

#include "mvr_pass_viewports.h"

#include "mvr_pass_twins.h"

#include "mvr_pass_cars.h"

#include "mvr_pass_two_views.h"

#include "mvr_pass_binds.h"

}  // namespace

namespace mvr_detail {

// The focus post at one of the pass's post-processing draws (the post window open): after YEBIS's copy of the resolved
// scene (the copy's pixel shader reading the resolve's target; its downsamples read other textures) the focus view's part
// copied likewise; after its composite (with the eye's glare and exposure bound) the focus view's composite.
// A texture's size and format for the log ("2076x1667 f10"), "-" for none.
std::string describe(ID3D11Resource* resource) {
    ComPtr<ID3D11Texture2D> t;
    if (!resource || FAILED(resource->QueryInterface(IID_PPV_ARGS(&t))))
        return "-";
    D3D11_TEXTURE2D_DESC d;
    t->GetDesc(&d);
    char s[48];
    std::snprintf(s, sizeof(s), "%ux%u f%d", d.Width, d.Height, static_cast<int>(d.Format));
    return s;
}

}  // namespace mvr_detail

namespace {

#include "mvr_pass_post.h"

#include "mvr_pass_doubling.h"

#include "mvr_pass_scene_learn.h"

}  // namespace

namespace mvr_detail {

// The cache's answer for `ps` (PsRoleEntry), GetPrivateData only the first time a shader is seen since the game last
// created one. 0: none (no shader, unmarked, or a role the probe doesn't know).
UINT role_of_ps(ID3D11PixelShader* ps) {
    if (!ps)
        return 0;
    ++g.stats.roleLookups;
    if (const uint64_t generation = mvr_pixel_shader_generation(); generation != g.roleGeneration) {
        g.roleGeneration = generation;
        ++g.roleEpoch;
    }
    PsRoleSet& set = g.psRoles[(static_cast<uint64_t>(reinterpret_cast<uintptr_t>(ps)) * 0x9E3779B97F4A7C15ull) >>
                               (64 - kTwinSetBits)];
    for (uint8_t k = 0; k < 2; ++k)
        if (set.entry[k].epoch == g.roleEpoch && set.entry[k].ps == ps) {
            set.next = static_cast<uint8_t>(1 - k);
            return set.entry[k].role;
        }
    ++g.stats.roleLookupMisses;
    UINT role = 0, size = sizeof(role);
    if (FAILED(ps->GetPrivateData(kYebisRoleGuid, &size, &role)) || size != sizeof(role))
        role = 0;
    const uint8_t k = set.entry[0].epoch != g.roleEpoch ? 0 : set.entry[1].epoch != g.roleEpoch ? 1 : set.next;
    set.next = static_cast<uint8_t>(1 - k);
    set.entry[k] = {ps, g.roleEpoch, role};
    return role;
}

}  // namespace mvr_detail

namespace {

#include "mvr_pass_post_roles.h"

#include "mvr_pass_focus_mask.h"

#include "mvr_pass_glass.h"

#include "mvr_pass_hooks_entry.h"

#include "mvr_pass_hook_install.h"

}  // namespace

#include "mvr_pass_api.h"

#include "mvr_pass_frame_end.h"

}  // namespace lmu
