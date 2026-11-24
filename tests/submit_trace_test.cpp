// Offline check of the submission's trace (quad/submit_trace.h): the names a motion compensator goes by, a frame's
// shape (the same whatever the poses, another with a layer more, another space or another eye), and each layer's line
// (its kind, its space as VIEW / LOCAL, the game's or ours, its pose).
#include <cstdio>
#include <string>
#include <vector>

#include "quad/submit_trace.h"
#include "test_util.h"

using namespace simqvfr;

namespace {

XrSpace space(uintptr_t n) { return reinterpret_cast<XrSpace>(n); }

bool has(const std::string& line, const char* part) { return line.find(part) != std::string::npos; }

}  // namespace

int SIM_QVFR_TEST_MAIN() {
    check(motion_compensation_name("XR_APILAYER_NOVENDOR_motion_compensation") &&
              motion_compensation_name("XR_APILAYER_NOVENDOR_Motion_Compensation.dll"),
          "OpenXR Motion Compensation's layer and DLL, whatever the case");
    check(motion_compensation_name("QubicOpenXRProxy64.dll") && motion_compensation_name("ForceSeatPM_VR.dll") &&
              motion_compensation_name("VRHeadWay.dll"),
          "Qubic's and Motion Systems' words");
    check(!motion_compensation_name("XR_APILAYER_MBUCCHIA_toolkit") && !motion_compensation_name("d3d11.dll") &&
              !motion_compensation_name(nullptr) && !motion_compensation_name(""),
          "other layers and modules are not");
    check(motion_compensation_among("XR_APILAYER_MBUCCHIA_toolkit, XR_APILAYER_NOVENDOR_motion_compensation") ==
              "XR_APILAYER_NOVENDOR_motion_compensation",
          "the compensator among the layers below");
    check(motion_compensation_among("").empty() && motion_compensation_among("XR_APILAYER_MBUCCHIA_toolkit").empty(),
          "none among none, or among others");

    XrCompositionLayerProjectionView views[2] = {{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW},
                                                 {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}};
    views[0].pose.position = {0.1f, 1.2f, -0.3f};
    views[0].pose.orientation.w = 1;
    XrCompositionLayerProjection projection{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    projection.space = space(10);
    projection.viewCount = 2;
    projection.views = views;
    XrCompositionLayerQuad menu{XR_TYPE_COMPOSITION_LAYER_QUAD};
    menu.space = space(20);
    menu.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
    menu.size = {0.6f, 0.4f};
    menu.pose.position = {0, 0, -1.5f};
    menu.pose.orientation.w = 1;
    const XrCompositionLayerBaseHeader* layers[] = {reinterpret_cast<const XrCompositionLayerBaseHeader*>(&projection),
                                                    reinterpret_cast<const XrCompositionLayerBaseHeader*>(&menu)};
    XrFrameEndInfo frame{XR_TYPE_FRAME_END_INFO};
    frame.layers = layers;
    frame.layerCount = 2;

    const uint64_t shape = submit_signature(frame);
    views[0].pose.position.x = 0.5f;
    menu.pose.position.z = -2;
    check(submit_signature(frame) == shape, "the shape doesn't follow the poses");
    frame.layerCount = 1;
    check(submit_signature(frame) != shape, "a layer less is another shape");
    frame.layerCount = 2;
    menu.eyeVisibility = XR_EYE_VISIBILITY_LEFT;
    check(submit_signature(frame) != shape, "a quad for one eye is another shape");
    menu.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
    menu.space = space(10);
    check(submit_signature(frame) != shape, "a quad in another space is another shape");
    menu.space = space(20);
    check(submit_signature(frame) == shape, "and back");

    const auto space_of = [](XrSpace s) {
        if (s == space(10))
            return TracedSpace{XR_REFERENCE_SPACE_TYPE_LOCAL, true, true};
        if (s == space(20))
            return TracedSpace{XR_REFERENCE_SPACE_TYPE_VIEW, false, false};
        return TracedSpace{};
    };
    std::vector<std::string> lines = submit_lines(frame, space_of);
    check(lines.size() == 2, "a line per layer");
    check(lines.size() == 2 && has(lines[0], "layer 0: projection, 2 views") &&
              has(lines[0], "(LOCAL with an offset, the game's)") && has(lines[0], "view 0 at 0.500,1.200,-0.300"),
          "the projection layer: its views, the game's LOCAL space, view 0's pose");
    check(lines.size() == 2 && has(lines[1], "layer 1: quad for both eyes") && has(lines[1], "(VIEW, ours)") &&
              has(lines[1], "0.600 x 0.400 m at 0.000,0.000,-2.000"),
          "the quad: its eyes, our VIEW space, its size and pose");
    menu.space = space(30);
    menu.eyeVisibility = XR_EYE_VISIBILITY_RIGHT;
    lines = submit_lines(frame, space_of);
    check(lines.size() == 2 && has(lines[1], "quad for the right eye") && has(lines[1], "(not a reference space seen created)"),
          "a space never seen created says so");
    XrCompositionLayerBaseHeader other{static_cast<XrStructureType>(1000)};
    layers[1] = &other;
    lines = submit_lines(frame, space_of);
    check(lines.size() == 2 && has(lines[1], "layer 1: type 1000"), "another kind of layer by its type");
    layers[1] = nullptr;
    check(submit_lines(frame, space_of).size() == 2 && has(submit_lines(frame, space_of)[1], "layer 1: null"),
          "a null layer doesn't stop it");

    XrPosef identity{};
    identity.orientation.w = 1;
    XrPosef moved = identity;
    moved.position.y = 0.1f;
    check(pose_is_identity(identity) && pose_is_identity(XrPosef{}) && !pose_is_identity(moved),
          "identity (an unset orientation included) against an offset");

    std::printf(failures ? "submit_trace_test: %d failure(s)\n" : "submit_trace_test: ok\n", failures);
    return failures ? 1 : 0;
}
