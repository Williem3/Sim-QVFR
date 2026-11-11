// Offline check of LMU's car part tracking (games/lmu/car_motion.h): the static world left out, a part's slot kept
// frame to frame, both eye passes one frame, the frame before's matrix there only when that frame drew it, keys not
// seen for a while freed, a full table refusing.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <map>
#include <vector>

#include "games/lmu/car_motion.h"

namespace {

int failures = 0;

void check(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    failures += ok ? 0 : 1;
}

struct M {
    float m[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    explicit M(float x = 0) { m[12] = x; }  // a translation along x (row vectors: the last row)
};

bool same(const float* a, const M& b) { return a && std::memcmp(a, b.m, sizeof(b.m)) == 0; }

}  // namespace

int main() {
    using lmu::CarDrawKey;
    using lmu::CarMotion;
    int vbA = 0, vbB = 0, ib = 0;
    const CarDrawKey body{&vbA, &ib, 0, 0, 0, 300, 0}, wheel{&vbB, &ib, 0, 0, 0, 60, 0};
    CarMotion cm;
    bool moved = true;

    cm.frame_begin(1);
    check(cm.draw(body, M().m, moved) == -1 && !moved, "the identity (the static world): no slot");
    const int s = cm.draw(body, M(10).m, moved);
    check(s >= 0 && !moved && same(cm.current(s), M(10)) && !cm.previous(s), "a part's first draw: a slot, no motion yet");
    check(cm.draw(body, M(10).m, moved) == s && !moved, "the other eye's draw in the same frame: the same slot, still none");
    const int w = cm.draw(wheel, M(11).m, moved);
    check(w >= 0 && w != s, "another key: a slot of its own");

    cm.frame_begin(2);
    check(cm.draw(body, M(12).m, moved) == s && moved && same(cm.previous(s), M(10)) && same(cm.current(s), M(12)),
          "the next frame: the same slot, the frame before's matrix with this one's");
    check(cm.draw(body, M(12).m, moved) == s && moved && same(cm.previous(s), M(10)),
          "its other eye: the frame before's kept (not this frame's first eye)");

    cm.frame_begin(3);  // the wheel not drawn in frame 2: no motion for it in frame 3
    check(cm.draw(wheel, M(13).m, moved) == w && !moved && !cm.previous(w), "drawn again after a frame without it: no motion");

    lmu::CarMotionStats st = cm.stats();
    check(st.frames == 3 && st.identityDraws == 1 && st.partDraws == 6 && st.matched == 2 && st.newKeys == 3 && st.live == 2,
          "counted: 1 identity, 6 part draws, 2 with motion, 3 without (two first sights, one after a gap)");

    // Keys not drawn for longer than evictAfter (here 4 frames) go at the next sweep (every 64th frame).
    CarMotion tiny(2, 4);
    tiny.frame_begin(1);
    const int a = tiny.draw(body, M(1).m, moved);
    const int b = tiny.draw(wheel, M(2).m, moved);
    int vbC = 0;
    const CarDrawKey third{&vbC, &ib, 0, 0, 0, 30, 0};
    check(a >= 0 && b >= 0 && tiny.draw(third, M(3).m, moved) == -1 && tiny.stats().full == 1,
          "a full table: the next new key gets no slot (counted)");
    tiny.frame_begin(64);
    check(tiny.stats().evicted == 2 && tiny.stats().live == 0 && tiny.draw(third, M(3).m, moved) >= 0,
          "keys unseen for longer than the limit freed at the sweep; their slots taken again");
    check(lmu::car_matrix_identity(M().m) && !lmu::car_matrix_identity(M(0.5f).m), "the identity test");

    // Keys coming and going through a small table (collisions, deletions shifting runs back), against a plain model:
    // a held key keeps its slot, its motion is there exactly when the frame before drew it, the counts agree.
    {
        CarMotion churn(48, 3);
        struct Held {
            int slot;
            uint64_t frame;
            bool moved;
        };
        std::map<uint32_t, Held> model;  // by key id
        static int vb[16];
        uint32_t rng = 12345;
        const auto next = [&] { return rng = rng * 1664525u + 1013904223u; };
        int wrong = 0;
        uint64_t full = 0;
        for (uint64_t frame = 1; frame <= 4000; ++frame) {
            churn.frame_begin(frame);
            if (frame % 64 == 0)
                for (auto it = model.begin(); it != model.end();)
                    it = frame > it->second.frame + 3 ? model.erase(it) : std::next(it);
            const int n = 20 + static_cast<int>(next() % 20);
            for (int d = 0; d < n; ++d) {
                const uint32_t id = (next() >> 8) % 90;  // 90 keys, a table of 48: some come and go
                const CarDrawKey key{&vb[id % 16], &ib, 0, 0, id * 7, 30 + id, 0};
                M mat(static_cast<float>(frame * 100 + id));
                const int s = churn.draw(key, mat.m, moved);
                const auto it = model.find(id);
                if (it == model.end()) {
                    if (s < 0) {
                        ++full;
                        continue;
                    }
                    wrong += moved ? 1 : 0;
                    model[id] = {s, frame, false};
                    continue;
                }
                Held& h = it->second;
                if (h.frame != frame)  // its first draw this frame
                    h.moved = h.frame + 1 == frame;
                h.frame = frame;
                if (s != h.slot || moved != h.moved || !same(churn.current(s), mat))
                    ++wrong;
                if (moved) {
                    const M before(static_cast<float>((frame - 1) * 100 + id));
                    wrong += same(churn.previous(s), before) ? 0 : 1;
                } else {
                    wrong += churn.previous(s) ? 1 : 0;
                }
            }
        }
        const lmu::CarMotionStats cs = churn.stats();
        char what[200];
        std::snprintf(what, sizeof(what),
                      "4,000 frames of churn through 48 slots: %d disagreements with the model, %llu evicted, %zu held "
                      "(model %zu), %llu refused (model %llu)",
                      wrong, static_cast<unsigned long long>(cs.evicted), cs.live, model.size(),
                      static_cast<unsigned long long>(cs.full), static_cast<unsigned long long>(full));
        check(wrong == 0 && cs.live == model.size() && cs.full == full && cs.evicted > 100, what);
    }

    // The motion matrices: a part turned about y and moved between the frames, far from the world's origin (LMU's
    // tracks span kilometres); a point on it this frame times its matrix lands where that point was the frame before.
    {
        CarMotion mm;
        const auto world = [](double angle, double x, double y, double z, float (&m)[16]) {
            const float c = static_cast<float>(std::cos(angle)), s = static_cast<float>(std::sin(angle));
            const float v[16] = {c, 0, -s, 0, 0, 1, 0, 0, s, 0, c, 0, static_cast<float>(x), static_cast<float>(y),
                                 static_cast<float>(z), 1};
            std::memcpy(m, v, sizeof(v));
        };
        float before[16], now[16];
        world(0.30, -1802.0, 12.0, -13795.0, before);
        world(0.31, -1801.2, 12.0, -13794.5, now);  // ~0.9 m and a hundredth of a radian in a frame
        const CarDrawKey part{&vbA, &ib, 0, 0, 0, 90, 0}, other{&vbB, &ib, 0, 0, 0, 30, 0};
        mm.frame_begin(1);
        mm.draw(part, before, moved);
        mm.draw(other, M(3).m, moved);
        mm.frame_begin(2);
        const int s = mm.draw(part, now, moved);
        std::vector<float> d;
        const size_t moving = mm.motion_matrices(d);
        const double o[3] = {0.7, 0.4, -2.1};  // a point of the part (its own space)
        double pn[3], pb[3], got[3];
        for (int c = 0; c < 3; ++c) {
            pn[c] = o[0] * now[c] + o[1] * now[4 + c] + o[2] * now[8 + c] + now[12 + c];
            pb[c] = o[0] * before[c] + o[1] * before[4 + c] + o[2] * before[8 + c] + before[12 + c];
        }
        const float* m = d.data() + static_cast<size_t>(s) * 12;
        double err = 0;
        for (int c = 0; c < 3; ++c) {  // in float, as the vectors' shader does
            const float g = static_cast<float>(pn[0]) * m[c] + static_cast<float>(pn[1]) * m[3 + c] +
                            static_cast<float>(pn[2]) * m[6 + c] + m[9 + c];
            got[c] = g;
            err = (std::max)(err, std::fabs(got[c] - pb[c]));
        }
        const int o2 = 1 - s;  // the other part's slot (0 or 1), not drawn in frame 2
        const float* id = d.data() + static_cast<size_t>(o2 >= 0 ? o2 : 0) * 12;
        const bool identity = id[0] == 1 && id[4] == 1 && id[8] == 1 && id[9] == 0 && id[10] == 0 && id[11] == 0;
        char what[200];
        std::snprintf(what, sizeof(what),
                      "a part's motion: its point this frame lands where it was the frame before (%.4f m off, in float); "
                      "%zu slot with a motion, the part not drawn this frame the identity",
                      err, moving);
        check(s >= 0 && moving == 1 && err < 0.01 && identity, what);
    }

    std::printf("RESULT: %s\n", failures ? "FAIL" : "pass");
    return failures ? 1 : 0;
}
