// Native quad views (quad/native_quad.h; [experiment] native_quad_views=1): on a runtime with XR_VARJO_quad_views
// (Varjo's, Pimax's), the game's stereo session runs as PRIMARY_QUAD_VARJO below this layer, so the runtime composes the
// four views itself, the focus views at their own resolution. The game and the rest of this layer keep a stereo
// session: the wrappers here, installed in `next` when the instance has the extension, translate at the layer's
// bottom edge (the session's begin, its view locations, its visibility mask, its frames).
#include <atomic>
#include <cstring>
#include <vector>

#include "log.h"
#include "quad/native_quad.h"
#include "xr.h"
#include "layer/layer.h"

namespace simqvfr::layer {

namespace {

constexpr XrViewConfigurationType kQuad = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_QUAD_VARJO;
constexpr XrViewConfigurationType kStereo = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;

// Set at xrCreateSession: the system has the quad view configuration and this run wants it.
std::atomic<bool> planned = false;

const char* view_name(uint32_t v) {
    static const char* const kNames[] = {"left eye", "right eye", "left focus", "right focus"};
    return v < 4 ? kNames[v] : "?";
}

XrResult XRAPI_CALL quadBeginSession(XrSession session, const XrSessionBeginInfo* info) {
    if (!planned || !info || info->primaryViewConfigurationType != kStereo)
        return nativeQuad.BeginSession(session, info);
    XrSessionBeginInfo quadInfo = *info;
    quadInfo.primaryViewConfigurationType = kQuad;
    const XrResult r = nativeQuad.BeginSession(session, &quadInfo);
    if (XR_SUCCEEDED(r)) {
        nativeQuad.active = true;
        log_line("native quad views: the session runs as PRIMARY_QUAD_VARJO: the runtime composes the eyes and the focus "
                 "views (views 2-3), the game sees stereo");
        return r;
    }
    logf("native quad views: the runtime refused a quad view session (%d): stereo, composed by this layer", r);
    return nativeQuad.BeginSession(session, info);
}

XrResult XRAPI_CALL quadLocateViews(XrSession session, const XrViewLocateInfo* info, XrViewState* state,
                                    uint32_t capacity, uint32_t* countOutput, XrView* views) {
    if (!nativeQuad.active || !info || info->viewConfigurationType != kStereo)
        return nativeQuad.LocateViews(session, info, state, capacity, countOutput, views);
    if (!countOutput || (capacity && !views))
        return XR_ERROR_VALIDATION_FAILURE;
    *countOutput = 2;
    if (capacity == 0)
        return XR_SUCCESS;
    if (capacity < 2)
        return XR_ERROR_SIZE_INSUFFICIENT;
    XrViewLocateInfo quadInfo = *info;
    quadInfo.viewConfigurationType = kQuad;
    XrView four[kQuadViews];
    for (uint32_t v = 0; v < kQuadViews; ++v)
        four[v] = {XR_TYPE_VIEW, v < 2 ? views[v].next : nullptr};
    uint32_t located = 0;
    const XrResult r = nativeQuad.LocateViews(session, &quadInfo, state, kQuadViews, &located, four);
    if (XR_FAILED(r))
        return r;
    if (located < 2)
        return XR_ERROR_RUNTIME_FAILURE;
    static bool logged = false;
    if (!logged) {
        logged = true;
        for (uint32_t v = 0; v < located && v < kQuadViews; ++v)
            logf("native quad views: the runtime's %s FOV %.4f/%.4f/%.4f/%.4f (the game gets views 0-1; this layer's "
                 "own focus boxes replace 2-3)",
                 view_name(v), four[v].fov.angleLeft, four[v].fov.angleRight, four[v].fov.angleUp, four[v].fov.angleDown);
    }
    for (uint32_t v = 0; v < 2; ++v) {
        views[v].pose = four[v].pose;
        views[v].fov = four[v].fov;
    }
    return r;
}

XrResult XRAPI_CALL quadGetVisibilityMask(XrSession session, XrViewConfigurationType type, uint32_t viewIndex,
                                          XrVisibilityMaskTypeKHR maskType, XrVisibilityMaskKHR* mask) {
    // The eyes are views 0-1 of the quad configuration as of the stereo one.
    if (nativeQuad.active && type == kStereo && viewIndex < 2)
        type = kQuad;
    return nativeQuad.GetVisibilityMaskKHR(session, type, viewIndex, maskType, mask);
}

XrResult XRAPI_CALL quadEndFrame(XrSession session, const XrFrameEndInfo* info) {
    if (!nativeQuad.active || !info || info->layerCount == 0)
        return nativeQuad.EndFrame(session, info);
    QuadFrame frame;
    quad_frame(*info, frame);
    // A frame of views4_submit's with the focus views: a projection layer it already gave four views.
    bool focus = false;
    for (uint32_t i = 0; i < info->layerCount; ++i) {
        const auto* l = info->layers[i];
        focus |= l && l->type == XR_TYPE_COMPOSITION_LAYER_PROJECTION &&
                 reinterpret_cast<const XrCompositionLayerProjection*>(l)->viewCount == kQuadViews;
    }
    if (focus) {
        ++nativeQuad.focusFrames;
        if (!nativeQuad.loggedFirstFocus) {
            nativeQuad.loggedFirstFocus = true;
            log_line("native quad views: the first frame with the focus views as views 2-3");
        }
    } else if (frame.widened) {
        ++nativeQuad.copyFrames;
        if (!nativeQuad.loggedFirstCopy) {
            nativeQuad.loggedFirstCopy = true;
            log_line("native quad views: a frame without focus views (menus, loading, or composed): views 2-3 copy the "
                     "eyes");
        }
    }
    if (frame.refused) {
        if (nativeQuad.refusedLayers == 0)
            logf("native quad views: %u projection layer(s) of neither two nor four views left out of a frame",
                 frame.refused);
        nativeQuad.refusedLayers += frame.refused;
    }
    XrFrameEndInfo quadInfo = *info;
    quadInfo.layers = frame.layers.data();
    quadInfo.layerCount = static_cast<uint32_t>(frame.layers.size());
    return nativeQuad.EndFrame(session, &quadInfo);
}

}  // namespace

QuadOffer quad_views_offered(PFN_xrGetInstanceProcAddr gipa, bool runtimeBelow) {
    if (!runtimeBelow || !gipa)
        return QuadOffer::Unknown;  // a layer below may not answer before its own instance exists (gaze_extension)
    PFN_xrVoidFunction function = nullptr;
    if (XR_FAILED(gipa(XR_NULL_HANDLE, "xrEnumerateInstanceExtensionProperties", &function)) || !function)
        return QuadOffer::Unknown;
    const auto enumerate = reinterpret_cast<PFN_xrEnumerateInstanceExtensionProperties>(function);
    uint32_t count = 0;
    if (XR_FAILED(enumerate(nullptr, 0, &count, nullptr)))
        return QuadOffer::Unknown;
    std::vector<XrExtensionProperties> properties(count, XrExtensionProperties{XR_TYPE_EXTENSION_PROPERTIES});
    if (count && XR_FAILED(enumerate(nullptr, count, &count, properties.data())))
        return QuadOffer::Unknown;
    for (const XrExtensionProperties& p : properties)
        if (std::strcmp(p.extensionName, XR_VARJO_QUAD_VIEWS_EXTENSION_NAME) == 0)
            return QuadOffer::Yes;
    return QuadOffer::No;
}

void native_quad_install(XrInstance instance, PFN_xrGetInstanceProcAddr gipa) {
    if (!nativeQuad.extension || !next.BeginSession || !next.LocateViews || !next.EndFrame)
        return;
    gipa(instance, "xrEnumerateViewConfigurations",
         reinterpret_cast<PFN_xrVoidFunction*>(&nativeQuad.EnumerateViewConfigurations));
    nativeQuad.BeginSession = next.BeginSession;
    nativeQuad.LocateViews = next.LocateViews;
    nativeQuad.EndFrame = next.EndFrame;
    nativeQuad.GetVisibilityMaskKHR = next.GetVisibilityMaskKHR;
    next.BeginSession = quadBeginSession;
    next.LocateViews = quadLocateViews;
    next.EndFrame = quadEndFrame;
    if (next.GetVisibilityMaskKHR)
        next.GetVisibilityMaskKHR = quadGetVisibilityMask;
}

void native_quad_plan(XrInstance instance, XrSystemId system) {
    planned = false;
    nativeQuad.active = false;
    if (!nativeQuad.extension)
        return;
    if (!experiment.nativeQuadViews || !experiment.views4) {
        log_line("native quad views: the runtime has them, but this run doesn't use them (QVFR off, or not this game)");
        return;
    }
    uint32_t count = 0;
    std::vector<XrViewConfigurationType> types;
    if (!nativeQuad.EnumerateViewConfigurations ||
        XR_FAILED(nativeQuad.EnumerateViewConfigurations(instance, system, 0, &count, nullptr))) {
        log_line("native quad views: the runtime's view configurations unknown: stereo, composed by this layer");
        return;
    }
    types.resize(count);
    if (count && XR_FAILED(nativeQuad.EnumerateViewConfigurations(instance, system, count, &count, types.data())))
        types.clear();
    bool quadOffered = false;
    for (XrViewConfigurationType t : types)
        quadOffered |= t == kQuad;
    if (!quadOffered) {
        log_line("native quad views: the system has no PRIMARY_QUAD_VARJO configuration: stereo, composed by this layer");
        return;
    }
    // What the runtime would render the four views at (its own foveation settings), for comparing with this layer's.
    if (next.EnumerateViewConfigurationViews) {
        XrViewConfigurationView views[kQuadViews];
        for (auto& v : views)
            v = {XR_TYPE_VIEW_CONFIGURATION_VIEW};
        uint32_t n = 0;
        if (XR_SUCCEEDED(next.EnumerateViewConfigurationViews(instance, system, kQuad, kQuadViews, &n, views)))
            for (uint32_t v = 0; v < n && v < kQuadViews; ++v)
                logf("native quad views: the runtime recommends %ux%u for the %s (max %ux%u)",
                     views[v].recommendedImageRectWidth, views[v].recommendedImageRectHeight, view_name(v),
                     views[v].maxImageRectWidth, views[v].maxImageRectHeight);
    }
    planned = true;
}

void native_quad_session_ended() {
    planned = false;
    nativeQuad.active = false;
    if (nativeQuad.focusFrames || nativeQuad.copyFrames)
        logf("native quad views: %llu frames with the focus views as views 2-3, %llu with copies of the eyes, %llu "
             "projection layers left out",
             static_cast<unsigned long long>(nativeQuad.focusFrames),
             static_cast<unsigned long long>(nativeQuad.copyFrames),
             static_cast<unsigned long long>(nativeQuad.refusedLayers));
    nativeQuad.focusFrames = nativeQuad.copyFrames = nativeQuad.refusedLayers = 0;
    nativeQuad.loggedFirstFocus = nativeQuad.loggedFirstCopy = false;
}

}  // namespace simqvfr::layer
