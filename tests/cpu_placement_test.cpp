// Offline check of the CPU placement probe (probe/cpu_placement.h): the kinds of cores for the layouts it must tell
// apart (one CCD, a dual-CCD X3D, two equal CCDs, Intel's P- and E-cores), the busy share from idle cycles, and the
// live readings on this machine (every logical processor has a kind, Sleep(1) measures).
#include <cstdio>
#include <string>
#include <vector>

#include <windows.h>

#include "probe/cpu_isolation.h"
#include "probe/cpu_placement.h"
#include "test_util.h"

using namespace simqvfr;

namespace {

constexpr uint64_t kMb = 1024ull * 1024;

// `cores` cores of two logical processors each, all on one L3 of `l3Mb` per `perL3` cores, efficiency class `eff`.
std::vector<LogicalCpu> layout(int cores, int perL3, uint64_t firstL3Mb, uint64_t otherL3Mb) {
    std::vector<LogicalCpu> cpus;
    for (int c = 0; c < cores; ++c)
        for (int t = 0; t < 2; ++t) {
            LogicalCpu cpu;
            cpu.number = static_cast<uint8_t>(2 * c + t);
            cpu.core = c;
            cpu.l3 = c / perL3;
            cpu.l3Bytes = (cpu.l3 == 0 ? firstL3Mb : otherL3Mb) * kMb;
            cpus.push_back(cpu);
        }
    return cpus;
}

}  // namespace

int SIM_QVFR_TEST_MAIN() {
    {  // 5800X3D: one CCD, one kind
        const CpuKinds k = cpu_kinds_from(layout(8, 8, 96, 96), true);
        check(k.labels.size() == 1, "one CCD: one kind");
        check(!k.labels.empty() && k.labels[0] == "all, 96 MB L3", "one CCD: labelled all, 96 MB L3");
        check(!k.logical.empty() && k.logical[0] == 16 && k.cores[0] == 8, "one CCD: 16 logical, 8 cores");
    }
    {  // 9950X3D: the V-Cache CCD first, then the other
        const CpuKinds k = cpu_kinds_from(layout(16, 8, 96, 32), true);
        check(k.labels.size() == 2, "dual-CCD X3D: two kinds");
        check(k.labels.size() == 2 && k.labels[0] == "CCD0 96 MB L3 (V-Cache)", "dual-CCD X3D: CCD0 is the V-Cache one");
        check(k.labels.size() == 2 && k.labels[1] == "CCD1 32 MB L3", "dual-CCD X3D: CCD1 plain");
        check(k.kindOf.size() == 32 && k.kindOf[0] == 0 && k.kindOf[31] == 1, "dual-CCD X3D: processors on their CCD");
        check(k.cores.size() == 2 && k.cores[0] == 8 && k.cores[1] == 8, "dual-CCD X3D: 8 cores each");
    }
    {  // V-Cache on the second CCD (as some BIOSes number them)
        const CpuKinds k = cpu_kinds_from(layout(16, 8, 32, 96), true);
        check(k.labels.size() == 2 && k.labels[1] == "CCD1 96 MB L3 (V-Cache)", "V-Cache on CCD1 found");
    }
    {  // 7950X: two equal CCDs, no V-Cache
        const CpuKinds k = cpu_kinds_from(layout(16, 8, 32, 32), true);
        check(k.labels.size() == 2 && k.labels[0] == "CCD0 32 MB L3" && k.labels[1] == "CCD1 32 MB L3",
              "two equal CCDs: no V-Cache label");
    }
    {  // Intel hybrid: one L3, P-cores (class 1, two threads) and E-cores (class 0, one thread)
        std::vector<LogicalCpu> cpus;
        uint8_t n = 0;
        for (int c = 0; c < 8; ++c)
            for (int t = 0; t < 2; ++t)
                cpus.push_back({0, n++, c, 0, 36 * kMb, 1});
        for (int c = 8; c < 24; ++c)
            cpus.push_back({0, n++, c, 0, 36 * kMb, 0});
        const CpuKinds k = cpu_kinds_from(cpus, false);
        check(k.labels.size() == 2 && k.labels[0] == "P-cores" && k.labels[1] == "E-cores", "hybrid: P-cores and E-cores");
        check(k.logical.size() == 2 && k.logical[0] == 16 && k.logical[1] == 16 && k.cores[0] == 8 && k.cores[1] == 16,
              "hybrid: counts");
    }
    {  // busy from idle cycles
        const std::vector<double> b = processor_busy_from({0, 100, 500}, {750, 1100, 500}, 1000);
        check(b.size() == 3 && close_to(b[0], 25, 1e-9) && close_to(b[1], 0, 1e-9) && close_to(b[2], 100, 1e-9),
              "busy share from idle cycles (clamped)");
        check(processor_busy_from({1}, {2}, 0).empty(), "no time: no shares");
    }
    {  // this machine
        bool amd = false;
        const std::string brand = cpu_brand(&amd);
        const std::vector<LogicalCpu> cpus = cpu_topology();
        const CpuKinds k = cpu_kinds_from(cpus, amd);
        std::printf("     %s: %zu logical processors, %zu kinds", brand.c_str(), cpus.size(), k.labels.size());
        for (const std::string& l : k.labels)
            std::printf(" [%s]", l.c_str());
        std::printf("\n");
        check(!brand.empty(), "brand string read");
        check(!cpus.empty() && k.kindOf.size() == cpus.size() && !k.labels.empty(), "every logical processor has a kind");
        const double sleep = sleep_one_ms(2);
        std::printf("     Sleep(1) %.2f ms\n", sleep);
        check(sleep > 0.4 && sleep < 40, "Sleep(1) measured");
        check(std::string(priority_class_name(0x20)) == "Normal", "priority class name");
    }
    {  // core isolation's plan: the fastest kind's highest core, never core 0
        const IsolationPlan one = isolation_plan(layout(8, 8, 96, 96));
        check(one.valid && one.core == 7 && one.mainCpus == std::vector<int>{14, 15} && one.others.size() == 14,
              "5800X3D: core 7 (logical 14+15), the other 14 for the rest");
        const IsolationPlan x3d = isolation_plan(layout(16, 8, 32, 96));
        check(x3d.valid && x3d.core == 15, "dual-CCD X3D, V-Cache on CCD1: its highest core");
        const IsolationPlan x3d0 = isolation_plan(layout(16, 8, 96, 32));
        check(x3d0.valid && x3d0.core == 7, "dual-CCD X3D, V-Cache on CCD0: core 7, not the other CCD's");
        std::vector<LogicalCpu> hybrid;
        uint8_t n = 0;
        for (int c = 0; c < 8; ++c)
            for (int t = 0; t < 2; ++t)
                hybrid.push_back({0, n++, c, 0, 36 * kMb, 1});
        for (int c = 8; c < 24; ++c)
            hybrid.push_back({0, n++, c, 0, 36 * kMb, 0});
        const IsolationPlan intel = isolation_plan(hybrid);
        check(intel.valid && intel.core == 7 && intel.mainCpus.size() == 2, "hybrid: the highest P-core");
        check(!isolation_plan(layout(1, 1, 32, 32)).valid, "one core: nothing to isolate");
        const SideStats st = side_stats({1, 2, 3, 4, 5, 6, 7, 8, 9, 10}, {11, 11, 11, 11, 11, 11, 11, 11, 11, 20}, 5);
        check(st.frames == 10 && close_to(st.cpuMean, 5.5, 1e-9) && close_to(st.cpuP90, 10, 1e-9) &&
                  close_to(st.frameP90, 20, 1e-9) && close_to(st.onCore, 50, 1e-9),
              "side stats: mean, p90, share on the core");
    }
    {  // this process: on gives the thread its core and the process the rest, off takes both back
        const IsolationPlan plan = isolation_plan(cpu_topology());
        ULONG count = 0;
        cpu_isolation_mode(1);
        cpu_isolation_frame();
        GetProcessDefaultCpuSets(GetCurrentProcess(), nullptr, 0, &count);
        ULONG threadCount = 0;
        GetThreadSelectedCpuSets(GetCurrentThread(), nullptr, 0, &threadCount);
        std::printf("     on: process default %lu CPU sets, this thread %lu (plan: core %d)\n", count, threadCount, plan.core);
        check(!plan.valid || (count == plan.others.size() && threadCount == plan.mainCpus.size()),
              "isolation on: the other logical processors for the process, the core's for the thread");
        cpu_isolation_mode(0);
        cpu_isolation_frame();
        count = threadCount = 99;
        const BOOL processNone = GetProcessDefaultCpuSets(GetCurrentProcess(), nullptr, 0, &count);
        const BOOL threadNone = GetThreadSelectedCpuSets(GetCurrentThread(), nullptr, 0, &threadCount);
        check(processNone && count == 0 && threadNone && threadCount == 0, "isolation off: both cleared");
        cpu_isolation_end();
    }
    return test_result();
}
