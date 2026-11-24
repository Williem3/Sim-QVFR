// The submission's trace (quad/submit_trace.h): the layer's last wrappers before whatever is below it, under eye off's.
// For reports of UI drifting on a motion rig: which motion compensator is in the process (a layer below this one, or
// a module loaded another way), and each new shape of frame handed down, with every layer's space and pose. Info
// level, a handful of lines a session: a frame's shape is logged once, the first kTraceShapes of a session.
#include <algorithm>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <windows.h>
#include <psapi.h>

#include "log.h"
#include "quad/submit_trace.h"
#include "layer/layer.h"

namespace simqvfr::layer {

namespace {

constexpr size_t kTraceShapes = 24;

struct SubmitTrace {
    PFN_xrEndFrame endFrame = nullptr;
    PFN_xrCreateReferenceSpace createReferenceSpace = nullptr;
    PFN_xrDestroySpace destroySpace = nullptr;
    std::mutex mutex;  // the spaces: created and destroyed on any thread
    std::unordered_map<XrSpace, TracedSpace> spaces;
    // The thread that submits frames only:
    XrSession session = XR_NULL_HANDLE;
    std::vector<uint64_t> shapes;  // the session's frame shapes logged
    bool full = false;
} trace;

// The loaded modules a motion compensator goes by (file names only: a path can hold the player's name).
std::string compensator_modules() {
    std::vector<HMODULE> modules(1024);
    DWORD bytes = 0;
    if (!K32EnumProcessModules(GetCurrentProcess(), modules.data(), static_cast<DWORD>(modules.size() * sizeof(HMODULE)),
                               &bytes))
        return {};
    modules.resize(std::min<size_t>(modules.size(), bytes / sizeof(HMODULE)));
    std::string found;
    for (HMODULE m : modules) {
        char path[MAX_PATH] = {};
        const DWORD n = GetModuleFileNameA(m, path, MAX_PATH);
        if (n == 0 || n >= MAX_PATH)
            continue;
        const char* name = path;
        for (const char* c = path; *c; ++c)
            if (*c == '\\' || *c == '/')
                name = c + 1;
        if (motion_compensation_name(name))
            found += (found.empty() ? "" : ", ") + std::string(name);
    }
    return found;
}

void log_compensators() {
    const std::string below = motion_compensation_among(layersBelow);
    const std::string modules = compensator_modules();
    if (below.empty() && modules.empty()) {
        log_line("motion compensation: none found (no layer below this one and no loaded module by a known name)");
        return;
    }
    logf("motion compensation: layers below this one: [%s]; loaded modules: [%s]%s", below.c_str(), modules.c_str(),
         below.empty() ? " (not below this layer: above it, or not an OpenXR layer)" : "");
}

XrResult XRAPI_CALL traceCreateReferenceSpace(XrSession session, const XrReferenceSpaceCreateInfo* info, XrSpace* space) {
    const XrResult r = trace.createReferenceSpace(session, info, space);
    if (XR_SUCCEEDED(r) && info && space) {
        std::lock_guard guard(trace.mutex);
        trace.spaces[*space] = {info->referenceSpaceType, false, !pose_is_identity(info->poseInReferenceSpace)};
    }
    return r;
}

XrResult XRAPI_CALL traceDestroySpace(XrSpace space) {
    {
        std::lock_guard guard(trace.mutex);
        trace.spaces.erase(space);
    }
    return trace.destroySpace(space);
}

XrResult XRAPI_CALL traceEndFrame(XrSession session, const XrFrameEndInfo* info) {
    if (session != trace.session) {
        trace.session = session;
        trace.shapes.clear();
        trace.full = false;
        log_compensators();  // at the session's first frame: every layer and helper is loaded by then
    }
    if (info && !trace.full) {
        const uint64_t shape = submit_signature(*info);
        if (std::find(trace.shapes.begin(), trace.shapes.end(), shape) == trace.shapes.end()) {
            trace.shapes.push_back(shape);
            std::vector<std::string> lines;
            {
                std::lock_guard guard(trace.mutex);
                lines = submit_lines(*info, [](XrSpace space) {
                    const auto it = trace.spaces.find(space);
                    return it == trace.spaces.end() ? TracedSpace{} : it->second;
                });
            }
            logf("submitted: a new frame shape at frame %llu (display time %lld): %u layers, bottom first",
                 static_cast<unsigned long long>(endFrames), static_cast<long long>(info->displayTime), info->layerCount);
            for (const std::string& line : lines)
                logf("submitted: %s", line.c_str());
            if (trace.shapes.size() >= kTraceShapes) {
                trace.full = true;
                logf("submitted: %zu frame shapes logged; no more this session", trace.shapes.size());
            }
        }
    }
    return trace.endFrame(session, info);
}

}  // namespace

void submit_trace_install() {
    if (!next.EndFrame || !next.CreateReferenceSpace || !next.DestroySpace || next.EndFrame == traceEndFrame)
        return;  // (each instance loads `next` anew)
    trace.endFrame = next.EndFrame;
    trace.createReferenceSpace = next.CreateReferenceSpace;
    trace.destroySpace = next.DestroySpace;
    next.EndFrame = traceEndFrame;
    next.CreateReferenceSpace = traceCreateReferenceSpace;
    next.DestroySpace = traceDestroySpace;
}

void submit_trace_game_space(XrSpace space) {
    std::lock_guard guard(trace.mutex);
    const auto it = trace.spaces.find(space);
    if (it != trace.spaces.end())
        it->second.game = true;
}

void submit_trace_session_ended() {
    trace.session = XR_NULL_HANDLE;
    trace.shapes.clear();
    trace.full = false;
}

}  // namespace simqvfr::layer
