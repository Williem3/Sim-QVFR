// Offline check of native quad views (quad/native_quad.h, layer/native_quad.cpp) against a fake runtime that offers
// PRIMARY_QUAD_VARJO: the game's stereo session is begun as a quad view one, its stereo view locations come from the
// runtime's four, its visibility mask is asked for as quad, and every frame reaches the runtime with four views per
// projection layer (copies of the eyes where the layer gave only two). A runtime that refuses the quad session, or a
// system without the configuration, leaves the session stereo.
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

#include "layer/layer.h"
#include "quad/native_quad.h"

using namespace simqvfr;
using namespace simqvfr::layer;

namespace {

int failures = 0;

void check(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    failures += !ok;
}

// The fake runtime's record of what reached it.
struct Fake {
    bool hasQuad = true;          // the system lists PRIMARY_QUAD_VARJO
    bool refuseQuadBegin = false; // xrBeginSession refuses it
    XrViewConfigurationType begun = XR_VIEW_CONFIGURATION_TYPE_MAX_ENUM;
    XrViewConfigurationType located = XR_VIEW_CONFIGURATION_TYPE_MAX_ENUM;
    uint32_t locateCapacity = 0;
    XrViewConfigurationType masked = XR_VIEW_CONFIGURATION_TYPE_MAX_ENUM;
    std::vector<uint32_t> endViewCounts;  // each projection layer's view count, last frame
    std::vector<XrSwapchain> endSwapchains;  // the last frame's first projection layer's views' swapchains
    uint32_t endLayers = 0;
    bool foveatedAsked = false;  // the last locate carried XrViewLocateFoveatedRenderingVARJO, active
    float innerShift = 0;        // the inner views' FOV moved this far right and up (tangents): the runtime's gaze
} fake;

XrResult XRAPI_CALL fakeEnumerateConfigs(XrInstance, XrSystemId, uint32_t capacity, uint32_t* count,
                                         XrViewConfigurationType* types) {
    const XrViewConfigurationType all[] = {XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
                                           XR_VIEW_CONFIGURATION_TYPE_PRIMARY_QUAD_VARJO};
    const uint32_t n = fake.hasQuad ? 2 : 1;
    *count = n;
    if (capacity == 0)
        return XR_SUCCESS;
    if (capacity < n)
        return XR_ERROR_SIZE_INSUFFICIENT;
    std::memcpy(types, all, n * sizeof(all[0]));
    return XR_SUCCESS;
}

XrResult XRAPI_CALL fakeGipa(XrInstance, const char* name, PFN_xrVoidFunction* function) {
    if (std::strcmp(name, "xrEnumerateViewConfigurations") == 0) {
        *function = reinterpret_cast<PFN_xrVoidFunction>(fakeEnumerateConfigs);
        return XR_SUCCESS;
    }
    *function = nullptr;
    return XR_ERROR_FUNCTION_UNSUPPORTED;
}

XrResult XRAPI_CALL fakeViews(XrInstance, XrSystemId, XrViewConfigurationType type, uint32_t capacity, uint32_t* count,
                              XrViewConfigurationView* views) {
    const uint32_t n = type == XR_VIEW_CONFIGURATION_TYPE_PRIMARY_QUAD_VARJO ? 4 : 2;
    *count = n;
    for (uint32_t v = 0; v < capacity && v < n; ++v) {
        views[v].recommendedImageRectWidth = v < 2 ? 1000 : 1200;
        views[v].recommendedImageRectHeight = v < 2 ? 900 : 1100;
    }
    return XR_SUCCESS;
}

XrResult XRAPI_CALL fakeBegin(XrSession, const XrSessionBeginInfo* info) {
    if (info->primaryViewConfigurationType == XR_VIEW_CONFIGURATION_TYPE_PRIMARY_QUAD_VARJO && fake.refuseQuadBegin)
        return XR_ERROR_VIEW_CONFIGURATION_TYPE_UNSUPPORTED;
    fake.begun = info->primaryViewConfigurationType;
    return XR_SUCCESS;
}

// Views 0-3 at distinct FOVs: the eyes wide, the inner views narrow (the runtime's own foveation).
XrResult XRAPI_CALL fakeLocate(XrSession, const XrViewLocateInfo* info, XrViewState* state, uint32_t capacity,
                               uint32_t* count, XrView* views) {
    fake.located = info->viewConfigurationType;
    fake.locateCapacity = capacity;
    fake.foveatedAsked = false;
    for (auto* s = static_cast<const XrBaseInStructure*>(info->next); s; s = s->next)
        if (s->type == XR_TYPE_VIEW_LOCATE_FOVEATED_RENDERING_VARJO)
            fake.foveatedAsked = reinterpret_cast<const XrViewLocateFoveatedRenderingVARJO*>(s)->foveatedRenderingActive;
    const bool quad = info->viewConfigurationType == XR_VIEW_CONFIGURATION_TYPE_PRIMARY_QUAD_VARJO;
    if (!quad && fake.begun == XR_VIEW_CONFIGURATION_TYPE_PRIMARY_QUAD_VARJO)
        return XR_ERROR_VALIDATION_FAILURE;  // a quad session is located as one
    const uint32_t n = quad ? 4 : 2;
    *count = n;
    if (capacity < n)
        return capacity ? XR_ERROR_SIZE_INSUFFICIENT : XR_SUCCESS;
    state->viewStateFlags = XR_VIEW_STATE_ORIENTATION_VALID_BIT | XR_VIEW_STATE_POSITION_VALID_BIT;
    for (uint32_t v = 0; v < n; ++v) {
        const float w = v < 2 ? 0.9f : 0.3f;
        views[v].fov = {-w - 0.01f * v, w, w, -w};
        if (v >= 2 && fake.foveatedAsked) {  // foveated: the inner views centred on tangents (shift, shift)
            const float s = fake.innerShift;
            views[v].fov = {std::atan(s - 0.3f), std::atan(s + 0.3f), std::atan(s + 0.3f), std::atan(s - 0.3f)};
        }
        views[v].pose.orientation = {0, 0, 0, 1};
        views[v].pose.position = {v % 2 ? 0.03f : -0.03f, 0, 0};
    }
    return XR_SUCCESS;
}

XrResult XRAPI_CALL fakeMask(XrSession, XrViewConfigurationType type, uint32_t, XrVisibilityMaskTypeKHR,
                             XrVisibilityMaskKHR*) {
    fake.masked = type;
    return XR_SUCCESS;
}

XrResult XRAPI_CALL fakeEnd(XrSession, const XrFrameEndInfo* info) {
    fake.endViewCounts.clear();
    fake.endSwapchains.clear();
    fake.endLayers = info->layerCount;
    for (uint32_t i = 0; i < info->layerCount; ++i) {
        if (info->layers[i]->type != XR_TYPE_COMPOSITION_LAYER_PROJECTION)
            continue;
        const auto* p = reinterpret_cast<const XrCompositionLayerProjection*>(info->layers[i]);
        fake.endViewCounts.push_back(p->viewCount);
        if (fake.endSwapchains.empty())
            for (uint32_t v = 0; v < p->viewCount; ++v)
                fake.endSwapchains.push_back(p->views[v].subImage.swapchain);
    }
    // A quad view session refuses projection layers that don't have four views.
    if (fake.begun == XR_VIEW_CONFIGURATION_TYPE_PRIMARY_QUAD_VARJO)
        for (uint32_t c : fake.endViewCounts)
            if (c != 4)
                return XR_ERROR_VALIDATION_FAILURE;
    return XR_SUCCESS;
}

// A fresh layer state over the fake runtime, the instance having the extension, a run that wants quad views.
void reset(bool hasQuad, bool refuseQuadBegin) {
    native_quad_session_ended();
    fake = {};
    fake.hasQuad = hasQuad;
    fake.refuseQuadBegin = refuseQuadBegin;
    next = Dispatch{};
    next.EnumerateViewConfigurationViews = fakeViews;
    next.BeginSession = fakeBegin;
    next.LocateViews = fakeLocate;
    next.EndFrame = fakeEnd;
    next.GetVisibilityMaskKHR = fakeMask;
    nativeQuad.extension = true;
    experiment.nativeQuadViews = true;
    experiment.views4 = true;
    native_quad_install(reinterpret_cast<XrInstance>(1), fakeGipa);
    native_quad_plan(reinterpret_cast<XrInstance>(1), 1);
}

const auto kSession = reinterpret_cast<XrSession>(2);
const auto kLeft = reinterpret_cast<XrSwapchain>(10), kRight = reinterpret_cast<XrSwapchain>(11),
           kFocus = reinterpret_cast<XrSwapchain>(12);

XrCompositionLayerProjectionView eye_view(XrSwapchain chain) {
    XrCompositionLayerProjectionView v{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};
    v.subImage.swapchain = chain;
    v.subImage.imageRect = {{0, 0}, {100, 90}};
    return v;
}

}  // namespace

int main() {
    // quad_frame on its own: a stereo layer widened with copies, a four-view one kept, a quad passed through, a
    // three-view one left out.
    {
        XrCompositionLayerProjectionView two[2] = {eye_view(kLeft), eye_view(kRight)};
        XrCompositionLayerProjectionView four[4] = {eye_view(kLeft), eye_view(kRight), eye_view(kFocus), eye_view(kFocus)};
        XrCompositionLayerProjection stereo{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
        stereo.viewCount = 2;
        stereo.views = two;
        XrCompositionLayerProjection quadLayer = stereo;
        quadLayer.viewCount = 4;
        quadLayer.views = four;
        XrCompositionLayerProjection three = stereo;
        three.viewCount = 3;
        XrCompositionLayerQuad overlay{XR_TYPE_COMPOSITION_LAYER_QUAD};
        const XrCompositionLayerBaseHeader* layers[] = {
            reinterpret_cast<const XrCompositionLayerBaseHeader*>(&stereo),
            reinterpret_cast<const XrCompositionLayerBaseHeader*>(&quadLayer),
            reinterpret_cast<const XrCompositionLayerBaseHeader*>(&three),
            reinterpret_cast<const XrCompositionLayerBaseHeader*>(&overlay)};
        XrFrameEndInfo info{XR_TYPE_FRAME_END_INFO};
        info.layerCount = 4;
        info.layers = layers;
        QuadFrame f;
        quad_frame(info, f);
        const auto* widened = reinterpret_cast<const XrCompositionLayerProjection*>(f.layers[0]);
        check(f.layers.size() == 3 && f.widened == 1 && f.refused == 1, "quad_frame: 2 views widened, 3 left out, 4 kept");
        check(widened->viewCount == 4 && widened->views[0].subImage.swapchain == kLeft &&
                  widened->views[1].subImage.swapchain == kRight && widened->views[2].subImage.swapchain == kLeft &&
                  widened->views[3].subImage.swapchain == kRight,
              "quad_frame: the widened layer's views 2-3 copy its eyes");
        check(f.layers[1] == layers[1] && f.layers[2] == layers[3], "quad_frame: the four-view layer and the quad as they are");
    }

    // A runtime with quad views: the session begun as one, the game still stereo.
    reset(true, false);
    XrSessionBeginInfo begin{XR_TYPE_SESSION_BEGIN_INFO};
    begin.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    check(XR_SUCCEEDED(next.BeginSession(kSession, &begin)) &&
              fake.begun == XR_VIEW_CONFIGURATION_TYPE_PRIMARY_QUAD_VARJO && nativeQuad.active,
          "a stereo xrBeginSession reaches the runtime as PRIMARY_QUAD_VARJO");

    XrViewLocateInfo locate{XR_TYPE_VIEW_LOCATE_INFO};
    locate.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    XrViewState state{XR_TYPE_VIEW_STATE};
    XrView views[2] = {{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};
    uint32_t count = 0;
    const XrResult located = next.LocateViews(kSession, &locate, &state, 2, &count, views);
    check(XR_SUCCEEDED(located) && fake.located == XR_VIEW_CONFIGURATION_TYPE_PRIMARY_QUAD_VARJO &&
              fake.locateCapacity == 4 && count == 2,
          "a stereo xrLocateViews asks the runtime for its four views and returns two");
    check(views[0].fov.angleRight > 0.85f && views[1].fov.angleRight > 0.85f && views[1].pose.position.x > 0 &&
              (state.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT),
          "the game gets the eyes (views 0-1) and the view state");
    count = 0;
    check(XR_SUCCEEDED(next.LocateViews(kSession, &locate, &state, 0, &count, nullptr)) && count == 2,
          "the count query (capacity 0) answers 2");

    check(XR_SUCCEEDED(next.GetVisibilityMaskKHR(kSession, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 1,
                                                 XR_VISIBILITY_MASK_TYPE_HIDDEN_TRIANGLE_MESH_KHR, nullptr)) &&
              fake.masked == XR_VIEW_CONFIGURATION_TYPE_PRIMARY_QUAD_VARJO,
          "the eyes' visibility mask is asked for as quad");

    // A frame without focus views (a menu): its stereo layer reaches the runtime with four views.
    XrCompositionLayerProjectionView two[2] = {eye_view(kLeft), eye_view(kRight)};
    XrCompositionLayerProjection stereo{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    stereo.viewCount = 2;
    stereo.views = two;
    XrCompositionLayerQuad overlay{XR_TYPE_COMPOSITION_LAYER_QUAD};
    const XrCompositionLayerBaseHeader* menuLayers[] = {reinterpret_cast<const XrCompositionLayerBaseHeader*>(&stereo),
                                                        reinterpret_cast<const XrCompositionLayerBaseHeader*>(&overlay)};
    XrFrameEndInfo end{XR_TYPE_FRAME_END_INFO};
    end.layerCount = 2;
    end.layers = menuLayers;
    check(XR_SUCCEEDED(next.EndFrame(kSession, &end)) && fake.endLayers == 2 && fake.endViewCounts.size() == 1 &&
              fake.endViewCounts[0] == 4 && fake.endSwapchains[2] == kLeft && fake.endSwapchains[3] == kRight &&
              nativeQuad.copyFrames == 1,
          "a menu frame: four views, 2-3 copying the eyes, accepted by the quad session");

    // A race frame: views4_submit's layer already has the focus views as 2-3; it reaches the runtime as it is.
    XrCompositionLayerProjectionView four[4] = {eye_view(kLeft), eye_view(kRight), eye_view(kFocus), eye_view(kFocus)};
    XrCompositionLayerProjection race = stereo;
    race.viewCount = 4;
    race.views = four;
    const XrCompositionLayerBaseHeader* raceLayers[] = {reinterpret_cast<const XrCompositionLayerBaseHeader*>(&race)};
    end.layerCount = 1;
    end.layers = raceLayers;
    check(XR_SUCCEEDED(next.EndFrame(kSession, &end)) && fake.endViewCounts.size() == 1 && fake.endViewCounts[0] == 4 &&
              fake.endSwapchains[2] == kFocus && nativeQuad.focusFrames == 1,
          "a race frame: the focus views as views 2-3, untouched");

    // Foveated rendering: the locate asks for it, and the runtime's focus views (moved right and up by tangent 0.2)
    // give the gaze for that same call, in its space and at its time only.
    {
        XrVector3f none{0, 0, -1};
        check(!runtime_gaze(locate.space, locate.displayTime, none), "no runtime gaze without foveated rendering");
        nativeQuad.foveated = true;
        fake.innerShift = 0.2f;
        locate.displayTime = 1000;
        count = 0;
        check(XR_SUCCEEDED(next.LocateViews(kSession, &locate, &state, 2, &count, views)) && fake.foveatedAsked,
              "foveated rendering: the locate asks the runtime to move its focus views with the eyes");
        XrVector3f gazeDir{0, 0, -1};
        const bool got = runtime_gaze(locate.space, 1000, gazeDir);
        const double tx = gazeDir.x / -gazeDir.z, ty = gazeDir.y / -gazeDir.z;
        std::printf("     runtime gaze tangents %.4f, %.4f\n", tx, ty);
        check(got && std::fabs(tx - 0.2) < 1e-3 && std::fabs(ty - 0.2) < 1e-3,
              "foveated rendering: the gaze points through the middle of the runtime's focus views");
        check(!runtime_gaze(locate.space, 999, gazeDir), "foveated rendering: another frame's lookup gets none");
        nativeQuad.foveated = false;
    }

    // An empty frame (the game's first) passes as it is.
    end.layerCount = 0;
    end.layers = nullptr;
    check(XR_SUCCEEDED(next.EndFrame(kSession, &end)) && fake.endLayers == 0, "an empty frame passes as it is");

    // A runtime that refuses the quad session: stereo, nothing translated.
    reset(true, true);
    check(XR_SUCCEEDED(next.BeginSession(kSession, &begin)) && fake.begun == XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO &&
              !nativeQuad.active,
          "quad session refused: begun as stereo instead");
    count = 0;
    check(XR_SUCCEEDED(next.LocateViews(kSession, &locate, &state, 2, &count, views)) &&
              fake.located == XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO && count == 2,
          "quad session refused: views located as stereo");
    end.layerCount = 1;
    end.layers = menuLayers;
    check(XR_SUCCEEDED(next.EndFrame(kSession, &end)) && fake.endViewCounts.size() == 1 && fake.endViewCounts[0] == 2,
          "quad session refused: frames stay stereo");

    // A system without the quad configuration: not planned, stereo.
    reset(false, false);
    check(XR_SUCCEEDED(next.BeginSession(kSession, &begin)) && fake.begun == XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO &&
              !nativeQuad.active,
          "no PRIMARY_QUAD_VARJO on the system: stereo");

    // A run that doesn't want them (native_quad_views=0) on a runtime that has them: stereo.
    reset(true, false);
    experiment.nativeQuadViews = false;
    native_quad_plan(reinterpret_cast<XrInstance>(1), 1);
    check(XR_SUCCEEDED(next.BeginSession(kSession, &begin)) && fake.begun == XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO &&
              !nativeQuad.active,
          "native_quad_views=0: stereo even where the runtime has them");

    native_quad_session_ended();
    std::printf(failures ? "native_quad_test: %d failure(s)\n" : "native_quad_test: ok\n", failures);
    return failures ? 1 : 0;
}
