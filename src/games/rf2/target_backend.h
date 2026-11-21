#pragma once

#include <d3d11.h>
#include <wrl/client.h>

#include "render_backend.h"

namespace simqvfr::games::rf2 {

// The full-size FP16/MSAA eye scene target (see render_backend.h).
inline bool is_full_scene_target(ID3D11RenderTargetView* rtv) {
    if (!rtv) return false;
    Microsoft::WRL::ComPtr<ID3D11Resource> resource;
    rtv->GetResource(&resource);
    return is_scene_resource(resource.Get());
}

}  // namespace simqvfr::games::rf2
