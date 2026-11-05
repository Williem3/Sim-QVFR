#include "batch_watch.h"

#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include <MinHook.h>

#include "abi.h"
#include "../../engine/code_patch.h"
#include "../../log.h"

namespace simqvfr {
namespace {

constexpr int kMaxGroups = 64;  // ACE's GPU visibility passes cap the group count at 64
constexpr uint64_t kReportEveryFrames = 900;
constexpr int kWarnPercent = 80;

struct Group {
    char name[48];
    int64_t draws, subEntries, instances, lods;
};

// One merge's view of the batch: the totals ACE is about to check, and the groups behind them.
struct Batch {
    int groups, used;  // all groups / those with draws (the ones ACE merges)
    int64_t draws, subEntries, instances, lods;
    int32_t limitInstances, limitDraws, limitSubEntries;
    Group group[kMaxGroups];
};

using MergeFn = void (*)(void* renderer);
MergeFn mergeOriginal = nullptr;
bool installed = false;

SRWLOCK lock = SRWLOCK_INIT;
Batch windowMax = {};  // per-total maxima over the report window (groups not kept)
Batch peak = {};       // the frame with the highest share of any limit so far, groups included
int peakPercent = -1;
bool warned = false, overLogged = false;
uint64_t merges = 0, unreadable = 0;

template <class T>
T at(uintptr_t p) {
    return *reinterpret_cast<const T*>(p);
}

int64_t vector_count(uintptr_t v, size_t element) {
    return static_cast<int64_t>((at<uintptr_t>(v + 8) - at<uintptr_t>(v)) / element);
}

// Plain data only, so the read can sit under __try.
bool read_batch(uintptr_t renderer, Batch& b) {
    __try {
        const uintptr_t groups = at<uintptr_t>(renderer + ace::kBatchGroupsBegin);
        const uintptr_t groupsEnd = at<uintptr_t>(renderer + ace::kBatchGroupsEnd);
        const uintptr_t entries = at<uintptr_t>(renderer + ace::kBatchEntries);
        const uintptr_t limits = at<uintptr_t>(renderer + ace::kBatchLimits);
        b.groups = static_cast<int>((groupsEnd - groups) / ace::kBatchGroupStride);
        b.limitInstances = at<int32_t>(limits);
        b.limitDraws = at<int32_t>(limits + 4);
        b.limitSubEntries = at<int32_t>(limits + 8);
        b.used = 0;
        b.draws = b.subEntries = b.instances = b.lods = 0;
        for (int i = 0; i < b.groups; ++i) {
            const uintptr_t g = groups + static_cast<uintptr_t>(i) * ace::kBatchGroupStride;
            const int64_t draws = vector_count(g + ace::kGroupDraws, ace::kDrawSize);
            if (draws == 0)
                continue;
            const int64_t sub = vector_count(g + ace::kGroupSubEntries, ace::kSubEntrySize);
            const int64_t inst = at<int32_t>(g + ace::kGroupInstances);
            const int64_t lods = vector_count(g + ace::kGroupLods, 4);
            b.draws += draws;
            b.subEntries += sub;
            b.instances += inst;
            b.lods += lods;
            if (b.used < kMaxGroups) {
                Group& out = b.group[b.used++];
                out = {"?", draws, sub, inst, lods};
                // MSVC std::string: inline buffer (or pointer) at +0, size +0x10, capacity +0x18.
                const uintptr_t s = entries + static_cast<uintptr_t>(i) * ace::kBatchEntryStride + ace::kBatchEntryName;
                const size_t size = at<size_t>(s + 0x10), capacity = at<size_t>(s + 0x18);
                const char* text = capacity > 15 ? at<const char*>(s) : reinterpret_cast<const char*>(s);
                const size_t n = std::min(size, sizeof(out.name) - 1);
                std::memcpy(out.name, text, n);
                out.name[n] = 0;
            }
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

int percent(int64_t value, int64_t limit) { return limit > 0 ? static_cast<int>(value * 100 / limit) : 0; }

// The highest share of any limit; ACE throws at instances >= limit or any other total > its limit.
int worst_percent(const Batch& b) {
    return std::max({percent(b.instances, b.limitInstances), percent(b.draws, b.limitDraws),
                     percent(b.subEntries, b.limitSubEntries), percent(b.lods, ace::kBatchLodLimit),
                     percent(b.draws, ace::kBatchDrawCap)});
}

bool over_limit(const Batch& b) {
    return b.instances >= b.limitInstances || b.draws > b.limitDraws || b.subEntries > b.limitSubEntries ||
           b.lods > ace::kBatchLodLimit || b.draws > ace::kBatchDrawCap;
}

void log_totals(const char* what, const Batch& b) {
    logf("batch: %s: draws %lld/%d, sub-entries %lld/%d, instances %lld/%d, per-LOD entries %lld/%lld; "
         "%d of %d groups with draws",
         what, static_cast<long long>(b.draws), b.limitDraws, static_cast<long long>(b.subEntries), b.limitSubEntries,
         static_cast<long long>(b.instances), b.limitInstances, static_cast<long long>(b.lods),
         static_cast<long long>(ace::kBatchLodLimit), b.used, b.groups);
}

void log_groups(const Batch& b) {
    for (int i = 0; i < b.used; ++i) {
        const Group& g = b.group[i];
        logf("batch:   group \"%s\": draws %lld, sub-entries %lld, instances %lld, per-LOD entries %lld", g.name,
             static_cast<long long>(g.draws), static_cast<long long>(g.subEntries),
             static_cast<long long>(g.instances), static_cast<long long>(g.lods));
    }
}

void mergeHook(void* renderer) {
    static Batch b;  // the merge runs on one thread (the game thread); kept off its stack
    const bool ok = read_batch(reinterpret_cast<uintptr_t>(renderer), b);
    AcquireSRWLockExclusive(&lock);
    ++merges;
    if (!ok) {
        ++unreadable;
    } else {
        windowMax.groups = std::max(windowMax.groups, b.groups);
        windowMax.used = std::max(windowMax.used, b.used);
        windowMax.draws = std::max(windowMax.draws, b.draws);
        windowMax.subEntries = std::max(windowMax.subEntries, b.subEntries);
        windowMax.instances = std::max(windowMax.instances, b.instances);
        windowMax.lods = std::max(windowMax.lods, b.lods);
        windowMax.limitInstances = b.limitInstances;
        windowMax.limitDraws = b.limitDraws;
        windowMax.limitSubEntries = b.limitSubEntries;
        const int worst = worst_percent(b);
        if (worst > peakPercent) {
            peakPercent = worst;
            peak = b;
        }
        if (over_limit(b) && !overLogged) {
            overLogged = true;
            log_totals("OVER ACE's LIMIT, ACE throws \"Instance visibility batch is too large\" now", b);
            log_groups(b);
        } else if (worst >= kWarnPercent && !warned) {
            warned = true;
            char what[64];
            std::snprintf(what, sizeof(what), "first frame at %d%% of a limit", worst);
            log_totals(what, b);
            log_groups(b);
        }
    }
    ReleaseSRWLockExclusive(&lock);
    mergeOriginal(renderer);
}

}  // namespace

bool batch_watch_start() {
    if (installed)
        return true;
    if (!code_matches(ace::kBatchMergeRva, ace::kBatchMergeAnchor, sizeof(ace::kBatchMergeAnchor)) ||
        !code_matches(ace::kBatchLodLimitRva, ace::kBatchLodLimitAnchor, sizeof(ace::kBatchLodLimitAnchor))) {
        log_line("batch: anchor mismatch (different ACE build?); batch watch off");
        return false;
    }
    const MH_STATUS init = MH_Initialize();
    if (init != MH_OK && init != MH_ERROR_ALREADY_INITIALIZED) {
        log_line("batch: MH_Initialize failed; batch watch off");
        return false;
    }
    void* target = reinterpret_cast<void*>(module_base() + ace::kBatchMergeRva);
    MH_STATUS s = MH_CreateHook(target, reinterpret_cast<void*>(&mergeHook), reinterpret_cast<void**>(&mergeOriginal));
    if (s == MH_OK)
        s = MH_EnableHook(target);
    if (s != MH_OK) {
        logf("batch: merge hook failed (MinHook %s); batch watch off", MH_StatusToString(s));
        MH_RemoveHook(target);
        return false;
    }
    installed = true;
    log_line("batch: watching the instance visibility batch (log only)");
    return true;
}

void batch_watch_stop() {
    if (!installed)
        return;
    void* target = reinterpret_cast<void*>(module_base() + ace::kBatchMergeRva);
    MH_DisableHook(target);
    MH_RemoveHook(target);
    installed = false;
    AcquireSRWLockShared(&lock);
    if (peakPercent >= 0) {
        char what[64];
        std::snprintf(what, sizeof(what), "session peak, %d%% of a limit", peakPercent);
        log_totals(what, peak);
        log_groups(peak);
    }
    ReleaseSRWLockShared(&lock);
}

void batch_watch_frame_end() {
    static uint64_t frames = 0;
    if (!installed || ++frames % kReportEveryFrames != 0)
        return;
    AcquireSRWLockExclusive(&lock);
    if (merges > 0) {
        char what[96];
        std::snprintf(what, sizeof(what), "max over the last %llu frames (%llu merges, %llu unreadable)",
                      static_cast<unsigned long long>(kReportEveryFrames), static_cast<unsigned long long>(merges),
                      static_cast<unsigned long long>(unreadable));
        log_totals(what, windowMax);
    }
    windowMax = {};
    merges = unreadable = 0;
    ReleaseSRWLockExclusive(&lock);
}

}  // namespace simqvfr
