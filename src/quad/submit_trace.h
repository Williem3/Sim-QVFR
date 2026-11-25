// What the layer hands to whatever is below it at xrEndFrame, in words, for the log (layer/submit_trace.cpp): each
// composition layer's kind, the space it is placed in (head-locked VIEW, LOCAL, STAGE; the game's or this layer's own)
// and its pose. A motion compensator below this layer (OpenXR Motion Compensation, Qubic's VR HeadWay) rewrites
// poses by the layer's space, so a report of UI that drifts on a motion rig is read from these lines. Also the names
// a motion compensator's layer or module goes by. Pure: OpenXR structs only, no runtime or device.
#pragma once

#include <cctype>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "../xr.h"

namespace simqvfr {

// A space as the trace knows it: `type` 0 for one it never saw created as a reference space (an action space, or one
// made before the layer loaded).
struct TracedSpace {
    XrReferenceSpaceType type = static_cast<XrReferenceSpaceType>(0);
    bool game = false;    // created by the game (through the layer's hook); false: by this layer itself
    bool offset = false;  // created with a pose other than identity in its reference space
};

inline const char* traced_space_name(XrReferenceSpaceType type) {
    switch (static_cast<int>(type)) {
    case 0: return "not a reference space seen created";
    case XR_REFERENCE_SPACE_TYPE_VIEW: return "VIEW";
    case XR_REFERENCE_SPACE_TYPE_LOCAL: return "LOCAL";
    case XR_REFERENCE_SPACE_TYPE_STAGE: return "STAGE";
    default: return "another reference space";
    }
}

inline bool pose_is_identity(const XrPosef& p) {
    // An all-zero orientation counts: some games leave it unset (runtimes take it as identity or refuse the call).
    return p.position.x == 0 && p.position.y == 0 && p.position.z == 0 && p.orientation.x == 0 && p.orientation.y == 0 &&
           p.orientation.z == 0 && (p.orientation.w == 1 || p.orientation.w == 0);
}

// Whether `name` (an OpenXR layer's name, or a loaded module's file name) is a motion compensator's as far as they
// are known: OpenXR Motion Compensation's layer and DLL, and the words Qubic / Motion Systems' components go by
// (their OpenXR part's names are not published). Case-insensitive, anywhere in the name.
inline bool motion_compensation_name(const char* name) {
    static const char* const kWords[] = {"motion_compensation", "motioncompensation", "oxrmc",   "headway",
                                         "forceseat",           "qubic",              "motionsystems"};
    if (!name)
        return false;
    std::string lower(name);
    for (char& c : lower)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    for (const char* word : kWords)
        if (lower.find(word) != std::string::npos)
            return true;
    return false;
}

// The motion compensators among `names` ("A, B, C", as the layer logs the layers below it), comma-separated; empty: none.
inline std::string motion_compensation_among(const std::string& names) {
    std::string found;
    size_t at = 0;
    while (at <= names.size()) {
        size_t end = names.find(',', at);
        if (end == std::string::npos)
            end = names.size();
        size_t first = at, last = end;
        while (first < last && names[first] == ' ')
            ++first;
        while (last > first && names[last - 1] == ' ')
            --last;
        const std::string name = names.substr(first, last - first);
        if (motion_compensation_name(name.c_str()))
            found += (found.empty() ? "" : ", ") + name;
        at = end + 1;
    }
    return found;
}

// The frame's shape: its layers' kinds, spaces, flags, view counts and eyes, not their poses or images (which change
// every frame). Two frames with the same value read the same in submit_lines but for the poses.
inline uint64_t submit_signature(const XrFrameEndInfo& info) {
    uint64_t h = 1469598103934665603ull;  // FNV-1a
    const auto mix = [&h](uint64_t v) {
        for (int i = 0; i < 8; ++i) {
            h ^= (v >> (8 * i)) & 0xff;
            h *= 1099511628211ull;
        }
    };
    mix(info.layerCount);
    for (uint32_t i = 0; i < info.layerCount; ++i) {
        const XrCompositionLayerBaseHeader* l = info.layers ? info.layers[i] : nullptr;
        if (!l) {
            mix(0);
            continue;
        }
        mix(static_cast<uint64_t>(l->type));
        mix(static_cast<uint64_t>(l->layerFlags));
        mix(static_cast<uint64_t>(reinterpret_cast<uintptr_t>(l->space)));
        if (l->type == XR_TYPE_COMPOSITION_LAYER_PROJECTION)
            mix(reinterpret_cast<const XrCompositionLayerProjection*>(l)->viewCount);
        else if (l->type == XR_TYPE_COMPOSITION_LAYER_QUAD)
            mix(static_cast<uint64_t>(reinterpret_cast<const XrCompositionLayerQuad*>(l)->eyeVisibility));
    }
    return h;
}

// One line per layer of the frame, bottom layer first. `space_of(XrSpace)` gives the TracedSpace of a layer's space.
template <typename SpaceOf>
std::vector<std::string> submit_lines(const XrFrameEndInfo& info, SpaceOf&& space_of) {
    std::vector<std::string> lines;
    char text[400];
    for (uint32_t i = 0; i < info.layerCount; ++i) {
        const XrCompositionLayerBaseHeader* l = info.layers ? info.layers[i] : nullptr;
        if (!l) {
            std::snprintf(text, sizeof(text), "layer %u: null", i);
            lines.emplace_back(text);
            continue;
        }
        const TracedSpace s = space_of(l->space);
        char space[160];
        if (static_cast<int>(s.type) == 0)
            std::snprintf(space, sizeof(space), "space %p (%s)", static_cast<void*>(l->space), traced_space_name(s.type));
        else
            std::snprintf(space, sizeof(space), "space %p (%s%s, %s)", static_cast<void*>(l->space),
                          traced_space_name(s.type), s.offset ? " with an offset" : "", s.game ? "the game's" : "ours");
        if (l->type == XR_TYPE_COMPOSITION_LAYER_PROJECTION) {
            const auto* p = reinterpret_cast<const XrCompositionLayerProjection*>(l);
            if (p->viewCount && p->views) {
                const XrPosef& pose = p->views[0].pose;
                std::snprintf(text, sizeof(text),
                              "layer %u: projection, %u views, %s, flags 0x%llx, view 0 at %.3f,%.3f,%.3f facing "
                              "%.3f,%.3f,%.3f,%.3f",
                              i, p->viewCount, space, static_cast<unsigned long long>(p->layerFlags), pose.position.x,
                              pose.position.y, pose.position.z, pose.orientation.x, pose.orientation.y,
                              pose.orientation.z, pose.orientation.w);
            } else {
                std::snprintf(text, sizeof(text), "layer %u: projection, no views, %s, flags 0x%llx", i, space,
                              static_cast<unsigned long long>(p->layerFlags));
            }
        } else if (l->type == XR_TYPE_COMPOSITION_LAYER_QUAD) {
            const auto* q = reinterpret_cast<const XrCompositionLayerQuad*>(l);
            const char* eyes = q->eyeVisibility == XR_EYE_VISIBILITY_LEFT    ? "the left eye"
                               : q->eyeVisibility == XR_EYE_VISIBILITY_RIGHT ? "the right eye"
                                                                             : "both eyes";
            std::snprintf(text, sizeof(text),
                          "layer %u: quad for %s, %s, flags 0x%llx, %.3f x %.3f m at %.3f,%.3f,%.3f facing "
                          "%.3f,%.3f,%.3f,%.3f",
                          i, eyes, space, static_cast<unsigned long long>(q->layerFlags), q->size.width, q->size.height,
                          q->pose.position.x, q->pose.position.y, q->pose.position.z, q->pose.orientation.x,
                          q->pose.orientation.y, q->pose.orientation.z, q->pose.orientation.w);
        } else {
            std::snprintf(text, sizeof(text), "layer %u: type %d, %s, flags 0x%llx", i, static_cast<int>(l->type), space,
                          static_cast<unsigned long long>(l->layerFlags));
        }
        lines.emplace_back(text);
    }
    return lines;
}

}  // namespace simqvfr
