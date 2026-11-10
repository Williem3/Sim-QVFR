// Offline check of native quad views (quad/native_quad.h, layer/native_quad.cpp) against a fake runtime that offers
// PRIMARY_QUAD_VARJO: the game's stereo session is begun as a quad view one, its stereo view locations come from the
// runtime's four, its visibility mask is asked for as quad, and every frame reaches the runtime with four views per
// projection layer (copies of the eyes where the layer gave only two, cut to the runtime's focus views). A runtime that
// refuses the quad session, or a system without the configuration, leaves the session stereo. The geometry that puts
// views 2-3 over exactly the runtime's focus views (quad/native_quad.h) is checked on its own first.
#include <array>
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
    std::vector<XrCompositionLayerProjectionView> endViews;  // and those views
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
    fake.endViews.clear();
    fake.endLayers = info->layerCount;
    for (uint32_t i = 0; i < info->layerCount; ++i) {
        if (info->layers[i]->type != XR_TYPE_COMPOSITION_LAYER_PROJECTION)
            continue;
        const auto* p = reinterpret_cast<const XrCompositionLayerProjection*>(info->layers[i]);
        fake.endViewCounts.push_back(p->viewCount);
        if (fake.endSwapchains.empty())
            for (uint32_t v = 0; v < p->viewCount; ++v) {
                fake.endSwapchains.push_back(p->views[v].subImage.swapchain);
                fake.endViews.push_back(p->views[v]);
            }
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

    // The geometry (quad/native_quad.h). An eye of tangents -1..1 each way over 200 x 100 texels (at 10, 20 in its
    // image), cut to a region -0.3..0.2 across, 0.5..-0.1 up/down: texels 70..120 across and 25..55 down, on texel edges,
    // so the rect's FOV is the region's.
    {
        XrCompositionLayerProjectionView eye = eye_view(kLeft);
        eye.fov = {std::atan(-1.0f), std::atan(1.0f), std::atan(1.0f), std::atan(-1.0f)};
        eye.subImage.imageRect = {{10, 20}, {200, 100}};
        const auto tangents = [](const XrFovf& f) {
            return std::array<double, 4>{std::tan(f.angleLeft), std::tan(f.angleRight), std::tan(f.angleUp),
                                         std::tan(f.angleDown)};
        };
        XrCompositionLayerProjectionView cut{};
        const float region[4] = {-0.3f, 0.2f, 0.5f, -0.1f};
        bool ok = crop_to_region(eye, region, cut);
        auto t = tangents(cut.fov);
        std::printf("     cut: %d,%d %dx%d, tangents %.4f %.4f %.4f %.4f\n", cut.subImage.imageRect.offset.x,
                    cut.subImage.imageRect.offset.y, cut.subImage.imageRect.extent.width,
                    cut.subImage.imageRect.extent.height, t[0], t[1], t[2], t[3]);
        check(ok && cut.subImage.imageRect.offset.x == 80 && cut.subImage.imageRect.offset.y == 45 &&
                  cut.subImage.imageRect.extent.width == 50 && cut.subImage.imageRect.extent.height == 30 &&
                  cut.subImage.swapchain == kLeft,
              "crop_to_region: the region's texels of the eye's image");
        check(std::fabs(t[0] + 0.3) < 1e-5 && std::fabs(t[1] - 0.2) < 1e-5 && std::fabs(t[2] - 0.5) < 1e-5 &&
                  std::fabs(t[3] + 0.1) < 1e-5,
              "crop_to_region: on texel edges, the rect's FOV is the region's");
        // Off the texel edges: out to the next ones, covering the region within a texel (0.01 tangents across, 0.02
        // up/down).
        const float between[4] = {-0.305f, 0.207f, 0.513f, -0.1f};
        ok = crop_to_region(eye, between, cut);
        t = tangents(cut.fov);
        check(ok && t[0] <= -0.305 + 1e-6 && t[0] > -0.305 - 0.01 && t[1] >= 0.207 - 1e-6 && t[1] < 0.207 + 0.01 &&
                  t[2] >= 0.513 - 1e-6 && t[2] < 0.513 + 0.02 && cut.subImage.imageRect.extent.width == 52,
              "crop_to_region: off the texel edges, out to the next ones");
        // Partly past the eye: cut at its edge. Wholly past it: no cut.
        const float edge[4] = {0.9f, 1.5f, 0.2f, -0.2f}, past[4] = {1.2f, 1.5f, 0.2f, -0.2f};
        ok = crop_to_region(eye, edge, cut);
        check(ok && cut.subImage.imageRect.offset.x == 200 && cut.subImage.imageRect.extent.width == 10 &&
                  std::fabs(tangents(cut.fov)[1] - 1.0) < 1e-5,
              "crop_to_region: a region past the eye's edge cut at it");
        check(!crop_to_region(eye, past, cut), "crop_to_region: a region wholly past the eye: none");

        // The race frames' image: a box of tangents +-0.4 over 80 x 80 texels (100 per tangent), a region a little
        // off it: the box's grid out to whole texels over the region, the box at whole texels in it.
        const float box[4] = {-0.4f, 0.4f, 0.4f, -0.4f}, around[4] = {-0.453f, 0.351f, 0.402f, -0.399f};
        RegionImage image;
        ok = focus_region_image(box, 80, 80, around, image);
        std::printf("     region image %dx%d, box at %d,%d, tangents %.4f %.4f %.4f %.4f\n", image.width, image.height,
                    image.boxX, image.boxY, image.tan[0], image.tan[1], image.tan[2], image.tan[3]);
        check(ok && image.width == 82 && image.height == 81 && image.boxX == 6 && image.boxY == 1,
              "focus_region_image: the box's grid over the region, the box 6 texels across and 1 down in it");
        check(image.tan[0] <= around[0] && image.tan[1] >= around[1] && image.tan[2] >= around[2] &&
                  image.tan[3] <= around[3] && std::fabs((image.tan[1] - image.tan[0]) * 100 - image.width) < 1e-3 &&
                  std::fabs((image.tan[0] - box[0]) * 100 + image.boxX) < 1e-3,
              "focus_region_image: it covers the region at the box's density, on its texel edges");
        // A region inside the box (the box larger than the runtime's focus views): the box reaches out of it.
        const float inside[4] = {-0.2f, 0.2f, 0.2f, -0.2f};
        ok = focus_region_image(box, 80, 80, inside, image);
        check(ok && image.width == 40 && image.height == 40 && image.boxX == -20 && image.boxY == -20,
              "focus_region_image: a region inside the box, the box beyond the image");
        const float wide[4] = {-0.5f, 0.5f, 0.5f, -0.5f};
        check(std::fabs(box_share_in_region(box, inside) - 0.25) < 1e-6 &&
                  std::fabs(box_share_in_region(box, wide) - 1.0) < 1e-6 &&
                  std::fabs(box_share_in_region(box, around) - (0.751 / 0.8) * (0.799 / 0.8)) < 1e-4,
              "box_share_in_region: a quarter inside, all inside, a strip outside");

        // The runtime's focus view as its eye sees it: unturned, its own tangents; turned 10 degrees about the
        // vertical, the bounding box of its corners, shifted that way and wider.
        const XrQuaternionf identity{0, 0, 0, 1};
        const XrFovf narrow{std::atan(-0.3f), std::atan(0.3f), std::atan(0.3f), std::atan(-0.3f)};
        XrPosef focusPose{identity, {0, 0, 0}};
        FocusRegion r;
        ok = focus_region(identity, focusPose, narrow, r);
        check(ok && !r.turned && std::fabs(r.tan[0] + 0.3f) < 1e-6f && std::fabs(r.tan[2] - 0.3f) < 1e-6f,
              "focus_region: an unturned focus view, its own tangents");
        const float half = 0.5f * 10.0f * 3.14159265f / 180;
        focusPose.orientation = {0, std::sin(half), 0, std::cos(half)};  // turned left (towards -x)
        ok = focus_region(identity, focusPose, narrow, r);
        std::printf("     turned: %.4f %.4f %.4f %.4f\n", r.tan[0], r.tan[1], r.tan[2], r.tan[3]);
        check(ok && r.turned && r.tan[1] - r.tan[0] > 0.6f && r.tan[0] + r.tan[1] < -0.3f && r.tan[2] > 0.3f,
              "focus_region: a turned focus view, its corners' bounding box");
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

        // The runtime's focus views for that frame (tangents -0.1..0.5 each way), and the latest for another.
        float regions[2][4];
        bool exact = false;
        check(runtime_focus_regions(1000, regions, &exact) && exact && std::fabs(regions[0][0] + 0.1f) < 1e-5f &&
                  std::fabs(regions[0][1] - 0.5f) < 1e-5f && std::fabs(regions[1][2] - 0.5f) < 1e-5f &&
                  std::fabs(regions[1][3] + 0.1f) < 1e-5f,
              "the runtime's focus views as located for the frame, in each eye's frame");
        check(runtime_focus_regions(999, regions, &exact) && !exact, "another frame's: the latest located");

        // A menu frame at that time: views 2-3 its eyes' images cut to the runtime's focus views, the FOV theirs
        // within a texel of its eye (1000 x 900 over tangents about +-1.26).
        XrCompositionLayerProjectionView menuEyes[2] = {eye_view(kLeft), eye_view(kRight)};
        for (int e = 0; e < 2; ++e) {
            menuEyes[e].fov = views[e].fov;
            menuEyes[e].subImage.imageRect = {{0, 0}, {1000, 900}};
        }
        XrCompositionLayerProjection menu{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
        menu.viewCount = 2;
        menu.views = menuEyes;
        const XrCompositionLayerBaseHeader* menuOnly[] = {reinterpret_cast<const XrCompositionLayerBaseHeader*>(&menu)};
        XrFrameEndInfo menuEnd{XR_TYPE_FRAME_END_INFO};
        menuEnd.displayTime = 1000;
        menuEnd.layerCount = 1;
        menuEnd.layers = menuOnly;
        const uint64_t croppedBefore = nativeQuad.croppedFrames;
        bool covered = XR_SUCCEEDED(next.EndFrame(kSession, &menuEnd)) && fake.endViews.size() == 4 &&
                       nativeQuad.croppedFrames == croppedBefore + 1;
        for (int v = 2; covered && v < 4; ++v) {
            const XrFovf& f = fake.endViews[v].fov;
            const double l = std::tan(f.angleLeft), r = std::tan(f.angleRight), u = std::tan(f.angleUp),
                         d = std::tan(f.angleDown);
            std::printf("     view %d: %d,%d %dx%d, tangents %.4f %.4f %.4f %.4f\n", v,
                        fake.endViews[v].subImage.imageRect.offset.x, fake.endViews[v].subImage.imageRect.offset.y,
                        fake.endViews[v].subImage.imageRect.extent.width,
                        fake.endViews[v].subImage.imageRect.extent.height, l, r, u, d);
            covered &= l <= -0.1 + 1e-5 && l > -0.1 - 0.003 && r >= 0.5 - 1e-5 && r < 0.5 + 0.003 &&
                       u >= 0.5 - 1e-5 && u < 0.5 + 0.003 && d <= -0.1 + 1e-5 && d > -0.1 - 0.003 &&
                       fake.endViews[v].subImage.swapchain == (v == 2 ? kLeft : kRight);
        }
        check(covered, "a menu frame: views 2-3 cut to the runtime's focus views, at their FOV within a texel");

        // The gaze off (a fixed focus box): the runtime asked not to move its focus views.
        experiment.gaze = false;
        count = 0;
        check(XR_SUCCEEDED(next.LocateViews(kSession, &locate, &state, 2, &count, views)) && !fake.foveatedAsked,
              "the gaze off: the runtime's foveated rendering not asked for");
        experiment.gaze = true;
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
