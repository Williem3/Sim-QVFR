// Where the game runs on the CPU, read only (nothing is set): the processor's kinds of cores (a dual-CCD X3D's
// V-Cache CCD and its other CCD, Intel's P- and E-cores, by L3 cache and efficiency class), which kind the render
// thread ran on at each xrEndFrame, how busy and how many parked each kind is (every process), and what Windows
// gives the process: priority class, affinity, default CPU sets, EcoQoS (power throttling) as the game set it,
// the timer's real granularity (how long Sleep(1) takes in this process: Windows 11 coarsens it for a hidden
// window), whether its window is in front, Game Mode (HasExpandedResources) and the power plan.
// Logged at the session start and with each load report: what a CPU-side boost would change, measured first.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "probe/cpu_load.h"

namespace simqvfr {

// One logical processor: its group and number in it, its core, its L3 (-1: none known) and that cache's size,
// and its core's efficiency class (Windows: higher = faster; all 0 on a CPU of one kind).
struct LogicalCpu {
    uint16_t group = 0;
    uint8_t number = 0;
    int core = 0;
    int l3 = -1;
    uint64_t l3Bytes = 0;
    uint8_t efficiency = 0;
};

// The kinds of logical processors: one per L3 that differs from the others and per efficiency class. `kindOf`
// follows the `cpus` given; each kind has a label ("CCD1 32 MB L3", "CCD0 96 MB L3 (V-Cache)", "P-cores"), its
// logical processors and its cores.
struct CpuKinds {
    std::vector<std::string> labels;
    std::vector<int> logical, cores;  // per kind
    std::vector<int> kindOf;          // per cpu
};
// `amd`: name the L3 groups CCDs, and when their sizes differ the largest one V-Cache.
CpuKinds cpu_kinds_from(const std::vector<LogicalCpu>& cpus, bool amd);

// Each processor's busy share (percent, every process) from its idle cycles before and after `elapsedCycles`.
std::vector<double> processor_busy_from(const std::vector<uint64_t>& idleBefore, const std::vector<uint64_t>& idleAfter,
                                        uint64_t elapsedCycles);

// This machine's logical processors (GetLogicalProcessorInformationEx), group then number order.
std::vector<LogicalCpu> cpu_topology();
// The processor's brand string (CPUID), and whether it is AMD's.
std::string cpu_brand(bool* amd = nullptr);
// How long Sleep(1) takes on the calling thread, mean of `count` calls in ms (the process's timer granularity).
double sleep_one_ms(int count = 4);
// A priority class's name ("Normal", "Above normal", ...).
const char* priority_class_name(uint32_t priorityClass);

// The session's start: the topology and the process's state, logged once; a worker re-reads the window, the
// timer and Game Mode every few seconds.
void cpu_placement_start();
void cpu_placement_stop();
// On the render thread at each xrEndFrame: which kind of core it is on (a counter).
void cpu_placement_frame();
// With the load report: the window's placement and state, the busiest threads' ideal processors (one line).
void cpu_placement_report(const std::vector<ThreadLoad>& busiest);

}  // namespace simqvfr
