#include "d3d12_trace.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>
#include <tuple>
#include <unordered_map>
#include <unordered_set>

#include <wrl/client.h>

#include "../log.h"
#include "../xr.h"

extern "C" size_t aceqv_slot_CreateRenderTargetView(void);
extern "C" size_t aceqv_slot_OMSetRenderTargets(void);
extern "C" size_t aceqv_slot_RSSetViewports(void);
extern "C" size_t aceqv_slot_BeginRenderPass(void);
extern "C" size_t aceqv_slot_CreateCommittedResource(void);
extern "C" size_t aceqv_slot_CreatePlacedResource(void);
extern "C" size_t aceqv_slot_CreateCommittedResource1(void);
extern "C" size_t aceqv_slot_CreateCommittedResource2(void);
extern "C" size_t aceqv_slot_CreatePlacedResource1(void);
extern "C" size_t aceqv_slot_CreateCommittedResource3(void);
extern "C" size_t aceqv_slot_CreatePlacedResource2(void);

namespace aceqv {
namespace {

using CreateRtvFn = void(STDMETHODCALLTYPE*)(ID3D12Device*, ID3D12Resource*, const D3D12_RENDER_TARGET_VIEW_DESC*,
                                              D3D12_CPU_DESCRIPTOR_HANDLE);
using OMSetRenderTargetsFn = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT,
                                                      const D3D12_CPU_DESCRIPTOR_HANDLE*, BOOL,
                                                      const D3D12_CPU_DESCRIPTOR_HANDLE*);
using RSSetViewportsFn = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, const D3D12_VIEWPORT*);
using BeginRenderPassFn = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList4*, UINT,
                                                   const D3D12_RENDER_PASS_RENDER_TARGET_DESC*,
                                                   const D3D12_RENDER_PASS_DEPTH_STENCIL_DESC*,
                                                   D3D12_RENDER_PASS_FLAGS);
using CreateCommittedFn = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, const D3D12_HEAP_PROPERTIES*, D3D12_HEAP_FLAGS,
                                                      const D3D12_RESOURCE_DESC*, D3D12_RESOURCE_STATES,
                                                      const D3D12_CLEAR_VALUE*, REFIID, void**);
using CreatePlacedFn = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, ID3D12Heap*, UINT64, const D3D12_RESOURCE_DESC*,
                                                   D3D12_RESOURCE_STATES, const D3D12_CLEAR_VALUE*, REFIID, void**);
using CreateCommitted1Fn = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device4*, const D3D12_HEAP_PROPERTIES*, D3D12_HEAP_FLAGS,
                                                       const D3D12_RESOURCE_DESC*, D3D12_RESOURCE_STATES,
                                                       const D3D12_CLEAR_VALUE*, ID3D12ProtectedResourceSession*,
                                                       REFIID, void**);
using CreateCommitted2Fn = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device8*, const D3D12_HEAP_PROPERTIES*, D3D12_HEAP_FLAGS,
                                                       const D3D12_RESOURCE_DESC1*, D3D12_RESOURCE_STATES,
                                                       const D3D12_CLEAR_VALUE*, ID3D12ProtectedResourceSession*,
                                                       REFIID, void**);
using CreatePlaced1Fn = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device8*, ID3D12Heap*, UINT64, const D3D12_RESOURCE_DESC1*,
                                                    D3D12_RESOURCE_STATES, const D3D12_CLEAR_VALUE*, REFIID, void**);
using CreateCommitted3Fn = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device10*, const D3D12_HEAP_PROPERTIES*, D3D12_HEAP_FLAGS,
                                                       const D3D12_RESOURCE_DESC1*, D3D12_BARRIER_LAYOUT,
                                                       const D3D12_CLEAR_VALUE*, ID3D12ProtectedResourceSession*,
                                                       UINT32, const DXGI_FORMAT*, REFIID, void**);
using CreatePlaced2Fn = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device10*, ID3D12Heap*, UINT64, const D3D12_RESOURCE_DESC1*,
                                                    D3D12_BARRIER_LAYOUT, const D3D12_CLEAR_VALUE*, UINT32,
                                                    const DXGI_FORMAT*, REFIID, void**);

constexpr uint64_t kStackedSwapchainHits = 8;
constexpr size_t kMaxDistinct = 96;
constexpr UINT64 kLargeWidth = 1024;

CreateRtvFn createRtvOriginal = nullptr;
OMSetRenderTargetsFn omSetOriginal = nullptr;
RSSetViewportsFn viewportsOriginal = nullptr;
BeginRenderPassFn beginPassOriginal = nullptr;
CreateCommittedFn committedOriginal = nullptr;
CreatePlacedFn placedOriginal = nullptr;
CreateCommitted1Fn committed1Original = nullptr;
CreateCommitted2Fn committed2Original = nullptr;
CreatePlaced1Fn placed1Original = nullptr;
CreateCommitted3Fn committed3Original = nullptr;
CreatePlaced2Fn placed2Original = nullptr;

struct Target {
    ID3D12Resource* resource = nullptr;
    UINT64 width = 0;
    UINT height = 0;
    UINT16 arraySize = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
};

struct ImageInfo {
    uint32_t width, height, index;
};

SRWLOCK lock = SRWLOCK_INIT;
std::unordered_map<SIZE_T, Target> rtvTargets;
std::unordered_map<ID3D12Resource*, ImageInfo> images;
std::unordered_set<std::string> seenTargets, seenAllocations;
UINT rtvIncrement = 0;
bool started = false;
std::atomic<uint64_t> swapchainHits{0}, swapchainHitsThisFrame{0}, bindsThisFrame{0};
std::atomic<bool> sequence{false};
thread_local int pendingViewportLog = 0;
thread_local char pendingLabel[96];

std::string describe(void* address) {
    HMODULE module = nullptr;
    char name[MAX_PATH] = "?";
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           static_cast<LPCSTR>(address), &module) &&
        module) {
        char path[MAX_PATH];
        if (GetModuleFileNameA(module, path, MAX_PATH)) {
            const char* slash = std::strrchr(path, '\\');
            std::snprintf(name, sizeof(name), "%s", slash ? slash + 1 : path);
        }
    }
    char out[MAX_PATH + 32];
    std::snprintf(out, sizeof(out), "%s+0x%llx", name,
                  static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(address) - reinterpret_cast<uintptr_t>(module)));
    return out;
}

int append_stack(char* line, int len, int capacity, ULONG skip) {
    void* frames[14];
    const USHORT captured = RtlCaptureStackBackTrace(skip, 14, frames, nullptr);
    len += std::snprintf(line + len, capacity - len, " stack:");
    for (USHORT i = 0; i < captured && len < capacity - 64; ++i)
        len += std::snprintf(line + len, capacity - len, " %s", describe(frames[i]).c_str());
    return len;
}

// True the first time `key` is seen (bounded so a long session cannot grow the log without end).
bool first_seen(std::unordered_set<std::string>& seen, const std::string& key) {
    AcquireSRWLockExclusive(&lock);
    const bool fresh = seen.size() < kMaxDistinct && seen.insert(key).second;
    ReleaseSRWLockExclusive(&lock);
    return fresh;
}

bool lookup(D3D12_CPU_DESCRIPTOR_HANDLE handle, Target* target, ImageInfo* image, bool* isSwapchain) {
    AcquireSRWLockShared(&lock);
    bool found = false;
    auto rtv = rtvTargets.find(handle.ptr);
    if (rtv != rtvTargets.end()) {
        *target = rtv->second;
        auto it = images.find(rtv->second.resource);
        *isSwapchain = it != images.end();
        if (*isSwapchain)
            *image = it->second;
        found = true;
    }
    ReleaseSRWLockShared(&lock);
    return found;
}

void on_bind(const char* via, D3D12_CPU_DESCRIPTOR_HANDLE handle, UINT slot, UINT count, void* list) {
    Target target;
    ImageInfo image{};
    bool isSwapchain = false;
    if (!lookup(handle, &target, &image, &isSwapchain))
        return;
    char line[1600];
    int len = 0;
    if (isSwapchain) {
        const uint64_t n = ++swapchainHits;
        ++swapchainHitsThisFrame;
        if (n > kStackedSwapchainHits && !sequence.load(std::memory_order_relaxed))
            return;
        len = std::snprintf(line, sizeof(line), "d3d12 swapchain write #%llu via=%s target=%ux%u image=%u slot=%u/%u list=%p",
                            static_cast<unsigned long long>(n), via, image.width, image.height, image.index, slot, count,
                            list);
        if (n <= kStackedSwapchainHits)
            len = append_stack(line, len, sizeof(line), 3);
        log_line(line);
        std::snprintf(pendingLabel, sizeof(pendingLabel), "swapchain #%llu", static_cast<unsigned long long>(n));
        pendingViewportLog = 1;
        return;
    }
    if (target.width < kLargeWidth)
        return;
    ++bindsThisFrame;
    char key[96];
    std::snprintf(key, sizeof(key), "%llux%u fmt=%d array=%u", static_cast<unsigned long long>(target.width),
                  target.height, target.format, target.arraySize);
    if (first_seen(seenTargets, key)) {
        len = std::snprintf(line, sizeof(line), "d3d12 render target first bind %s via=%s slot=%u/%u resource=%p", key, via,
                            slot, count, target.resource);
        len = append_stack(line, len, sizeof(line), 3);
        log_line(line);
        std::snprintf(pendingLabel, sizeof(pendingLabel), "first %s", key);
        pendingViewportLog = 1;
    } else if (sequence.load(std::memory_order_relaxed)) {
        logf("d3d12 rt bind %s via=%s slot=%u/%u resource=%p list=%p", key, via, slot, count, target.resource, list);
        std::snprintf(pendingLabel, sizeof(pendingLabel), "bind %s", key);
        pendingViewportLog = 1;
    }
}

void on_allocation(const char* via, const D3D12_RESOURCE_DESC* desc) {
    if (!desc || desc->Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || desc->Width < kLargeWidth)
        return;
    constexpr D3D12_RESOURCE_FLAGS kTargetFlags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET |
                                                  D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL |
                                                  D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    if (!(desc->Flags & kTargetFlags))
        return;
    char key[128];
    std::snprintf(key, sizeof(key), "%llux%u fmt=%d array=%u mips=%u flags=0x%x", static_cast<unsigned long long>(desc->Width),
                  desc->Height, desc->Format, desc->DepthOrArraySize, desc->MipLevels, desc->Flags);
    if (!first_seen(seenAllocations, key))
        return;
    char line[1600];
    int len = std::snprintf(line, sizeof(line), "d3d12 texture allocation %s via=%s", key, via);
    len = append_stack(line, len, sizeof(line), 3);
    log_line(line);
}

void STDMETHODCALLTYPE createRtvHook(ID3D12Device* device, ID3D12Resource* resource,
                                     const D3D12_RENDER_TARGET_VIEW_DESC* desc, D3D12_CPU_DESCRIPTOR_HANDLE handle) {
    createRtvOriginal(device, resource, desc, handle);
    Target target;
    if (resource) {
        const D3D12_RESOURCE_DESC rd = resource->GetDesc();
        target = Target{resource, rd.Width, rd.Height, rd.DepthOrArraySize, desc ? desc->Format : rd.Format};
    }
    AcquireSRWLockExclusive(&lock);
    rtvTargets[handle.ptr] = target;
    ReleaseSRWLockExclusive(&lock);
}

void STDMETHODCALLTYPE omSetHook(ID3D12GraphicsCommandList* list, UINT count, const D3D12_CPU_DESCRIPTOR_HANDLE* rtvs,
                                 BOOL singleRange, const D3D12_CPU_DESCRIPTOR_HANDLE* dsv) {
    for (UINT i = 0; rtvs && i < count; ++i)
        on_bind("OMSetRenderTargets",
                singleRange ? D3D12_CPU_DESCRIPTOR_HANDLE{rtvs[0].ptr + i * rtvIncrement} : rtvs[i], i, count, list);
    omSetOriginal(list, count, rtvs, singleRange, dsv);
}

void STDMETHODCALLTYPE viewportsHook(ID3D12GraphicsCommandList* list, UINT count, const D3D12_VIEWPORT* viewports) {
    if (pendingViewportLog && count > 0 && viewports) {
        pendingViewportLog = 0;
        logf("d3d12 %s viewport=%.0f,%.0f %.0fx%.0f count=%u", pendingLabel, viewports[0].TopLeftX, viewports[0].TopLeftY,
             viewports[0].Width, viewports[0].Height, count);
    }
    viewportsOriginal(list, count, viewports);
}

void STDMETHODCALLTYPE beginPassHook(ID3D12GraphicsCommandList4* list, UINT count,
                                     const D3D12_RENDER_PASS_RENDER_TARGET_DESC* targets,
                                     const D3D12_RENDER_PASS_DEPTH_STENCIL_DESC* depth, D3D12_RENDER_PASS_FLAGS flags) {
    for (UINT i = 0; targets && i < count; ++i)
        on_bind("BeginRenderPass", targets[i].cpuDescriptor, i, count, list);
    beginPassOriginal(list, count, targets, depth, flags);
}

HRESULT STDMETHODCALLTYPE committedHook(ID3D12Device* d, const D3D12_HEAP_PROPERTIES* hp, D3D12_HEAP_FLAGS hf,
                                        const D3D12_RESOURCE_DESC* desc, D3D12_RESOURCE_STATES s,
                                        const D3D12_CLEAR_VALUE* cv, REFIID riid, void** out) {
    on_allocation("CreateCommittedResource", desc);
    return committedOriginal(d, hp, hf, desc, s, cv, riid, out);
}
HRESULT STDMETHODCALLTYPE placedHook(ID3D12Device* d, ID3D12Heap* heap, UINT64 offset, const D3D12_RESOURCE_DESC* desc,
                                     D3D12_RESOURCE_STATES s, const D3D12_CLEAR_VALUE* cv, REFIID riid, void** out) {
    on_allocation("CreatePlacedResource", desc);
    return placedOriginal(d, heap, offset, desc, s, cv, riid, out);
}
HRESULT STDMETHODCALLTYPE committed1Hook(ID3D12Device4* d, const D3D12_HEAP_PROPERTIES* hp, D3D12_HEAP_FLAGS hf,
                                         const D3D12_RESOURCE_DESC* desc, D3D12_RESOURCE_STATES s,
                                         const D3D12_CLEAR_VALUE* cv, ID3D12ProtectedResourceSession* ps, REFIID riid,
                                         void** out) {
    on_allocation("CreateCommittedResource1", desc);
    return committed1Original(d, hp, hf, desc, s, cv, ps, riid, out);
}
// D3D12_RESOURCE_DESC1 starts with every D3D12_RESOURCE_DESC field, so the prefix is read as one.
HRESULT STDMETHODCALLTYPE committed2Hook(ID3D12Device8* d, const D3D12_HEAP_PROPERTIES* hp, D3D12_HEAP_FLAGS hf,
                                         const D3D12_RESOURCE_DESC1* desc, D3D12_RESOURCE_STATES s,
                                         const D3D12_CLEAR_VALUE* cv, ID3D12ProtectedResourceSession* ps, REFIID riid,
                                         void** out) {
    on_allocation("CreateCommittedResource2", reinterpret_cast<const D3D12_RESOURCE_DESC*>(desc));
    return committed2Original(d, hp, hf, desc, s, cv, ps, riid, out);
}
HRESULT STDMETHODCALLTYPE placed1Hook(ID3D12Device8* d, ID3D12Heap* heap, UINT64 offset, const D3D12_RESOURCE_DESC1* desc,
                                      D3D12_RESOURCE_STATES s, const D3D12_CLEAR_VALUE* cv, REFIID riid, void** out) {
    on_allocation("CreatePlacedResource1", reinterpret_cast<const D3D12_RESOURCE_DESC*>(desc));
    return placed1Original(d, heap, offset, desc, s, cv, riid, out);
}
HRESULT STDMETHODCALLTYPE committed3Hook(ID3D12Device10* d, const D3D12_HEAP_PROPERTIES* hp, D3D12_HEAP_FLAGS hf,
                                         const D3D12_RESOURCE_DESC1* desc, D3D12_BARRIER_LAYOUT layout,
                                         const D3D12_CLEAR_VALUE* cv, ID3D12ProtectedResourceSession* ps, UINT32 nf,
                                         const DXGI_FORMAT* formats, REFIID riid, void** out) {
    on_allocation("CreateCommittedResource3", reinterpret_cast<const D3D12_RESOURCE_DESC*>(desc));
    return committed3Original(d, hp, hf, desc, layout, cv, ps, nf, formats, riid, out);
}
HRESULT STDMETHODCALLTYPE placed2Hook(ID3D12Device10* d, ID3D12Heap* heap, UINT64 offset, const D3D12_RESOURCE_DESC1* desc,
                                      D3D12_BARRIER_LAYOUT layout, const D3D12_CLEAR_VALUE* cv, UINT32 nf,
                                      const DXGI_FORMAT* formats, REFIID riid, void** out) {
    on_allocation("CreatePlacedResource2", reinterpret_cast<const D3D12_RESOURCE_DESC*>(desc));
    return placed2Original(d, heap, offset, desc, layout, cv, nf, formats, riid, out);
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

}  // namespace

bool d3d12_trace_start(ID3D12Device* device) {
    if (started || !device)
        return started;
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> allocator;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> list;
    if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))) ||
        FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr,
                                         IID_PPV_ARGS(&list)))) {
        log_line("d3d12 trace: could not create a probe command list");
        return false;
    }
    list->Close();
    rtvIncrement = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    void** deviceVtable = *reinterpret_cast<void***>(device);
    void** listVtable = *reinterpret_cast<void***>(list.Get());
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList4> list4;
    Microsoft::WRL::ComPtr<ID3D12Device4> device4;
    Microsoft::WRL::ComPtr<ID3D12Device8> device8;
    Microsoft::WRL::ComPtr<ID3D12Device10> device10;
    const bool hasRenderPasses = SUCCEEDED(list.As(&list4));
    const bool has4 = SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&device4)));
    const bool has8 = SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&device8)));
    const bool has10 = SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&device10)));

    bool ok = hook(deviceVtable, aceqv_slot_CreateRenderTargetView(), &createRtvHook, &createRtvOriginal) &&
              hook(deviceVtable, aceqv_slot_CreateCommittedResource(), &committedHook, &committedOriginal) &&
              hook(deviceVtable, aceqv_slot_CreatePlacedResource(), &placedHook, &placedOriginal) &&
              hook(listVtable, aceqv_slot_OMSetRenderTargets(), &omSetHook, &omSetOriginal) &&
              hook(listVtable, aceqv_slot_RSSetViewports(), &viewportsHook, &viewportsOriginal);
    // Newer interfaces on the same object: their methods extend the device's single vtable.
    if (ok && has4)
        ok = hook(*reinterpret_cast<void***>(device4.Get()), aceqv_slot_CreateCommittedResource1(), &committed1Hook,
                  &committed1Original);
    if (ok && has8)
        ok = hook(*reinterpret_cast<void***>(device8.Get()), aceqv_slot_CreateCommittedResource2(), &committed2Hook,
                  &committed2Original) &&
             hook(*reinterpret_cast<void***>(device8.Get()), aceqv_slot_CreatePlacedResource1(), &placed1Hook,
                  &placed1Original);
    if (ok && has10)
        ok = hook(*reinterpret_cast<void***>(device10.Get()), aceqv_slot_CreateCommittedResource3(), &committed3Hook,
                  &committed3Original) &&
             hook(*reinterpret_cast<void***>(device10.Get()), aceqv_slot_CreatePlacedResource2(), &placed2Hook,
                  &placed2Original);
    if (ok && hasRenderPasses)
        ok = hook(*reinterpret_cast<void***>(list4.Get()), aceqv_slot_BeginRenderPass(), &beginPassHook,
                  &beginPassOriginal);
    started = ok;
    logf("d3d12 trace: %s (device vtable %p, list vtable %p, render passes %d, device4/8/10 %d/%d/%d, rtv increment %u)",
         ok ? "installed" : "FAILED", deviceVtable, listVtable, hasRenderPasses ? 1 : 0, has4 ? 1 : 0, has8 ? 1 : 0,
         has10 ? 1 : 0, rtvIncrement);
    return ok;
}

void d3d12_trace_register_image(ID3D12Resource* image, uint32_t width, uint32_t height, uint32_t index) {
    AcquireSRWLockExclusive(&lock);
    images[image] = ImageInfo{width, height, index};
    ReleaseSRWLockExclusive(&lock);
}

void d3d12_trace_frame_end(bool sequenceActive) {
    const bool was = sequence.exchange(sequenceActive);
    const uint64_t writes = swapchainHitsThisFrame.exchange(0);
    const uint64_t binds = bindsThisFrame.exchange(0);
    if (was || sequenceActive)
        logf("d3d12 frame summary: swapchain writes=%llu large render-target binds=%llu",
             static_cast<unsigned long long>(writes), static_cast<unsigned long long>(binds));
}

}  // namespace aceqv
