// Offline check of one eye off (quad/eye_off.h): the ini's values, the frame's layers as the runtime gets them with an
// eye off (stereo and quad view projection layers, quad layers for both eyes or one, other layers untouched, depth
// left out of the black views), and the headset menu's Eye off line (on the Overlay and menu page for a game with it).
#include <algorithm>
#include <cstdio>
#include <vector>

#include "quad/eye_off.h"
#include "quad/settings_menu.h"
#include "test_util.h"

using namespace simqvfr;

namespace {

XrSwapchain chain(uintptr_t n) { return reinterpret_cast<XrSwapchain>(n); }

XrCompositionLayerProjectionView view(uintptr_t swapchain, const void* next = nullptr) {
    XrCompositionLayerProjectionView v{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};
    v.next = next;
    v.subImage.swapchain = chain(swapchain);
    v.subImage.imageRect = {{100, 50}, {1000, 900}};
    v.subImage.imageArrayIndex = 1;
    v.fov = {-0.9f, 0.8f, 0.85f, -0.9f};
    return v;
}

XrCompositionLayerQuad quad(XrEyeVisibility eyes) {
    XrCompositionLayerQuad q{XR_TYPE_COMPOSITION_LAYER_QUAD};
    q.eyeVisibility = eyes;
    q.subImage.swapchain = chain(77);
    return q;
}

}  // namespace

int SIM_QVFR_TEST_MAIN() {
    check(eye_off_from_ini(0) == kEyeOffNone && eye_off_from_ini(1) == 0 && eye_off_from_ini(2) == 1 &&
              eye_off_from_ini(7) == kEyeOffNone,
          "ini values: 0 none, 1 the left eye, 2 the right, anything else none");
    check(eye_off_to_ini(kEyeOffNone) == 0 && eye_off_to_ini(0) == 1 && eye_off_to_ini(1) == 2, "and back");
    set_eye_off(1);
    check(eye_off() == 1, "the setting is kept");
    set_eye_off(5);
    check(eye_off() == kEyeOffNone, "an eye other than 0 or 1 is none");

    // A stereo projection layer with depth on both views, a four-view one (quad views), a quad for both eyes, one for
    // each eye, and a cylinder layer (passed as it is).
    XrCompositionLayerDepthInfoKHR depth{XR_TYPE_COMPOSITION_LAYER_DEPTH_INFO_KHR};
    const XrCompositionLayerProjectionView stereoViews[2] = {view(1, &depth), view(2, &depth)};
    XrCompositionLayerProjection stereo{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    stereo.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
    stereo.viewCount = 2;
    stereo.views = stereoViews;
    const XrCompositionLayerProjectionView fourViews[4] = {view(3), view(4), view(5), view(6)};
    XrCompositionLayerProjection four{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    four.viewCount = 4;
    four.views = fourViews;
    const XrCompositionLayerQuad both = quad(XR_EYE_VISIBILITY_BOTH), left = quad(XR_EYE_VISIBILITY_LEFT),
                                 right = quad(XR_EYE_VISIBILITY_RIGHT);
    XrCompositionLayerCylinderKHR cylinder{XR_TYPE_COMPOSITION_LAYER_CYLINDER_KHR};
    const XrCompositionLayerBaseHeader* layers[] = {
        reinterpret_cast<const XrCompositionLayerBaseHeader*>(&stereo),
        reinterpret_cast<const XrCompositionLayerBaseHeader*>(&four),
        reinterpret_cast<const XrCompositionLayerBaseHeader*>(&both),
        reinterpret_cast<const XrCompositionLayerBaseHeader*>(&left),
        reinterpret_cast<const XrCompositionLayerBaseHeader*>(&right),
        reinterpret_cast<const XrCompositionLayerBaseHeader*>(&cylinder)};
    XrFrameEndInfo info{XR_TYPE_FRAME_END_INFO};
    info.layerCount = static_cast<uint32_t>(std::size(layers));
    info.layers = layers;
    const XrSwapchain black = chain(99);

    // The left eye off.
    EyeOffFrame f;
    eye_off_frame(info, 0, black, 64, 64, f);
    check(f.layers.size() == 5 && f.quadsDropped == 1 && f.quadsHidden == 1 && f.blankedViews == 3,
          "left off: the left-only quad left out, the both-eyes quad for one eye, 3 views black");
    const auto* p0 = reinterpret_cast<const XrCompositionLayerProjection*>(f.layers[0]);
    check(p0 != &stereo && p0->viewCount == 2 && p0->layerFlags == stereo.layerFlags,
          "the stereo layer is a copy with its flags");
    check(p0->views[0].subImage.swapchain == black && p0->views[0].subImage.imageRect.offset.x == 0 &&
              p0->views[0].subImage.imageRect.extent.width == 64 && p0->views[0].subImage.imageArrayIndex == 0 &&
              p0->views[0].next == nullptr,
          "its left view shows the black image, whole, without its depth");
    check(p0->views[0].fov.angleLeft == stereoViews[0].fov.angleLeft, "and keeps its FOV");
    check(p0->views[1].subImage.swapchain == chain(2) && p0->views[1].next == &depth &&
              p0->views[1].subImage.imageRect.offset.x == 100,
          "its right view as the game gave it, depth and all");
    const auto* p1 = reinterpret_cast<const XrCompositionLayerProjection*>(f.layers[1]);
    check(p1->viewCount == 4 && p1->views[0].subImage.swapchain == black && p1->views[2].subImage.swapchain == black &&
              p1->views[1].subImage.swapchain == chain(4) && p1->views[3].subImage.swapchain == chain(6),
          "four views: the left eye's (0) and its focus view (2) black, the right's (1, 3) as they were");
    const auto* q = reinterpret_cast<const XrCompositionLayerQuad*>(f.layers[2]);
    check(q->type == XR_TYPE_COMPOSITION_LAYER_QUAD && q->eyeVisibility == XR_EYE_VISIBILITY_RIGHT,
          "the quad for both eyes shows in the right only");
    check(reinterpret_cast<const XrCompositionLayerQuad*>(f.layers[3])->eyeVisibility == XR_EYE_VISIBILITY_RIGHT,
          "the right eye's quad stays");
    check(f.layers[4] == reinterpret_cast<const XrCompositionLayerBaseHeader*>(&cylinder), "another layer as it is");
    check(stereoViews[0].subImage.swapchain == chain(1) && both.eyeVisibility == XR_EYE_VISIBILITY_BOTH,
          "the game's own structs untouched");

    // The right eye off.
    eye_off_frame(info, 1, black, 64, 64, f);
    p0 = reinterpret_cast<const XrCompositionLayerProjection*>(f.layers[0]);
    p1 = reinterpret_cast<const XrCompositionLayerProjection*>(f.layers[1]);
    check(f.layers.size() == 5 && p0->views[1].subImage.swapchain == black && p0->views[0].subImage.swapchain == chain(1) &&
              p1->views[1].subImage.swapchain == black && p1->views[3].subImage.swapchain == black &&
              p1->views[0].subImage.swapchain == chain(3),
          "right off: the right eye's views black (1 and 3 of four)");
    check(reinterpret_cast<const XrCompositionLayerQuad*>(f.layers[2])->eyeVisibility == XR_EYE_VISIBILITY_LEFT,
          "the both-eyes quad in the left only");

    // None: the layers as they were.
    eye_off_frame(info, kEyeOffNone, black, 64, 64, f);
    check(f.layers.size() == info.layerCount && std::equal(f.layers.begin(), f.layers.end(), layers) && f.blankedViews == 0,
          "no eye off: the game's layers themselves");

    // A projection layer of another view count: nothing known of its eyes, left as it is (copied).
    XrCompositionLayerProjection one{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    one.viewCount = 1;
    one.views = stereoViews;
    const XrCompositionLayerBaseHeader* single[] = {reinterpret_cast<const XrCompositionLayerBaseHeader*>(&one)};
    XrFrameEndInfo oneInfo = info;
    oneInfo.layerCount = 1;
    oneInfo.layers = single;
    eye_off_frame(oneInfo, 0, black, 64, 64, f);
    check(f.blankedViews == 0 &&
              reinterpret_cast<const XrCompositionLayerProjection*>(f.layers[0])->views[0].subImage.swapchain == chain(1),
          "a one-view layer untouched");

    // The headset menu: Eye off first on the Overlay and menu page for a game with it, stepped none - left - right.
    SettingsMenu menu;
    menu.page = static_cast<int>(menu_pages(menu).size()) - 1;  // the Overlay and menu page is the last
    std::vector<MenuItem> items = menu_page_items(menu);
    check(std::find(items.begin(), items.end(), MenuItem::EyeOff) == items.end(), "no Eye off line for a game without it");
    menu.launch.eyeOff = true;
    items = menu_page_items(menu);
    check(items.size() > 1 && items[1] == MenuItem::EyeOff, "Eye off is the page's first line for a game with it");
    menu.selected = 1;
    check(menu_lines(menu)[1].value == L"None", "it reads None");
    check(menu_key(menu, MenuKey::Right) && menu.values.eyeOff == 1 && menu_lines(menu)[1].value == L"Left (right eye only)",
          "Right: the left eye off");
    check(menu_key(menu, MenuKey::Right) && menu.values.eyeOff == 2, "Right again: the right eye off");
    check(!menu_key(menu, MenuKey::Right) && menu.values.eyeOff == 2, "no further");
    check(menu_key(menu, MenuKey::Left) && menu_key(menu, MenuKey::Left) && menu.values.eyeOff == 0, "Left back to none");

    std::printf(failures ? "eye_off_test: %d failure(s)\n" : "eye_off_test: ok\n", failures);
    return failures ? 1 : 0;
}
