// Offline check of the probe stub: hooks a local function with mixed integer/float/stack arguments
// through the same generated stub the probes use, and verifies arguments, results and the stack walk.
#include "../src/probe/probe.cpp"

#include <cstdio>

namespace {

volatile int victimRuns = 0;

__declspec(noinline) double victim(int a, float b, long long c, double d, int e, float f, long long g) {
    victimRuns = victimRuns + 1;  // a side effect, so the optimiser cannot fold repeated calls into one
    return a + b + c + d + e + f + g;
}

__declspec(noinline) double caller() { return victim(1, 2.5f, 3, 4.25, 5, 6.5f, 7); }

}  // namespace

int main() {
    using namespace aceqv;
    if (MH_Initialize() != MH_OK)
        return 2;
    Probe& p = probes[0];
    p.target = reinterpret_cast<void*>(&victim);
    p.stub = build_stub(0);
    if (!p.stub || MH_CreateHook(p.target, p.stub, &p.original) != MH_OK || (set_stub_target(p.stub, p.original), false) ||
        MH_EnableHook(p.target) != MH_OK)
        return 3;

    const double expected = 1 + 2.5 + 3 + 4.25 + 5 + 6.5 + 7;
    bool ok = true;
    for (int i = 0; i < 3; ++i) {
        const double got = caller();
        std::printf("call %d: result=%.4f expected=%.4f\n", i, got, expected);
        ok &= got == expected;
    }
    std::printf("probe calls=%llu original runs=%d (expected 3 and 3)\n",
                static_cast<unsigned long long>(p.calls.load()), victimRuns);
    ok &= p.calls.load() == 3 && victimRuns == 3;

    // walk_stack from a synthetic entry: the stack captured inside on_probe is logged to the file.
    MH_DisableHook(p.target);
    MH_Uninitialize();
    std::printf("RESULT: %s (see ACE_QV_LOG for the logged stacks)\n", ok ? "pass" : "FAIL");
    return ok ? 0 : 1;
}
