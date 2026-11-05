#include "vram_census.h"

#include <atomic>
#include <cstdlib>
#include <mutex>
#include <unordered_map>

#include <d3d12.h>
#include <wrl/client.h>

#include "../log.h"

extern "C" size_t simqvfr_slot_CreateCommittedResource(void);
extern "C" size_t simqvfr_slot_CreateCommittedResource1(void);
extern "C" size_t simqvfr_slot_CreateCommittedResource2(void);
extern "C" size_t simqvfr_slot_CreateCommittedResource3(void);

namespace simqvfr {
namespace {

// {6b0f3a52-2c1e-4b8e-9d55-7a3f0e6c1d21}: the tracker's private-data slot on each recorded texture.
constexpr GUID kTrackerGuid = {0x6b0f3a52, 0x2c1e, 0x4b8e, {0x9d, 0x55, 0x7a, 0x3f, 0x0e, 0x6c, 0x1d, 0x21}};

struct Texture {
    uint64_t width = 0;
    uint32_t height = 0;
    uint64_t bytes = 0;
};

std::mutex mutex;
std::unordered_map<uint64_t, Texture> live;  // by tracker id
uint64_t nextId = 1;
bool started = false;

// Destroyed with the texture (D3D12 releases private-data interfaces with their object): takes it off.
class Tracker final : public IUnknown {
public:
    explicit Tracker(uint64_t id) : id_(id) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override {
        if (!out)
            return E_POINTER;
        if (riid == __uuidof(IUnknown)) {
            *out = static_cast<IUnknown*>(this);
            AddRef();
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG n = --refs_;
        if (n == 0) {
            {
                std::lock_guard guard(mutex);
                live.erase(id_);
            }
            delete this;
        }
        return n;
    }

private:
    std::atomic<ULONG> refs_{1};
    uint64_t id_;
};

void record(ID3D12Device* device, const D3D12_RESOURCE_DESC* desc, HRESULT hr, void** out) {
    constexpr D3D12_RESOURCE_FLAGS kTargetFlags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET |
                                                  D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL |
                                                  D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    if (FAILED(hr) || !out || !*out || !desc || desc->Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
        !(desc->Flags & kTargetFlags))
        return;
    Microsoft::WRL::ComPtr<ID3D12Object> object;
    if (FAILED(static_cast<IUnknown*>(*out)->QueryInterface(IID_PPV_ARGS(&object))))
        return;
    const D3D12_RESOURCE_ALLOCATION_INFO info = device->GetResourceAllocationInfo(0, 1, desc);
    if (info.SizeInBytes == UINT64_MAX)
        return;
    uint64_t id;
    {
        std::lock_guard guard(mutex);
        id = nextId++;
        live[id] = {desc->Width, desc->Height, info.SizeInBytes};
    }
    auto* tracker = new Tracker(id);
    if (FAILED(object->SetPrivateDataInterface(kTrackerGuid, tracker))) {
        std::lock_guard guard(mutex);
        live.erase(id);
    }
    tracker->Release();  // the texture holds it now (or nobody: then it is gone)
}

using CommittedFn = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, const D3D12_HEAP_PROPERTIES*, D3D12_HEAP_FLAGS,
                                                const D3D12_RESOURCE_DESC*, D3D12_RESOURCE_STATES,
                                                const D3D12_CLEAR_VALUE*, REFIID, void**);
using Committed1Fn = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device4*, const D3D12_HEAP_PROPERTIES*, D3D12_HEAP_FLAGS,
                                                 const D3D12_RESOURCE_DESC*, D3D12_RESOURCE_STATES,
                                                 const D3D12_CLEAR_VALUE*, ID3D12ProtectedResourceSession*, REFIID,
                                                 void**);
using Committed2Fn = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device8*, const D3D12_HEAP_PROPERTIES*, D3D12_HEAP_FLAGS,
                                                 const D3D12_RESOURCE_DESC1*, D3D12_RESOURCE_STATES,
                                                 const D3D12_CLEAR_VALUE*, ID3D12ProtectedResourceSession*, REFIID,
                                                 void**);
using Committed3Fn = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device10*, const D3D12_HEAP_PROPERTIES*, D3D12_HEAP_FLAGS,
                                                 const D3D12_RESOURCE_DESC1*, D3D12_BARRIER_LAYOUT,
                                                 const D3D12_CLEAR_VALUE*, ID3D12ProtectedResourceSession*, UINT32,
                                                 const DXGI_FORMAT*, REFIID, void**);
CommittedFn committedOriginal = nullptr;
Committed1Fn committed1Original = nullptr;
Committed2Fn committed2Original = nullptr;
Committed3Fn committed3Original = nullptr;

HRESULT STDMETHODCALLTYPE committedHook(ID3D12Device* d, const D3D12_HEAP_PROPERTIES* hp, D3D12_HEAP_FLAGS hf,
                                        const D3D12_RESOURCE_DESC* desc, D3D12_RESOURCE_STATES s,
                                        const D3D12_CLEAR_VALUE* cv, REFIID riid, void** out) {
    const HRESULT hr = committedOriginal(d, hp, hf, desc, s, cv, riid, out);
    record(d, desc, hr, out);
    return hr;
}
HRESULT STDMETHODCALLTYPE committed1Hook(ID3D12Device4* d, const D3D12_HEAP_PROPERTIES* hp, D3D12_HEAP_FLAGS hf,
                                         const D3D12_RESOURCE_DESC* desc, D3D12_RESOURCE_STATES s,
                                         const D3D12_CLEAR_VALUE* cv, ID3D12ProtectedResourceSession* ps, REFIID riid,
                                         void** out) {
    const HRESULT hr = committed1Original(d, hp, hf, desc, s, cv, ps, riid, out);
    record(d, desc, hr, out);
    return hr;
}
// D3D12_RESOURCE_DESC1 starts with every D3D12_RESOURCE_DESC field, so the prefix is read as one.
HRESULT STDMETHODCALLTYPE committed2Hook(ID3D12Device8* d, const D3D12_HEAP_PROPERTIES* hp, D3D12_HEAP_FLAGS hf,
                                         const D3D12_RESOURCE_DESC1* desc, D3D12_RESOURCE_STATES s,
                                         const D3D12_CLEAR_VALUE* cv, ID3D12ProtectedResourceSession* ps, REFIID riid,
                                         void** out) {
    const HRESULT hr = committed2Original(d, hp, hf, desc, s, cv, ps, riid, out);
    record(d, reinterpret_cast<const D3D12_RESOURCE_DESC*>(desc), hr, out);
    return hr;
}
HRESULT STDMETHODCALLTYPE committed3Hook(ID3D12Device10* d, const D3D12_HEAP_PROPERTIES* hp, D3D12_HEAP_FLAGS hf,
                                         const D3D12_RESOURCE_DESC1* desc, D3D12_BARRIER_LAYOUT layout,
                                         const D3D12_CLEAR_VALUE* cv, ID3D12ProtectedResourceSession* ps, UINT32 nf,
                                         const DXGI_FORMAT* formats, REFIID riid, void** out) {
    const HRESULT hr = committed3Original(d, hp, hf, desc, layout, cv, ps, nf, formats, riid, out);
    record(d, reinterpret_cast<const D3D12_RESOURCE_DESC*>(desc), hr, out);
    return hr;
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

bool vram_follows(uint64_t width, uint32_t height, int targetWidth, int targetHeight) {
    if (targetWidth <= 0 || targetHeight <= 0)
        return false;
    const auto axis = [](uint64_t size, int target) {
        for (int k = 1; k <= 8; k *= 2)
            if (std::llabs(static_cast<long long>(size) - static_cast<long long>((target + k - 1) / k)) <= 1 ||
                std::llabs(static_cast<long long>(size) - static_cast<long long>(target / k)) <= 1)
                return true;
        return false;
    };
    return axis(width, targetWidth) && axis(height, targetHeight);
}

bool vram_census_start(ID3D12Device* device) {
    if (started || !device)
        return started;
    void** vtable = *reinterpret_cast<void***>(device);
    bool ok = hook(vtable, simqvfr_slot_CreateCommittedResource(), &committedHook, &committedOriginal);
    // Newer interfaces on the same object: their methods extend the device's single vtable.
    Microsoft::WRL::ComPtr<ID3D12Device4> device4;
    Microsoft::WRL::ComPtr<ID3D12Device8> device8;
    Microsoft::WRL::ComPtr<ID3D12Device10> device10;
    if (SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&device4))))
        ok = ok && hook(*reinterpret_cast<void***>(device4.Get()), simqvfr_slot_CreateCommittedResource1(), &committed1Hook,
                        &committed1Original);
    if (SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&device8))))
        ok = ok && hook(*reinterpret_cast<void***>(device8.Get()), simqvfr_slot_CreateCommittedResource2(), &committed2Hook,
                        &committed2Original);
    if (SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&device10))))
        ok = ok && hook(*reinterpret_cast<void***>(device10.Get()), simqvfr_slot_CreateCommittedResource3(), &committed3Hook,
                        &committed3Original);
    started = ok;
    logf("vram census: %s (device4/8/10 %d/%d/%d)", ok ? "on" : "FAILED to patch the device", device4 ? 1 : 0,
         device8 ? 1 : 0, device10 ? 1 : 0);
    return ok;
}

VramCensus vram_census(int renderWidth, int renderHeight, int outputWidth, int outputHeight) {
    VramCensus c;
    std::lock_guard guard(mutex);
    for (const auto& [id, t] : live) {
        if (vram_follows(t.width, t.height, renderWidth, renderHeight) ||
            vram_follows(t.width, t.height, outputWidth, outputHeight)) {
            c.scaledBytes += t.bytes;
            ++c.scaledTextures;
        } else {
            c.otherBytes += t.bytes;
            ++c.otherTextures;
        }
    }
    return c;
}

}  // namespace simqvfr
