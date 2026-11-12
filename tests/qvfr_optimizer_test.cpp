// Offline check of the settings menu's QVFR optimizer (quad/qvfr_optimizer.h): which values give for a
// pixel count, and the search against a simulated GPU whose time is fixed + per pixel.
#include <cstdio>

#include "quad/qvfr_optimizer.h"

using namespace simqvfr;

namespace {

int failures = 0;

void check(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    failures += ok ? 0 : 1;
}

// A GPU whose frame time is `fixed` + `perPixel` x qvfr_pixels, against a 90 Hz budget.
struct Sim {
    double fixed = 3, perPixel = 0;
    QvfrValues values;
    bool cpuBound = false;
    double gpu() const { return fixed + perPixel * qvfr_pixels(values); }
};

// Feeds windows until the optimizer stops (or a window limit); applies what it asks for. Returns the windows fed.
int run(QvfrOptimizer& o, Sim& sim, int* applies = nullptr) {
    int n = 0;
    for (; n < 200 && o.running(); ++n) {
        const QvfrAction a = o.feed({sim.gpu(), 4, 11.111, sim.cpuBound, 45});
        if (a.apply) {
            sim.values = a.values;
            if (applies)
                ++*applies;
        }
    }
    return n;
}

}  // namespace

int SIM_QVFR_TEST_MAIN() {
    const QvfrValues start{60, 120, 35, 35};
    const QvfrLimits limits;
    {
        const double p = qvfr_pixels(start);
        check(p > 1.07 && p < 1.08, "pixels: two 60% eyes and two 120% x 35% boxes are about 1.07 native eyes");
        check(qvfr_values_for(p, start, limits) == start, "the start's own pixel count gives the start");
    }
    {
        const QvfrValues v = qvfr_values_for(qvfr_pixels(start) * 0.8, start, limits);
        check(v.periphery < 60 && v.density == 120 && v.width == 35, "a little less: only the periphery gives");
        check(qvfr_pixels(v) <= qvfr_pixels(start) * 0.8 + 1e-9, "and the result stays at or under the count");
    }
    {
        const QvfrValues v = qvfr_values_for(0.35, start, limits);
        check(v.periphery == 25 && v.density < 120 && v.density >= 80 && v.width == 35,
              "more: the periphery at its floor, then the density gives");
    }
    {
        const QvfrValues v = qvfr_values_for(0.05, start, limits);
        check(v.periphery == 25 && v.density == 80 && v.width < 35 && v.width >= 25 && v.height == v.width,
              "much more: the box shrinks last, evenly, down to its floor");
        const QvfrValues none = qvfr_values_for(0, start, limits);
        check(none.periphery == 25 && none.density == 80 && none.width == 25, "nothing: everything at its floor");
    }
    {
        QvfrLimits l;
        l.ceiling = {70, 150, 100, 100};
        const QvfrValues v = qvfr_values_for(qvfr_pixels(start) * 1.1, start, l);
        check(v.density > 120 && v.periphery == 60 && v.width == 35, "headroom: the focus density rises first, box kept");
        const QvfrValues all = qvfr_values_for(10, start, l);
        check(all.density == 150 && all.periphery == 70, "lots of headroom: up to the ceilings, no further");
        l.nativeWidth = 4000;
        l.maxTargetWidth = 8192;
        const QvfrValues wide = qvfr_values_for(10, start, l);
        const double target = 2.0 * 4000 * (wide.periphery / 100.0 + wide.density / 100.0 * wide.width / 100.0);
        check(target <= 8192, "raising keeps ACE's target within the runtime's width");
    }
    {
        // About 80 fps: 12.5 ms on a 11.1 ms budget.
        Sim sim;
        sim.values = start;
        sim.perPixel = (12.5 - sim.fixed) / qvfr_pixels(start);
        QvfrOptimizer o;
        o.start(start, limits);
        int applies = 0;
        run(o, sim, &applies);
        const double target = QvfrOptimizer::kTargetShare * 11.111;
        check(o.state() == QvfrState::Done, "over budget: it finishes");
        check(sim.gpu() <= target && sim.gpu() >= target - QvfrOptimizer::kBandShare * 11.111,
              "the simulated GPU ends inside the target band");
        check(applies >= 1 && applies <= QvfrOptimizer::kMaxSteps, "in a few steps");
        check(sim.values.width == 35 && sim.values.density <= 120, "the box is kept when lowering the periphery suffices");
        const QvfrAction back = o.undo();
        check(back.apply && back.values == start && o.state() == QvfrState::Idle, "undo goes back to the start");
    }
    {
        // A first guess far off (almost all of it fixed): the fitted line still gets there.
        Sim sim;
        sim.values = start;
        sim.fixed = 9;
        sim.perPixel = (12.0 - sim.fixed) / qvfr_pixels(start);
        QvfrOptimizer o;
        o.start(start, limits);
        run(o, sim);
        const double target = QvfrOptimizer::kTargetShare * 11.111;
        check(o.state() == QvfrState::Done && sim.gpu() <= target, "mostly fixed cost: it still ends under the target");
    }
    {
        Sim sim;
        sim.values = start;
        sim.perPixel = (9.4 - sim.fixed) / qvfr_pixels(start);
        QvfrOptimizer o;
        o.start(start, limits);
        int applies = 0;
        run(o, sim, &applies);
        check(o.state() == QvfrState::Done && applies == 0, "already in the band: done, nothing changed");
    }
    {
        Sim sim;
        sim.values = start;
        sim.perPixel = (5.0 - sim.fixed) / qvfr_pixels(start);
        QvfrLimits l;
        l.ceiling = {100, 200, 100, 100};
        QvfrOptimizer o;
        o.start(start, l);
        run(o, sim);
        check(sim.values.density > start.density && sim.gpu() <= QvfrOptimizer::kTargetShare * 11.111,
              "headroom: it raises the values and stays under the target");
    }
    {
        Sim sim;
        sim.values = start;
        sim.perPixel = (40.0 - sim.fixed) / qvfr_pixels(start);
        QvfrOptimizer o;
        o.start(start, limits);
        run(o, sim);
        check(o.state() == QvfrState::Done && sim.values.periphery == 25 && sim.values.density == 80 &&
                  o.status().find(L"floors") != std::wstring::npos,
              "far too slow: it stops at the floors and says so");
    }
    {
        Sim sim;
        sim.values = start;
        sim.perPixel = (12.5 - sim.fixed) / qvfr_pixels(start);
        sim.cpuBound = true;
        QvfrOptimizer o;
        o.start(start, limits);
        int applies = 0;
        run(o, sim, &applies);
        check(o.state() == QvfrState::Failed && applies == 0 && o.status().find(L"CPU") != std::wstring::npos,
              "CPU limited: nothing changes, and it says why");
    }
    {
        QvfrOptimizer o;
        o.start(start, limits);
        for (int i = 0; i < QvfrOptimizer::kMeasureWindows; ++i)
            o.feed({-1, -1, 11.111, false, 0});
        check(o.state() == QvfrState::Failed, "no GPU timing: it fails instead of guessing");
        o.start(start, limits);
        o.cancel();
        check(o.state() == QvfrState::Idle && !o.running(), "cancel stops it");
    }
    std::printf("%s\n", failures ? "FAILED" : "all passed");
    return failures ? 1 : 0;
}
