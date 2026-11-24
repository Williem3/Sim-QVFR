// The core's own types to the mod ABI's plain structs (mod_api.h) and back, field by field. Core only: a mod sees just
// the C structs. Both directions live here so a field added to one side is added to the other (mod_api_test checks
// that a round trip keeps every field).
#pragma once

#include "../xr.h"

#include "../engine/dlss_settings.h"
#include "../engine/nr_settings.h"
#include "../games/adapter.h"
#include "../quad/focus_mask.h"
#include "../quad/views4_layout.h"
#include "mod_api.h"

namespace simqvfr::mod_api {

static_assert(SQVFR_FOCUS_POINTS_MAX == kFocusPointsMax, "mod_api.h's focus points follow quad/focus_shape.h");

inline int32_t to_c(bool v) { return v ? 1 : 0; }

inline SqvfrLayout to_c(const Views4Layout& l) {
    SqvfrLayout c = {};
    c.outer = l.outer;
    c.focus = l.focus;
    c.focusHeight = l.focusHeight;
    c.cropWidth = l.cropWidth;
    c.cropHeight = l.cropHeight;
    for (int v = 0; v < 4; ++v) {
        c.width[v] = l.width[v];
        c.height[v] = l.height[v];
    }
    c.layerFloor = l.layerFloor;
    c.live = to_c(l.live);
    c.sharedColumnWidth = to_c(l.sharedColumnWidth);
    c.secondViewRoom = to_c(l.secondViewRoom);
    c.secondViewBeside = to_c(l.secondViewBeside);
    return c;
}

inline Views4Layout from_c(const SqvfrLayout& c) {
    Views4Layout l;
    l.outer = c.outer;
    l.focus = c.focus;
    l.focusHeight = c.focusHeight;
    l.cropWidth = c.cropWidth;
    l.cropHeight = c.cropHeight;
    for (int v = 0; v < 4; ++v) {
        l.width[v] = c.width[v];
        l.height[v] = c.height[v];
    }
    l.layerFloor = c.layerFloor;
    l.live = c.live != 0;
    l.sharedColumnWidth = c.sharedColumnWidth != 0;
    l.secondViewRoom = c.secondViewRoom != 0;
    l.secondViewBeside = c.secondViewBeside != 0;
    return l;
}

inline SqvfrQvfrStart to_c(const games::QvfrStart& s) {
    SqvfrQvfrStart c = {};
    c.layout = to_c(s.layout);
    c.focusFraction = s.focusFraction;
    c.focusFractionY = s.focusFractionY;
    c.dlssLive = to_c(s.dlssLive);
    c.focusCull = to_c(s.focusCull);
    c.focusCullScale = s.focusCullScale;
    c.focusCullGaze = to_c(s.focusCullGaze);
    c.ini = s.ini;
    return c;
}

inline games::QvfrStart from_c(const SqvfrQvfrStart& c) {
    games::QvfrStart s;
    s.layout = from_c(c.layout);
    s.focusFraction = c.focusFraction;
    s.focusFractionY = c.focusFractionY;
    s.dlssLive = c.dlssLive != 0;
    s.focusCull = c.focusCull != 0;
    s.focusCullScale = c.focusCullScale;
    s.focusCullGaze = c.focusCullGaze != 0;
    s.ini = c.ini;
    return s;
}

inline SqvfrViews4Frame to_c(const Views4Frame& f) {
    SqvfrViews4Frame c = {};
    for (int e = 0; e < 2; ++e) {
        for (int i = 0; i < 4; ++i) {
            c.outerTan[e][i] = f.outerTan[e][i];
            c.focusTan[e][i] = f.focusTan[e][i];
        }
        c.turn[e][0] = f.turn[e].x;
        c.turn[e][1] = f.turn[e].y;
        c.turn[e][2] = f.turn[e].z;
        c.turn[e][3] = f.turn[e].w;
    }
    c.live = to_c(f.live);
    for (int v = 0; v < 4; ++v) {
        c.width[v] = f.width[v];
        c.height[v] = f.height[v];
        c.renderWidth[v] = f.renderWidth[v];
        c.renderHeight[v] = f.renderHeight[v];
    }
    c.eyesOnly = to_c(f.eyesOnly);
    return c;
}

inline Views4Frame from_c(const SqvfrViews4Frame& c) {
    Views4Frame f = {};
    for (int e = 0; e < 2; ++e) {
        for (int i = 0; i < 4; ++i) {
            f.outerTan[e][i] = c.outerTan[e][i];
            f.focusTan[e][i] = c.focusTan[e][i];
        }
        f.turn[e] = {c.turn[e][0], c.turn[e][1], c.turn[e][2], c.turn[e][3]};
    }
    f.live = c.live != 0;
    for (int v = 0; v < 4; ++v) {
        f.width[v] = c.width[v];
        f.height[v] = c.height[v];
        f.renderWidth[v] = c.renderWidth[v];
        f.renderHeight[v] = c.renderHeight[v];
    }
    f.eyesOnly = c.eyesOnly != 0;
    return f;
}

inline SqvfrFocusArea to_c(const FocusArea& a) {
    SqvfrFocusArea c = {};
    for (int i = 0; i < 2 * kFocusPointsMax; ++i)
        c.point[i] = a.point[i];
    c.points = a.points;
    c.radius = a.radius;
    c.aspect = a.aspect;
    for (int i = 0; i < 4; ++i)
        c.visible[i] = a.visible[i];
    return c;
}

inline FocusArea from_c(const SqvfrFocusArea& c) {
    FocusArea a;
    for (int i = 0; i < 2 * kFocusPointsMax; ++i)
        a.point[i] = c.point[i];
    a.points = c.points;
    a.radius = c.radius;
    a.aspect = c.aspect;
    for (int i = 0; i < 4; ++i)
        a.visible[i] = c.visible[i];
    return a;
}

// The folder string stays the caller's: valid while `d` is.
inline SqvfrDlssSettings to_c(const DlssSettings& d) {
    SqvfrDlssSettings c = {};
    c.on = to_c(d.on);
    c.folder = d.folder.c_str();
    c.focusScale = d.focusScale;
    c.focusPreset = d.focusPreset;
    c.eyePreset = d.eyePreset;
    c.textureSharpening = d.textureSharpening;
    c.carVectors = to_c(d.carVectors);
    c.upscaler = d.upscaler;
    return c;
}

inline DlssSettings from_c(const SqvfrDlssSettings& c) {
    DlssSettings d;
    d.on = c.on != 0;
    d.folder = c.folder ? c.folder : L"";
    d.focusScale = c.focusScale;
    d.focusPreset = c.focusPreset;
    d.eyePreset = c.eyePreset;
    d.textureSharpening = c.textureSharpening;
    d.carVectors = c.carVectors != 0;
    d.upscaler = c.upscaler;
    return d;
}

inline SqvfrNrSettings to_c(const NrSettings& n) {
    SqvfrNrSettings c = {};
    c.on = to_c(n.on);
    c.folder = n.folder.c_str();
    c.preset = n.preset;
    c.style = n.style;
    c.intensity = n.intensity;
    c.localTone = n.localTone;
    c.localStructure = n.localStructure;
    c.autoMask = to_c(n.autoMask);
    c.periphery = to_c(n.periphery);
    c.focusScale = n.focusScale;
    c.peripheryScale = n.peripheryScale;
    c.workingLimit = n.workingLimit;
    c.peripheryCoverage = n.peripheryCoverage;
    c.edgeBlend = n.edgeBlend;
    c.beforeDlss = to_c(n.beforeDlss);
    c.exposure = n.exposure;
    c.inputGamma = to_c(n.inputGamma);
    c.autoExposure = to_c(n.autoExposure);
    c.maxGain = n.maxGain;
    c.saveFrames = n.saveFrames;
    return c;
}

inline NrSettings from_c(const SqvfrNrSettings& c) {
    NrSettings n;
    n.on = c.on != 0;
    n.folder = c.folder ? c.folder : L"";
    n.preset = c.preset;
    n.style = c.style;
    n.intensity = c.intensity;
    n.localTone = c.localTone;
    n.localStructure = c.localStructure;
    n.autoMask = c.autoMask != 0;
    n.periphery = c.periphery != 0;
    n.focusScale = c.focusScale;
    n.peripheryScale = c.peripheryScale;
    n.workingLimit = c.workingLimit;
    n.peripheryCoverage = c.peripheryCoverage;
    n.edgeBlend = c.edgeBlend;
    n.beforeDlss = c.beforeDlss != 0;
    n.exposure = c.exposure;
    n.inputGamma = c.inputGamma != 0;
    n.autoExposure = c.autoExposure != 0;
    n.maxGain = c.maxGain;
    n.saveFrames = c.saveFrames;
    return n;
}

inline SqvfrLiveView to_c(const games::GameAdapter::LiveView& v) {
    SqvfrLiveView c = {};
    c.periphery = v.periphery;
    c.focusX = v.focusX;
    c.focusY = v.focusY;
    c.density = v.density;
    c.feather = v.feather;
    c.cornerRadius = v.cornerRadius;
    c.border = to_c(v.border);
    for (int i = 0; i < 4; ++i)
        c.borderColor[i] = v.borderColor[i];
    c.gaze = to_c(v.gaze);
    return c;
}

inline games::GameAdapter::LiveView from_c(const SqvfrLiveView& c) {
    games::GameAdapter::LiveView v;
    v.periphery = c.periphery;
    v.focusX = c.focusX;
    v.focusY = c.focusY;
    v.density = c.density;
    v.feather = c.feather;
    v.cornerRadius = c.cornerRadius;
    v.border = c.border != 0;
    for (int i = 0; i < 4; ++i)
        v.borderColor[i] = c.borderColor[i];
    v.gaze = c.gaze != 0;
    return v;
}

// The pose and FOV of an XrView; its type and next are the caller's to keep.
inline SqvfrView to_c(const XrView& x) {
    SqvfrView c = {};
    c.orientation[0] = x.pose.orientation.x;
    c.orientation[1] = x.pose.orientation.y;
    c.orientation[2] = x.pose.orientation.z;
    c.orientation[3] = x.pose.orientation.w;
    c.position[0] = x.pose.position.x;
    c.position[1] = x.pose.position.y;
    c.position[2] = x.pose.position.z;
    c.fov[0] = x.fov.angleLeft;
    c.fov[1] = x.fov.angleRight;
    c.fov[2] = x.fov.angleUp;
    c.fov[3] = x.fov.angleDown;
    return c;
}

inline void from_c(const SqvfrView& c, XrView& x) {
    x.pose.orientation = {c.orientation[0], c.orientation[1], c.orientation[2], c.orientation[3]};
    x.pose.position = {c.position[0], c.position[1], c.position[2]};
    x.fov = {c.fov[0], c.fov[1], c.fov[2], c.fov[3]};
}

inline XrView xr_view_from_c(const SqvfrView& c) {
    XrView x = {XR_TYPE_VIEW};
    from_c(c, x);
    return x;
}

}  // namespace simqvfr::mod_api
