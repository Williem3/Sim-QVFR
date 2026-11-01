// Offline check of the two_loops frame pairing against a fake runtime that enforces OpenXR's frame
// rules (wait before begin, begin/end strictly alternating), for ACE's normal call order and for a
// frame generator above the layer (two ends per begin), which must never arm.
#include "../src/xr_layer.cpp"

#include <cstdio>

namespace {

struct FakeRuntime {
    int waits = 0, begins = 0, ends = 0, violations = 0;
    bool waited = false, begun = false;
    XrTime time = 1000;
} rt;

XrResult XRAPI_CALL fakeWait(XrSession, const XrFrameWaitInfo*, XrFrameState* s) {
    ++rt.waits;
    rt.waited = true;
    s->predictedDisplayTime = rt.time += 11111;
    s->predictedDisplayPeriod = 11111;
    s->shouldRender = XR_TRUE;
    return XR_SUCCESS;
}
XrResult XRAPI_CALL fakeBegin(XrSession, const XrFrameBeginInfo*) {
    ++rt.begins;
    if (!rt.waited || rt.begun) ++rt.violations;
    rt.waited = false;
    rt.begun = true;
    return XR_SUCCESS;
}
XrResult XRAPI_CALL fakeEnd(XrSession, const XrFrameEndInfo* info) {
    ++rt.ends;
    if (!rt.begun) ++rt.violations;
    rt.begun = false;
    return info->displayTime == rt.time ? XR_SUCCESS : XR_ERROR_TIME_INVALID;
}

int run(int appFrames, bool frameGenerator) {
    using namespace aceqv;
    rt = FakeRuntime{};
    loops.active = loops.broken = false;
    loops.waits = loops.begins = loops.ends = 0;
    next.WaitFrame = fakeWait;
    next.BeginFrame = fakeBegin;
    next.EndFrame = fakeEnd;
    experiment.twoLoops = true;
    int errors = 0;
    for (int i = 0; i < appFrames; ++i) {
        XrFrameState state{XR_TYPE_FRAME_STATE};
        layerWaitFrame(nullptr, nullptr, &state);
        layerBeginFrame(nullptr, nullptr);
        XrFrameEndInfo end{XR_TYPE_FRAME_END_INFO};
        end.displayTime = state.predictedDisplayTime;
        errors += XR_FAILED(layerEndFrame(nullptr, &end));
        if (frameGenerator)  // a generated frame submitted between app frames
            errors += XR_FAILED(layerEndFrame(nullptr, &end));
    }
    return errors;
}

}  // namespace

int main() {
    const int errors = run(130, false);
    std::printf("ACE order: runtime waits=%d begins=%d ends=%d violations=%d endErrors=%d active=%d\n", rt.waits,
                rt.begins, rt.ends, rt.violations, errors, aceqv::loops.active ? 1 : 0);
    // 30 unpaired frames, then 100 app frames paired into 50 display frames
    bool ok = rt.waits == 80 && rt.begins == 80 && rt.ends == 80 && rt.violations == 0 && errors == 0;

    run(130, true);
    std::printf("frame generator above: active=%d broken=%d (expected 0 and 1)\n", aceqv::loops.active ? 1 : 0,
                aceqv::loops.broken ? 1 : 0);
    ok &= !aceqv::loops.active && aceqv::loops.broken;
    std::printf("RESULT: %s\n", ok ? "pass" : "FAIL");
    return ok ? 0 : 1;
}
