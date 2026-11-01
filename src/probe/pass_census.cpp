#include "pass_census.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <wrl/client.h>

#include "../log.h"
#include "../xr.h"

extern "C" size_t aceqv_slot_SetPipelineState(void);
extern "C" size_t aceqv_slot_DrawInstanced(void);
extern "C" size_t aceqv_slot_DrawIndexedInstanced(void);
extern "C" size_t aceqv_slot_Dispatch(void);
extern "C" size_t aceqv_slot_ExecuteIndirect(void);
extern "C" size_t aceqv_slot_RSSetViewports(void);

namespace aceqv {
namespace {

using List = ID3D12GraphicsCommandList;
using SetPipelineStateFn = void(STDMETHODCALLTYPE*)(List*, ID3D12PipelineState*);
using DrawInstancedFn = void(STDMETHODCALLTYPE*)(List*, UINT, UINT, UINT, UINT);
using DrawIndexedInstancedFn = void(STDMETHODCALLTYPE*)(List*, UINT, UINT, UINT, INT, UINT);
using DispatchFn = void(STDMETHODCALLTYPE*)(List*, UINT, UINT, UINT);
using ExecuteIndirectFn = void(STDMETHODCALLTYPE*)(List*, ID3D12CommandSignature*, UINT, ID3D12Resource*, UINT64,
                                                   ID3D12Resource*, UINT64);
using RSSetViewportsFn = void(STDMETHODCALLTYPE*)(List*, UINT, const D3D12_VIEWPORT*);

SetPipelineStateFn setPsoOriginal = nullptr;
DrawInstancedFn drawOriginal = nullptr;
DrawIndexedInstancedFn drawIndexedOriginal = nullptr;
DispatchFn dispatchOriginal = nullptr;
ExecuteIndirectFn indirectOriginal = nullptr;
RSSetViewportsFn viewportsOriginal = nullptr;

// Where draws land: views 0-3 of the target, its halves (views 0-1 / views 2-3 together), or elsewhere.
enum Region { kQuarter0, kQuarter1, kQuarter2, kQuarter3, kHalf0, kHalf1, kOther, kUnknown, kRegions };

struct Census {
    uint64_t draws[kRegions] = {};
    uint64_t indirect[kRegions] = {};
    uint64_t dispatches = 0;
    uint32_t dims[4][3] = {};
    uint32_t dimCount[4] = {};
    size_t distinctDims = 0;
    bool named = false, watched = false;  // watched: its name is pass_census_watch's
    int stacksLogged = 0;
    uint32_t listTypes = 0;  // dispatches: bit per D3D12_COMMAND_LIST_TYPE (0 direct, 2 compute)
};

// The command list a thread records, its viewport region and pipeline state.
struct ListState {
    List* list = nullptr;
    Region region = kUnknown;
    ID3D12PipelineState* pso = nullptr;
    D3D12_VIEWPORT viewport{};
};
thread_local ListState current;
thread_local bool barrierAfterNextDispatch = false;  // pass_census_barrier_after_next_dispatch

// Recording cost per region (the focus-cull measurement): direct draws, indirect batches, pipeline
// changes, and the CPU time between consecutive draws on one list in the same region. That gap is
// ACE's per-item work (plus these hooks' own), an approximation of each view's recording time.
struct Recording {
    std::atomic<uint64_t> draws{0}, indirect{0}, psoChanges{0}, gapTicks{0}, gaps{0};
};
Recording recording[kRegions];
constexpr double kMaxGapMs = 2.0;  // longer gaps are other work (a pass boundary, a wait), not counted
int64_t maxGapTicks = 0;
double msPerTick = 0;
thread_local List* lastDrawList = nullptr;
thread_local Region lastDrawRegion = kUnknown;
thread_local int64_t lastDrawTicks = 0;

SRWLOCK lock = SRWLOCK_INIT;
std::unordered_map<ID3D12PipelineState*, Census> census;
// Each view's columns in ACE's full-size target (views4 layout), once known (viewsKnown).
int viewLeft[4] = {}, viewWidth[4] = {};
std::atomic<bool> viewsKnown{false};
std::atomic<bool> counting{false};
std::atomic<bool> uavBarriers{false};
uint64_t frame = 0, firstFrame = 0, frameCount = 0;
bool started = false, reported = false;
char watchName[512] = {};

ListState& state_of(List* list) {
    if (current.list != list)
        current = ListState{list};
    return current;
}

Region region_of(const D3D12_VIEWPORT& v) {
    if (!viewsKnown.load(std::memory_order_acquire))
        return kUnknown;
    const auto close = [](float a, int b) { return std::fabs(a - static_cast<float>(b)) < 1.5f; };
    for (int i = 0; i < 4; ++i)
        if (close(v.Width, viewWidth[i]) && close(v.TopLeftX, viewLeft[i]))
            return static_cast<Region>(kQuarter0 + i);
    if (close(v.Width, viewWidth[0] + viewWidth[1]) && close(v.TopLeftX, viewLeft[0]))
        return kHalf0;
    if (close(v.Width, viewWidth[2] + viewWidth[3]) && close(v.TopLeftX, viewLeft[2]))
        return kHalf1;
    return kOther;
}

Census& entry(ID3D12PipelineState* pso) { return census[pso]; }
std::string name_of(ID3D12PipelineState* pso);

// module+offset of a return address.
std::string describe(void* address) {
    HMODULE module = nullptr;
    char path[MAX_PATH] = "?";
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           static_cast<LPCSTR>(address), &module) &&
        module)
        GetModuleFileNameA(module, path, MAX_PATH);
    const char* slash = std::strrchr(path, '\\');
    char out[MAX_PATH + 32];
    std::snprintf(out, sizeof(out), "%s+0x%llx", slash ? slash + 1 : path,
                  static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(address) - reinterpret_cast<uintptr_t>(module)));
    return out;
}

void STDMETHODCALLTYPE setPsoHook(List* list, ID3D12PipelineState* pso) {
    ListState& s = state_of(list);
    s.pso = pso;
    if (counting.load(std::memory_order_relaxed))
        recording[s.region].psoChanges.fetch_add(1, std::memory_order_relaxed);
    setPsoOriginal(list, pso);
}

void STDMETHODCALLTYPE viewportsHook(List* list, UINT count, const D3D12_VIEWPORT* viewports) {
    if (count > 0 && viewports) {
        ListState& s = state_of(list);
        s.region = region_of(viewports[0]);
        s.viewport = viewports[0];
    }
    viewportsOriginal(list, count, viewports);
}

// Whether `name` is one of pass_census_watch's comma-separated names; a name ending in '*' is a
// prefix (ACE's material pipelines are named by an id that changes between launches).
bool is_watched(const std::string& name) {
    if (name.empty())
        return false;
    for (const char* p = watchName; *p;) {
        const char* end = std::strchr(p, ',');
        const size_t length = end ? static_cast<size_t>(end - p) : std::strlen(p);
        const bool prefix = length > 0 && p[length - 1] == '*';
        if ((prefix ? name.size() >= length - 1 : name.size() == length) &&
            std::strncmp(name.c_str(), p, prefix ? length - 1 : length) == 0)
            return true;
        p = end ? end + 1 : p + length;
    }
    return false;
}

// Under the lock: whether this call of a watched pipeline gets its stack logged (the first four).
bool take_stack(Census& c, ID3D12PipelineState* pso, std::string* name) {
    if (!c.named) {
        c.named = true;
        c.watched = watchName[0] && is_watched(name_of(pso));
    }
    if (!c.watched || c.stacksLogged >= 4)
        return false;
    ++c.stacksLogged;
    *name = name_of(pso);
    return true;
}

// Logs the caller's stack, each distinct stack once (watched pipelines share their draw loops).
SRWLOCK stacksLock = SRWLOCK_INIT;
std::unordered_set<ULONG> stacksSeen;

void log_stack(const char* prefix, ULONG skip) {
    void* frames[12];
    ULONG hash = 0;
    const USHORT n = RtlCaptureStackBackTrace(skip, 12, frames, &hash);
    AcquireSRWLockExclusive(&stacksLock);
    const bool fresh = stacksSeen.insert(hash).second;
    ReleaseSRWLockExclusive(&stacksLock);
    if (!fresh)
        return;
    char line[1024];
    int len = std::snprintf(line, sizeof(line), "%s stack:", prefix);
    for (USHORT i = 0; i < n && len < 960; ++i)
        len += std::snprintf(line + len, sizeof(line) - len, " %s", describe(frames[i]).c_str());
    log_line(line);
}

void note_recording(List* list, Region region, bool indirect) {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    Recording& r = recording[region];
    (indirect ? r.indirect : r.draws).fetch_add(1, std::memory_order_relaxed);
    const int64_t gap = now.QuadPart - lastDrawTicks;
    if (lastDrawList == list && lastDrawRegion == region && gap >= 0 && gap < maxGapTicks) {
        r.gapTicks.fetch_add(static_cast<uint64_t>(gap), std::memory_order_relaxed);
        r.gaps.fetch_add(1, std::memory_order_relaxed);
    }
    lastDrawList = list;
    lastDrawRegion = region;
    lastDrawTicks = now.QuadPart;
}

void count_draw(List* list, bool indirect) {
    if (!counting.load(std::memory_order_relaxed))
        return;
    const ListState& s = state_of(list);
    note_recording(list, s.region, indirect);
    std::string name;
    AcquireSRWLockExclusive(&lock);
    Census& c = entry(s.pso);
    ++(indirect ? c.indirect : c.draws)[s.region];
    const bool logStack = take_stack(c, s.pso, &name);
    ReleaseSRWLockExclusive(&lock);
    if (logStack) {
        char prefix[256];
        std::snprintf(prefix, sizeof(prefix), "pass census: watched %s draw viewport=%.0f,%.0f %.0fx%.0f list=%p", name.c_str(),
                      s.viewport.TopLeftX, s.viewport.TopLeftY, s.viewport.Width, s.viewport.Height, list);
        log_stack(prefix, 3);  // log_stack, count_draw, the draw hook
    }
}

void STDMETHODCALLTYPE drawHook(List* list, UINT a, UINT b, UINT c, UINT d) {
    count_draw(list, false);
    drawOriginal(list, a, b, c, d);
}

void STDMETHODCALLTYPE drawIndexedHook(List* list, UINT a, UINT b, UINT c, INT d, UINT e) {
    count_draw(list, false);
    drawIndexedOriginal(list, a, b, c, d, e);
}

void STDMETHODCALLTYPE indirectHook(List* list, ID3D12CommandSignature* signature, UINT max, ID3D12Resource* args,
                                    UINT64 argsOffset, ID3D12Resource* countBuffer, UINT64 countOffset) {
    count_draw(list, true);
    indirectOriginal(list, signature, max, args, argsOffset, countBuffer, countOffset);
}

void STDMETHODCALLTYPE dispatchHook(List* list, UINT x, UINT y, UINT z) {
    if (counting.load(std::memory_order_relaxed)) {
        const ListState& s = state_of(list);
        AcquireSRWLockExclusive(&lock);
        Census& c = entry(s.pso);
        ++c.dispatches;
        c.listTypes |= 1u << static_cast<unsigned>(list->GetType());
        size_t i = 0;
        while (i < c.distinctDims && !(c.dims[i][0] == x && c.dims[i][1] == y && c.dims[i][2] == z))
            ++i;
        if (i == c.distinctDims && i < std::size(c.dims)) {
            c.dims[i][0] = x;
            c.dims[i][1] = y;
            c.dims[i][2] = z;
            ++c.distinctDims;
        }
        if (i < std::size(c.dims))
            ++c.dimCount[i];
        std::string name;
        const bool logStack = take_stack(c, s.pso, &name);
        ReleaseSRWLockExclusive(&lock);
        if (logStack) {
            char prefix[256];
            std::snprintf(prefix, sizeof(prefix), "pass census: watched %s dispatch %ux%ux%u list=%p", name.c_str(), x, y, z, list);
            log_stack(prefix, 2);  // log_stack, the dispatch hook
        }
    }
    dispatchOriginal(list, x, y, z);
    if (barrierAfterNextDispatch || uavBarriers.load(std::memory_order_relaxed)) {  // the dispatch's writes finish first
        barrierAfterNextDispatch = false;
        D3D12_RESOURCE_BARRIER barrier{D3D12_RESOURCE_BARRIER_TYPE_UAV};
        barrier.UAV.pResource = nullptr;
        list->ResourceBarrier(1, &barrier);
    }
}

bool patch(void** vtable, size_t slot, void* hook, void** original) {
    DWORD old = 0;
    if (!VirtualProtect(&vtable[slot], sizeof(void*), PAGE_READWRITE, &old))
        return false;
    *original = vtable[slot];
    vtable[slot] = hook;
    VirtualProtect(&vtable[slot], sizeof(void*), old, &old);
    return true;
}

template <class Fn> bool hook(void** vtable, size_t slot, Fn detour, Fn* original) {
    return patch(vtable, slot, reinterpret_cast<void*>(detour), reinterpret_cast<void**>(original));
}

std::string name_of(ID3D12PipelineState* pso) {
    if (!pso)
        return "(none)";
    wchar_t name[128] = {};
    UINT size = sizeof(name) - sizeof(wchar_t);
    if (FAILED(pso->GetPrivateData(WKPDID_D3DDebugObjectNameW, &size, name)))
        return "";
    char out[128];
    std::snprintf(out, sizeof(out), "%ls", name);
    return out;
}

void report() {
    AcquireSRWLockExclusive(&lock);
    const double n = static_cast<double>(frameCount);
    logf("pass census: %zu pipeline states over %llu frames (view widths %d/%d/%d/%d); per frame: q0 q1 q2 q3 | "
         "half0 half1 other unknown",
         census.size(), static_cast<unsigned long long>(frameCount), viewWidth[0], viewWidth[1], viewWidth[2],
         viewWidth[3]);
    std::vector<std::pair<ID3D12PipelineState*, const Census*>> rows;
    for (const auto& [pso, c] : census)
        rows.emplace_back(pso, &c);
    std::sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    const auto per = [&](uint64_t v) { return v / n; };
    for (const auto& [pso, c] : rows) {
        uint64_t draws = 0;
        for (int r = 0; r < kRegions; ++r)
            draws += c->draws[r] + c->indirect[r];
        const std::string name = name_of(pso);
        if (draws) {
            const auto both = [&](int r) { return per(c->draws[r] + c->indirect[r]); };
            const bool eyesOnly = (both(kQuarter0) > 0 || both(kQuarter1) > 0) && both(kQuarter2) == 0 && both(kQuarter3) == 0;
            logf("pass census: gfx %p%s%s%s draws %.1f %.1f %.1f %.1f | %.1f %.1f %.1f %.1f (indirect %.1f)%s", pso,
                 name.empty() ? "" : " \"", name.c_str(), name.empty() ? "" : "\"", both(kQuarter0), both(kQuarter1),
                 both(kQuarter2), both(kQuarter3), both(kHalf0), both(kHalf1), both(kOther), both(kUnknown),
                 per([&] {
                     uint64_t s = 0;
                     for (int r = 0; r < kRegions; ++r)
                         s += c->indirect[r];
                     return s;
                 }()),
                 eyesOnly ? "  <-- eyes only" : "");
        }
        if (c->dispatches) {
            char dims[256];
            int len = 0;
            for (size_t i = 0; i < c->distinctDims; ++i)
                len += std::snprintf(dims + len, sizeof(dims) - len, " %ux%ux%u:%.1f", c->dims[i][0], c->dims[i][1],
                                     c->dims[i][2], per(c->dimCount[i]));
            if (len == 0)
                dims[0] = 0;
            logf("pass census: cs %p%s%s%s dispatches %.1f dims%s lists%s%s", pso, name.empty() ? "" : " \"", name.c_str(),
                 name.empty() ? "" : "\"", per(c->dispatches), dims, (c->listTypes & 1) ? " direct" : "",
                 (c->listTypes & 4) ? " compute" : "");
        }
    }
    census.clear();
    ReleaseSRWLockExclusive(&lock);
    static const char* const names[kRegions] = {"q0", "q1", "q2", "q3", "half0", "half1", "other", "unknown"};
    for (int region = 0; region < kRegions; ++region) {
        Recording& r = recording[region];
        const uint64_t draws = r.draws.exchange(0), indirect = r.indirect.exchange(0), pso = r.psoChanges.exchange(0);
        const uint64_t gapTicks = r.gapTicks.exchange(0), gaps = r.gaps.exchange(0);
        if (draws + indirect + pso == 0)
            continue;
        logf("pass census: recording %s per frame: direct draws %.1f, indirect batches %.1f, pipeline changes %.1f, "
             "time between draws %.3f ms over %.1f gaps (%.2f us each)",
             names[region], per(draws), per(indirect), per(pso), per(gapTicks) * msPerTick, per(gaps),
             gaps ? static_cast<double>(gapTicks) * msPerTick * 1000.0 / static_cast<double>(gaps) : 0.0);
    }
}

}  // namespace

bool pass_census_start(ID3D12Device* device) {
    if (started || !device)
        return started;
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> allocator;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> list;
    if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))) ||
        FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr,
                                         IID_PPV_ARGS(&list)))) {
        log_line("pass census: could not create a probe command list");
        return false;
    }
    list->Close();
    LARGE_INTEGER frequency;
    QueryPerformanceFrequency(&frequency);
    msPerTick = 1000.0 / static_cast<double>(frequency.QuadPart);
    maxGapTicks = static_cast<int64_t>(kMaxGapMs / msPerTick);
    void** vtable = *reinterpret_cast<void***>(list.Get());
    started = hook(vtable, aceqv_slot_SetPipelineState(), &setPsoHook, &setPsoOriginal) &&
              hook(vtable, aceqv_slot_DrawInstanced(), &drawHook, &drawOriginal) &&
              hook(vtable, aceqv_slot_DrawIndexedInstanced(), &drawIndexedHook, &drawIndexedOriginal) &&
              hook(vtable, aceqv_slot_Dispatch(), &dispatchHook, &dispatchOriginal) &&
              hook(vtable, aceqv_slot_ExecuteIndirect(), &indirectHook, &indirectOriginal) &&
              hook(vtable, aceqv_slot_RSSetViewports(), &viewportsHook, &viewportsOriginal);
    logf("pass census: %s (list vtable %p)", started ? "installed" : "FAILED", vtable);
    return started;
}

void pass_census_set_view_columns(const int (&left)[4], const int (&width)[4]) {
    if (viewsKnown.load(std::memory_order_acquire))
        return;
    std::memcpy(viewLeft, left, sizeof(viewLeft));
    std::memcpy(viewWidth, width, sizeof(viewWidth));
    viewsKnown.store(true, std::memory_order_release);
}

void pass_census_set_uav_barriers(bool on) { uavBarriers = on; }

bool pass_census_installed() { return started; }

void pass_census_barrier_after_next_dispatch() { barrierAfterNextDispatch = true; }

void pass_census_watch(const char* name) { std::snprintf(watchName, sizeof(watchName), "%s", name ? name : ""); }

void pass_census_arm(uint64_t first, uint64_t frames) {
    firstFrame = first;
    frameCount = frames;
    reported = false;
}

void pass_census_frame_end() {
    ++frame;
    if (!started || reported || frameCount == 0)
        return;
    if (frame == firstFrame)
        counting = true;
    if (frame == firstFrame + frameCount) {
        counting = false;
        reported = true;
        report();
    }
}

}  // namespace aceqv
