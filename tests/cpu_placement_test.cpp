// Offline check of the CPU placement probe (probe/cpu_placement.h): the kinds of cores for the layouts it must tell
// apart (one CCD, a dual-CCD X3D, two equal CCDs, Intel's P- and E-cores), the busy share from idle cycles, and the
// live readings on this machine (every logical processor has a kind, Sleep(1) measures).
#include <cstdio>
#include <string>
#include <vector>

#include "engine/cpu_boost.h"
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
    {  // EcoQoS off (engine/cpu_boost.h): only its bit changes; the game's other choices (LMU's timer bit, 0x4) kept
        check(with_full_speed({0x4, 0}, true) == PowerThrottling{0x5, 0}, "full speed: EcoQoS controlled, off; timer bit kept");
        check(with_full_speed({0x5, 0x1}, true) == PowerThrottling{0x5, 0}, "full speed over EcoQoS on: off");
        check(with_full_speed({0x5, 0x4}, false) == PowerThrottling{0x4, 0x4}, "Windows decides: EcoQoS uncontrolled, the rest kept");
        PowerThrottling before{}, on{}, off{};
        const bool known = read_power_throttling(before);
        check(cpu_set_full_speed(true) && read_power_throttling(on) && (on.control & kThrottleExecutionSpeed) &&
                  !(on.state & kThrottleExecutionSpeed),
              "this process: full speed set and read back");
        check(cpu_set_full_speed(false) && read_power_throttling(off) && !(off.control & kThrottleExecutionSpeed),
              "this process: back to Windows");
        std::printf("     power throttling before 0x%x/0x%x, full speed 0x%x/0x%x, Windows 0x%x/0x%x\n", before.control,
                    before.state, on.control, on.state, off.control, off.state);
        check(known, "power throttling readable");
    }
    return test_result();
}
