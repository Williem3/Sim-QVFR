// Native quad views (quad/native_quad.h; [experiment] native_quad_views=1): on a runtime with XR_VARJO_quad_views
// (Varjo's, Pimax's), the game's stereo session runs as PRIMARY_QUAD_VARJO below this layer, so the runtime composes the
// four views itself, the focus views at their own resolution. The game and the rest of this layer keep a stereo
// session: the wrappers here, installed in `next` when the instance has the extension, translate at the layer's
// bottom edge (the session's begin, its view locations, its visibility mask, its frames).
#include <atomic>
#include <cmath>
#include <cstring>
#include <cwchar>
#include <iterator>
#include <mutex>
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

// The runtime's focus views as each eye sees them (FocusRegion), kept by display time for the frame that ends with
// it: its copies cut to them (quadEndFrame), its focus image drawn over them (views4.cpp), its focus boxes centred in
// them (views.cpp). The first located: their sizes in the log and in runtime.ini (the launcher's check).
void note_regions(XrTime time, const XrView (&four)[kQuadViews]) {
    NativeQuad::Located entry;
    entry.time = time;
    entry.valid = true;
    bool turned = false;
    for (int e = 0; e < 2; ++e) {
        FocusRegion region;
        if (!focus_region(four[e].pose.orientation, four[e + 2].pose, four[e + 2].fov, region))
            return;
        turned |= region.turned;
        std::memcpy(entry.tan[e], region.tan, sizeof(region.tan));
    }
    std::lock_guard guard(nativeQuad.regionMutex);
    nativeQuad.located[nativeQuad.locatedNext++ % std::size(nativeQuad.located)] = entry;
    if (turned && !nativeQuad.loggedTurned) {
        nativeQuad.loggedTurned = true;
        log_line("native quad views: the runtime's focus views are turned against their eyes: views 2-3 cover their "
                 "bounding box as each eye sees it");
    }
    if (!nativeQuad.loggedRegions) {
        nativeQuad.loggedRegions = true;
        // Each region's share of its eye's tangent span, the left eye's for the launcher.
        float share[2][2] = {};
        for (int e = 0; e < 2; ++e) {
            const XrFovf& f = four[e].fov;
            const float spanX = std::tan(f.angleRight) - std::tan(f.angleLeft), spanY = std::tan(f.angleUp) - std::tan(f.angleDown);
            share[e][0] = spanX > 0 ? (entry.tan[e][1] - entry.tan[e][0]) / spanX : 0;
            share[e][1] = spanY > 0 ? (entry.tan[e][2] - entry.tan[e][3]) / spanY : 0;
        }
        logf("native quad views: the runtime's focus views span %.1f%% x %.1f%% (left eye) and %.1f%% x %.1f%% (right "
             "eye) of the eyes' tangent spans; views 2-3 are submitted over exactly them, the focus box drawn inside",
             share[0][0] * 100, share[0][1] * 100, share[1][0] * 100, share[1][1] * 100);
        wchar_t value[32];
        std::swprintf(value, std::size(value), L"%.0fx%.0f", share[0][0] * 100, share[0][1] * 100);
        runtime_info(L"quad_runtime_focus", value);
    }
}

// The runtime's gaze (foveated rendering): the direction through the middle of each focus view's FOV, in its own
// frame, turned into `space`, the two eyes' averaged. Kept for the same locate's gaze lookup (runtime_gaze).
void note_runtime_gaze(XrSpace space, XrTime time, const XrView (&four)[kQuadViews]) {
    XrVector3f sum{0, 0, 0};
    float centre[2] = {};
    for (uint32_t v = 2; v < kQuadViews; ++v) {
        const XrFovf& f = four[v].fov;
        const float cx = 0.5f * (std::tan(f.angleLeft) + std::tan(f.angleRight));
        const float cy = 0.5f * (std::tan(f.angleUp) + std::tan(f.angleDown));
        if (v == 2) {
            centre[0] = cx;
            centre[1] = cy;
        }
        const float n = std::sqrt(cx * cx + cy * cy + 1);
        const XrVector3f d = rotate(four[v].pose.orientation, {cx / n, cy / n, -1 / n});
        sum = {sum.x + d.x, sum.y + d.y, sum.z + d.z};
    }
    const float n = std::sqrt(sum.x * sum.x + sum.y * sum.y + sum.z * sum.z);
    if (!(n > 1e-6f))
        return;
    std::lock_guard guard(nativeQuad.gazeMutex);
    nativeQuad.gaze = {sum.x / n, sum.y / n, sum.z / n};
    nativeQuad.gazeSpace = space;
    nativeQuad.gazeTime = time;
    if (nativeQuad.gazeFrames++ > 0 && (std::fabs(centre[0] - nativeQuad.lastCentre[0]) > 1e-4f ||
                                        std::fabs(centre[1] - nativeQuad.lastCentre[1]) > 1e-4f) &&
        nativeQuad.gazeMoves++ == 0)
        runtime_info(L"eye_tracking", L"1");  // the runtime's eye tracking moves its focus views (the launcher's line)
    nativeQuad.lastCentre[0] = centre[0];
    nativeQuad.lastCentre[1] = centre[1];
}

XrResult XRAPI_CALL quadBeginSession(XrSession session, const XrSessionBeginInfo* info) {
    if (!planned || !info || info->primaryViewConfigurationType != kStereo)
        return nativeQuad.BeginSession(session, info);
    XrSessionBeginInfo quadInfo = *info;
    quadInfo.primaryViewConfigurationType = kQuad;
    const XrResult r = nativeQuad.BeginSession(session, &quadInfo);
    if (XR_SUCCEEDED(r)) {
        nativeQuad.active = true;
        runtime_info(L"quad_views_used", L"1");  // the launcher's "the runtime composes" line
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
    // With foveated rendering the runtime puts views 2-3 where the eyes look (its own eye tracking); with the gaze off
    // (a fixed focus box) it keeps them where it puts them unfoveated, the box then centred there.
    XrViewLocateFoveatedRenderingVARJO foveated{XR_TYPE_VIEW_LOCATE_FOVEATED_RENDERING_VARJO};
    if (nativeQuad.foveated) {
        foveated.next = info->next;
        foveated.foveatedRenderingActive = experiment.gaze ? XR_TRUE : XR_FALSE;
        quadInfo.next = &foveated;
    }
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
            logf("native quad views: the runtime's %s FOV %.4f/%.4f/%.4f/%.4f (the game gets views 0-1; views 2-3 go "
                 "back over the runtime's focus views, this layer's focus box inside)",
                 view_name(v), four[v].fov.angleLeft, four[v].fov.angleRight, four[v].fov.angleUp, four[v].fov.angleDown);
    }
    for (uint32_t v = 0; v < 2; ++v) {
        views[v].pose = four[v].pose;
        views[v].fov = four[v].fov;
    }
    if (located >= kQuadViews) {
        note_regions(info->displayTime, four);
        if (nativeQuad.foveated)
            note_runtime_gaze(info->space, info->displayTime, four);
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
    // Copies (frames without focus views) cut to the runtime's focus views for this frame: a runtime may draw views 2-3
    // into its own focus region whatever FOV they carry.
    float regions[2][4];
    bool exact = false;
    const bool haveRegions = runtime_focus_regions(info->displayTime, regions, &exact);
    QuadFrame frame;
    quad_frame(*info, frame, haveRegions ? regions : nullptr);
    if (frame.widened) {
        if (frame.cropped)
            ++nativeQuad.croppedFrames;
        if (haveRegions && !exact)
            ++nativeQuad.regionMisses;
    }
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
            log_line(frame.cropped ? "native quad views: a frame without focus views (menus, loading, or composed): "
                                     "views 2-3 the eyes' images cut to the runtime's focus views"
                                   : "native quad views: a frame without focus views (menus, loading, or composed): "
                                     "views 2-3 copy the eyes (no runtime focus views located yet)");
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

bool runtime_focus_regions(XrTime time, float (&tan)[2][4], bool* exact) {
    if (!nativeQuad.active)
        return false;
    std::lock_guard guard(nativeQuad.regionMutex);
    const NativeQuad::Located* found = nullptr;
    const NativeQuad::Located* latest = nullptr;
    const uint32_t n = static_cast<uint32_t>(std::size(nativeQuad.located));
    for (uint32_t i = 1; i <= n; ++i) {  // newest first
        const NativeQuad::Located& l = nativeQuad.located[(nativeQuad.locatedNext + n - i) % n];
        if (!l.valid)
            continue;
        if (!latest)
            latest = &l;
        if (l.time == time) {
            found = &l;
            break;
        }
    }
    if (!found)
        found = latest;
    if (!found)
        return false;
    std::memcpy(tan, found->tan, sizeof(tan));
    if (exact)
        *exact = found->time == time;
    return true;
}

bool runtime_gaze(XrSpace space, XrTime time, XrVector3f& direction) {
    if (!nativeQuad.active || !nativeQuad.foveated)
        return false;
    std::lock_guard guard(nativeQuad.gazeMutex);
    if (nativeQuad.gazeTime != time || nativeQuad.gazeSpace != space)
        return false;
    direction = nativeQuad.gaze;
    if (!nativeQuad.usedRuntimeGaze) {
        nativeQuad.usedRuntimeGaze = true;
        log_line("gaze: from the runtime's foveated rendering (where it puts its focus views), the eye gaze extension "
                 "giving no samples");
    }
    return true;
}

QuadOffer extension_offered(PFN_xrGetInstanceProcAddr gipa, bool runtimeBelow, const char* name) {
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
        if (std::strcmp(p.extensionName, name) == 0)
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
        logf("native quad views: %llu frames with the focus views as views 2-3, %llu with copies of the eyes (%llu cut "
             "to the runtime's focus views), %llu projection layers left out; %llu frames took the latest located focus "
             "views (none at their display time)",
             static_cast<unsigned long long>(nativeQuad.focusFrames),
             static_cast<unsigned long long>(nativeQuad.copyFrames),
             static_cast<unsigned long long>(nativeQuad.croppedFrames),
             static_cast<unsigned long long>(nativeQuad.refusedLayers),
             static_cast<unsigned long long>(nativeQuad.regionMisses));
    if (nativeQuad.gazeFrames)
        logf("native quad views: the runtime's foveated rendering moved its focus views on %llu of %llu located frames",
             static_cast<unsigned long long>(nativeQuad.gazeMoves),
             static_cast<unsigned long long>(nativeQuad.gazeFrames));
    nativeQuad.focusFrames = nativeQuad.copyFrames = nativeQuad.refusedLayers = 0;
    nativeQuad.croppedFrames = nativeQuad.regionMisses = 0;
    nativeQuad.loggedFirstFocus = nativeQuad.loggedFirstCopy = false;
    {
        std::lock_guard guard(nativeQuad.regionMutex);
        for (NativeQuad::Located& l : nativeQuad.located)
            l = {};
        nativeQuad.locatedNext = 0;
        nativeQuad.loggedRegions = nativeQuad.loggedTurned = false;
    }
    {
        std::lock_guard guard(nativeQuad.gazeMutex);
        nativeQuad.gazeFrames = nativeQuad.gazeMoves = 0;
        nativeQuad.gazeTime = 0;
        nativeQuad.gazeSpace = XR_NULL_HANDLE;
        nativeQuad.usedRuntimeGaze = false;
    }
}

}  // namespace simqvfr::layer
