// Locating views: the focus views' FOV, the submitted FOV's log, and the LocateViews hook.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <mutex>

#include "log.h"
#include "quad/focus_converge.h"
#include "quad/focus_offset.h"
#include "quad/focus_turn.h"
#include "quad/fov_crop.h"
#include "xr.h"
#include "layer/layer.h"

namespace simqvfr::layer {

namespace {

// The convergence now and then in the log: the distance, where from, and the boxes' horizontal tangents.
void convergence_note(double distance, bool fromGame, const float (&tan)[2][2]) {
    static std::uint64_t calls = 0;
    static double minD = 1e9, maxD = 0;
    minD = std::min(minD, distance);
    maxD = std::max(maxD, distance);
    if (++calls % 1800 != 1)
        return;
    logf("views4 converge: the focus boxes on a point %.2f m away (%s; %.2f-%.2f m since the last note), boxes at "
         "x tangent %.4f (left eye) and %.4f (right eye)",
         distance, fromGame ? "the game's depth at the gaze" : "[quad] converge_m", minD, maxD, tan[0][0], tan[1][0]);
    minD = 1e9;
    maxD = 0;
}

XrResult locate_views(XrSession session, const XrViewLocateInfo* info, XrViewState* state, uint32_t capacity,
                      uint32_t* countOutput, XrView* views) {
    XrResult r = next.LocateViews(session, info, state, capacity, countOutput, views);
    // The FOV crop: ACE renders, and submits, the cropped FOV (everything below follows it).
    const FovCrop crop = experiment.crop();
    const bool stereo = XR_SUCCEEDED(r) && views && countOutput && std::min(*countOutput, capacity) >= 2 &&
                        info->viewConfigurationType == XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    if (stereo)
        for (int i = 0; i < 2; ++i)
            views[i].fov = crop_fov(views[i].fov, crop, i);
    if (experiment.views4 && XR_SUCCEEDED(r) && views && countOutput) {
        game->qvfr_note_views(views, std::min(*countOutput, capacity));
        if (views4_outline_on() && *countOutput >= 2 && capacity >= 2) {
            std::lock_guard guard(v4outline.mutex);
            v4outline.fov[0] = views[0].fov;
            v4outline.fov[1] = views[1].fov;
            v4outline.fovValid = true;
        }
        if (experiment.composing() && *countOutput >= 2 && capacity >= 2) {
            {
                std::lock_guard guard(quad.mutex);
                quad.v4Fov[0] = views[0].fov;
                quad.v4Fov[1] = views[1].fov;
                if (stereo) {
                    quad.v4Crop = crop;
                    // Each eye against the head (the eyes' middle orientation): canted displays turn the eyes.
                    XrQuaternionf a = views[0].pose.orientation, b = views[1].pose.orientation;
                    if (a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w < 0)
                        b = {-b.x, -b.y, -b.z, -b.w};
                    XrQuaternionf head{a.x + b.x, a.y + b.y, a.z + b.z, a.w + b.w};
                    const float n = std::sqrt(head.x * head.x + head.y * head.y + head.z * head.z + head.w * head.w);
                    if (n > 1e-6f) {
                        head = {head.x / n, head.y / n, head.z / n, head.w / n};
                        quad.v4EyeTurn[0] = qmul(qconj(head), views[0].pose.orientation);
                        quad.v4EyeTurn[1] = qmul(qconj(head), views[1].pose.orientation);
                        // Each eye from the eyes' middle (the helmet's depth): half the IPD, in the head's frame.
                        const XrVector3f& p0 = views[0].pose.position;
                        const XrVector3f& p1 = views[1].pose.position;
                        const XrVector3f half{(p0.x - p1.x) / 2, (p0.y - p1.y) / 2, (p0.z - p1.z) / 2};
                        quad.v4EyeOffset[0] = rotate(qconj(head), half);
                        quad.v4EyeOffset[1] = {-quad.v4EyeOffset[0].x, -quad.v4EyeOffset[0].y, -quad.v4EyeOffset[0].z};
                    }
                }
                quad.v4FovValid = true;
            }
            // Gaze: where the gaze ray points in each eye's frame (canted displays rotate each eye), as
            // tangents; the focus cameras turn towards it, clamped against the FOV ACE renders with
            // (fill_focus_cameras). Untracked gaze recentres after kGazeHoldNs. The vertical offset
            // (focus_offset.h) moves the tracked gaze, or the fixed box off each eye's forward axis.
            // A head-locked call (OpenComposite's eyes-to-head query) leaves the gaze to the scene-space one
            // (gaze_skips_space).
            // A quad view session: each eye's box centred in the runtime's focus view just located (its own eye
            // tracking with the gaze on, where it keeps them with it off), so the box sits inside what the runtime
            // shows of views 2-3; the gaze dead-zone, convergence and offset don't apply (the runtime places them).
            float regions[2][4];
            bool exactRegion = false;
            const bool inRuntimeFocus = runtime_focus_regions(info->displayTime, regions, &exactRegion) && exactRegion;
            if (inRuntimeFocus) {
                float tan[2][2];
                const bool valid[2] = {true, true};
                for (int e = 0; e < 2; ++e) {
                    tan[e][0] = 0.5f * (regions[e][0] + regions[e][1]);
                    tan[e][1] = 0.5f * (regions[e][2] + regions[e][3]);
                }
                static bool logged = false;
                if (!logged) {
                    logged = true;
                    logf("native quad views: the focus boxes centred in the runtime's focus views (left eye at "
                         "tangents %.3f, %.3f; %s); the gaze dead-zone, convergence and offset don't apply",
                         tan[0][0], tan[0][1],
                         experiment.gaze ? "its eye tracking" : "the gaze off: where it keeps them unfoveated");
                }
                game->qvfr_set_gaze(tan, valid, experiment.gaze);
            }
            if (!inRuntimeFocus && !(experiment.gaze && gaze_skips_space(info->space, info->displayTime))) {
                float tan[2][2] = {};
                bool valid[2] = {};
                XrVector3f direction{0, 0, -1};
                // The eye gaze extension's gaze, else (a quad view session with foveated rendering) the runtime's.
                const bool gazing = experiment.gaze && (gaze_now(session, info->space, info->displayTime, direction) ||
                                                        runtime_gaze(info->space, info->displayTime, direction));
                steady_gaze(gazing, direction);
                const int offset = gazing ? experiment.gazeOffsetY : experiment.fixedOffsetY;
                for (int e = 0; e < 2; ++e) {
                    if (gazing) {
                        const XrVector3f d = rotate(qconj(views[e].pose.orientation), direction);
                        valid[e] = d.z < -0.1f;
                        if (valid[e]) {
                            tan[e][0] = d.x / -d.z;
                            tan[e][1] = d.y / -d.z;
                        }
                    } else {
                        valid[e] = offset != 0;
                    }
                }
                // Converging (focus_converge.h): both boxes on the point the eyes look at, the gaze (or the head's
                // forward axis) out to the fixation distance, each as its eye sees it. Without a distance, parallel.
                const double fixation = experiment.views4Converge ? game->qvfr_fixation_m() : 0.0;
                const double distance = fixation > 0 ? fixation : experiment.convergeM;
                if (distance > 0) {
                    const XrVector3f eyePos[2] = {views[0].pose.position, views[1].pose.position};
                    const XrQuaternionf eyeOri[2] = {views[0].pose.orientation, views[1].pose.orientation};
                    const XrVector3f aim = gazing ? direction : rotate_by(eyes_middle(eyeOri), XrVector3f{0, 0, -1});
                    converged_tangents(eyePos, eyeOri, eyes_middle(eyePos), aim, distance, tan, valid);
                    convergence_note(distance, fixation > 0, tan);
                }
                for (int e = 0; e < 2; ++e)
                    if (valid[e] && offset != 0)
                        tan[e][1] = static_cast<float>(offset_tan_y(tan[e][1], offset));
                game->qvfr_set_gaze(tan, valid, gazing);
            }
        }
    }
    const bool narrowThisFrame = experiment.focusOnly || (experiment.twoLoops && experiment.focusInB && loops.inB);
    if (narrowThisFrame && capacity > 0 && XR_SUCCEEDED(r)) {
        XrVector3f direction{0, 0, -1};
        const bool gazing = experiment.quad && experiment.gaze &&
                            gaze_direction(session, info->space, info->displayTime, direction);
        if (experiment.quad)
            steady_gaze(gazing, direction);
        for (uint32_t i = 0; i < *countOutput && i < capacity; ++i) {
            const XrFovf before = views[i].fov;
            double tanX = 0, tanY = 0;
            if (gazing) {  // gaze ray direction in this eye's frame (canted displays rotate each eye)
                const XrQuaternionf& q = views[i].pose.orientation;
                const XrVector3f d = rotate({-q.x, -q.y, -q.z, q.w}, direction);
                if (d.z < -0.1f) {
                    tanX = d.x / -d.z;
                    tanY = d.y / -d.z;
                }
            }
            if (experiment.quad)  // the vertical offset (focus_offset.h), on the gaze or the fixed box
                tanY = offset_tan_y(tanY, gazing ? experiment.gazeOffsetY : experiment.fixedOffsetY);
            if (experiment.quad) {
                turn_view(views[i], experiment.focusFraction, experiment.focus_y(), tanX, tanY);
            } else {
                narrow(views[i].fov.angleLeft, views[i].fov.angleRight, experiment.focusFraction, tanX);
                narrow(views[i].fov.angleDown, views[i].fov.angleUp, experiment.focus_y(), tanY);
            }
            if (!focusLogged)
                logf("experiment focus_only view=%u fov %.4f/%.4f/%.4f/%.4f -> %.4f/%.4f/%.4f/%.4f", i,
                     before.angleLeft, before.angleRight, before.angleUp, before.angleDown, views[i].fov.angleLeft,
                     views[i].fov.angleRight, views[i].fov.angleUp, views[i].fov.angleDown);
        }
        focusLogged = true;
    }
    if (capacity > 0 && sampled(++locates) && XR_SUCCEEDED(r))
        for (uint32_t i = 0; i < *countOutput && i < capacity; ++i)
            logf("xrLocateViews call=%llu config=%d time=%lld view=%u fov=%.4f/%.4f/%.4f/%.4f pos=%.4f,%.4f,%.4f",
                 static_cast<unsigned long long>(locates), info->viewConfigurationType,
                 static_cast<long long>(info->displayTime), i, views[i].fov.angleLeft, views[i].fov.angleRight,
                 views[i].fov.angleUp, views[i].fov.angleDown, views[i].pose.position.x, views[i].pose.position.y,
                 views[i].pose.position.z);
    return r;
}

}  // namespace

// views4: ACE's two "eyes" are each an outer plus a focus view wide and hold views 0+1 and 2+3 side
// by side (views4_layout.h). Submit views 0 and 1, the real eyes, as the projection views.
// Logs ACE's own submitted FOV next to the one the output is submitted with, once and on a change.
void log_submitted_fov(uint32_t eye, const XrFovf& submitted, const XrFovf& composed) {
    XrFovf& last = quad.v4LoggedGameFov[eye];
    const auto same = [](const XrFovf& a, const XrFovf& b, float eps) {
        return std::fabs(a.angleLeft - b.angleLeft) < eps && std::fabs(a.angleRight - b.angleRight) < eps &&
               std::fabs(a.angleUp - b.angleUp) < eps && std::fabs(a.angleDown - b.angleDown) < eps;
    };
    if (same(submitted, last, 0.002f))
        return;
    last = submitted;
    logf("views4: eye %u ACE submits FOV %.4f/%.4f/%.4f/%.4f, output submitted with %.4f/%.4f/%.4f/%.4f (its "
         "rendered projection)%s",
         eye, submitted.angleLeft, submitted.angleRight, submitted.angleUp, submitted.angleDown, composed.angleLeft, composed.angleRight,
         composed.angleUp, composed.angleDown, same(submitted, composed, 0.002f) ? "" : "  <-- DIFFERENT");
}

XrResult XRAPI_CALL layerLocateViews(XrSession session, const XrViewLocateInfo* info, XrViewState* state,
                                     uint32_t capacity, uint32_t* countOutput, XrView* views) {
    if (!cpu.realLocateViews)
        return locate_views(session, info, state, capacity, countOutput, views);
    const int64_t t0 = qpc_now();
    const XrResult r = locate_views(session, info, state, capacity, countOutput, views);
    cpu.locate.add(qpc_now() - t0);
    return r;
}

}  // namespace simqvfr::layer
