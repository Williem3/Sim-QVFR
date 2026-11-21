#pragma once

#include <cmath>
#include <cstdint>
#include <mutex>
#include <unordered_map>
#include <vector>
#include <wrl/client.h>
#include <d3d11.h>

#include <openxr/openxr.h>

namespace simqvfr::games::openvr_bridge {

struct Submission {
    XrSwapchain swapchain[2]{XR_NULL_HANDLE, XR_NULL_HANDLE};
    XrRect2Di rect[2]{};
    uint32_t viewCount = 0;
    uint64_t sequence = 0;
    bool sameSwapchain = false;
    bool valid = false;
};

struct SubmittedImages {
    ID3D11Texture2D* texture[2]{};  // borrowed from held COM references below
    uint32_t index[2]{};
    uint64_t sequence = 0;
    bool valid[2]{};
};

struct StereoViews {
    float tanFov[2][4]{};  // left, right, down, up
    uint64_t sequence = 0;
    bool valid = false;
};

namespace detail {
inline std::mutex mutex;
inline StereoViews views{};
inline Submission submission{};
inline uint64_t sequence = 0;
inline uint64_t submissionSequence = 0;
struct ChainImages {
    std::vector<Microsoft::WRL::ComPtr<ID3D11Texture2D>> images;
    uint32_t acquired = 0;
    bool haveAcquired = false;
};
inline std::unordered_map<XrSwapchain, ChainImages> chains;
}  // namespace detail

inline void note_views(const XrView* xrViews, uint32_t count) {
    if (!xrViews || count < 2)
        return;

    StereoViews next{};
    for (int eye = 0; eye < 2; ++eye) {
        next.tanFov[eye][0] = std::tan(xrViews[eye].fov.angleLeft);
        next.tanFov[eye][1] = std::tan(xrViews[eye].fov.angleRight);
        next.tanFov[eye][2] = std::tan(xrViews[eye].fov.angleDown);
        next.tanFov[eye][3] = std::tan(xrViews[eye].fov.angleUp);
        if (!(next.tanFov[eye][1] > next.tanFov[eye][0]) ||
            !(next.tanFov[eye][3] > next.tanFov[eye][2]))
            return;
    }

    std::lock_guard<std::mutex> lock(detail::mutex);
    next.sequence = ++detail::sequence;
    next.valid = true;
    detail::views = next;
}

inline void note_projection(const XrCompositionLayerProjection* projection) {
    if (!projection || !projection->views || projection->viewCount < 2)
        return;
    Submission next{};
    next.viewCount = projection->viewCount;
    for (int eye = 0; eye < 2; ++eye) {
        next.swapchain[eye] = projection->views[eye].subImage.swapchain;
        next.rect[eye] = projection->views[eye].subImage.imageRect;
    }
    next.sameSwapchain = next.swapchain[0] == next.swapchain[1];
    std::lock_guard<std::mutex> lock(detail::mutex);
    next.sequence = ++detail::submissionSequence;
    next.valid = true;
    detail::submission = next;
}

inline void note_swapchain_images(XrSwapchain chain, const XrSwapchainImageD3D11KHR* images, uint32_t count) {
    if (chain == XR_NULL_HANDLE || !images || !count) return;
    std::lock_guard<std::mutex> lock(detail::mutex);
    auto& state = detail::chains[chain];
    state.images.clear();
    state.images.reserve(count);
    for (uint32_t i = 0; i < count; ++i) state.images.emplace_back(images[i].texture);
}

inline void note_swapchain_acquired(XrSwapchain chain, uint32_t index) {
    if (chain == XR_NULL_HANDLE) return;
    std::lock_guard<std::mutex> lock(detail::mutex);
    auto& state = detail::chains[chain];
    state.acquired = index;
    state.haveAcquired = true;
}

inline bool submitted_images_snapshot(SubmittedImages& out) {
    std::lock_guard<std::mutex> lock(detail::mutex);
    if (!detail::submission.valid) return false;
    SubmittedImages next{};
    next.sequence = detail::submission.sequence;
    for (int eye = 0; eye < 2; ++eye) {
        auto it = detail::chains.find(detail::submission.swapchain[eye]);
        if (it == detail::chains.end() || !it->second.haveAcquired ||
            it->second.acquired >= it->second.images.size()) continue;
        next.index[eye] = it->second.acquired;
        next.texture[eye] = it->second.images[next.index[eye]].Get();
        next.valid[eye] = next.texture[eye] != nullptr;
    }
    out = next;
    return next.valid[0] || next.valid[1];
}

inline bool submission_snapshot(Submission& out) {
    std::lock_guard<std::mutex> lock(detail::mutex);
    if (!detail::submission.valid)
        return false;
    out = detail::submission;
    return true;
}

inline bool snapshot(StereoViews& out) {
    std::lock_guard<std::mutex> lock(detail::mutex);
    if (!detail::views.valid)
        return false;
    out = detail::views;
    return true;
}

inline void reset() {
    std::lock_guard<std::mutex> lock(detail::mutex);
    detail::views = {};
    detail::submission = {};
    detail::sequence = 0;
    detail::submissionSequence = 0;
    detail::chains.clear();
}

}  // namespace simqvfr::games::openvr_bridge
