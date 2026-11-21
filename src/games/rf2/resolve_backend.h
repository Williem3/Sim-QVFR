#pragma once

#include <d3d11.h>

#include "backend_stats.h"
#include "compositor.h"
#include "pass_state.h"
#include "render_backend.h"

namespace simqvfr::games::rf2 {

struct ResolveResult {
    int eye = -1;
    bool attempted = false;
    bool composed = false;
};

inline ResolveResult handle_resolve(PassState& pass, Compositor& compositor, BackendStats& stats,
                                    ID3D11DeviceContext* context, ID3D11DeviceContext* trackedContext,
                                    bool& replaying, ID3D11Resource* dst, UINT dstSub,
                                    ID3D11Resource* src, unsigned long long frame,
                                    const float (&focusRect)[2][4], const AtlasLayout& layout) {
    ResolveResult result;
    if (context != trackedContext || replaying || !is_scene_resource(src)) return result;

    result.eye = pass.consume_composite_eye(frame);
    if (result.eye < 0) return result;

    result.attempted = true;
    result.composed = compositor.compose(context, dst, dstSub, result.eye, focusRect, layout, replaying);
    stats.note_composite(result.composed);
    return result;
}

}  // namespace simqvfr::games::rf2
