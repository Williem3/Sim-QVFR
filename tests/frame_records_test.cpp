// Offline check of the frame records (quad/frame_records.h) that carry what each frame was rendered with to its
// composition: taken in order; a long backlog drops to the newest; a backlog lasting above the frames in flight is
// resynced (ACE: none in flight, so to the newest; ACC: its game thread a frame or so ahead, so a steady backlog of
// that many is not an offset); taking before anything new reuses the last.
#include <cstdio>

#include "quad/frame_records.h"

using namespace simqvfr;

namespace {

int failures = 0;

void check(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    failures += !ok;
}

}  // namespace

int SIM_QVFR_TEST_MAIN() {
    {
        FrameRecords<int> r(0, 3);
        int v = -1;
        check(!r.take(v) && !r.latest(v), "nothing pushed: nothing to take");
        r.push(1);
        r.push(2);
        check(r.take(v) && v == 1 && r.take(v) && v == 2, "taken in the order pushed");
        check(r.take(v) && v == 2 && r.counts().emptyTakes == 1, "taking again before a push: the last one again");
        for (int i = 3; i <= 8; ++i)
            r.push(i);
        check(r.take(v) && v == 8 && r.counts().resyncs == 1, "a backlog over the maximum drops to the newest");
        check(r.latest(v) && v == 8, "latest: the newest, without taking it");
    }
    {
        // ACE: one record left over after every composition, for kDriftTakes compositions, is an offset.
        FrameRecords<int> r(0, 3);
        int v = 0, next = 0;
        r.push(next++);
        bool drifted = false;
        uint64_t take = 0;
        for (; take < FrameRecords<int>::kDriftTakes && !drifted; ++take) {
            r.push(next++);
            r.take(v, &drifted);
        }
        check(drifted && take == FrameRecords<int>::kDriftTakes && v == next - 1,
              "none in flight: a lasting backlog of one resyncs to the newest");
    }
    {
        // ACC: the game thread one frame ahead, so one left over is the frame in flight, not an offset.
        FrameRecords<int> r(1, 4);
        int v = 0, next = 0;
        r.push(next++);
        bool drifted = false, ever = false;
        bool ordered = true;
        for (int i = 0; i < 300; ++i) {
            r.push(next++);
            r.take(v, &drifted);
            ever |= drifted;
            ordered &= v == next - 2;
        }
        check(!ever && ordered, "one in flight: a steady backlog of one is kept, each take a frame behind");
        // Two left over for a while: resynced so that one remains.
        r.push(next++);
        uint64_t behind = 0;
        for (int i = 0; i < 200 && !drifted; ++i) {
            r.push(next++);
            r.take(v, &drifted, &behind);
        }
        check(drifted && v == next - 2, "one in flight: a lasting backlog of two resyncs to one behind the newest");
    }
    std::printf("RESULT: %s\n", failures ? "FAIL" : "pass");
    return failures ? 1 : 0;
}
