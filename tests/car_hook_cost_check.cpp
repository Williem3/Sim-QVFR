// Check outside the game (not in the suite: a measurement, no pass/fail beyond running): what a detour like LMU's car
// hooks (games/lmu/mvr_pass_cars.h: an inline MinHook detour on the immediate context's function, a handler that compares
// the context and the resource, then calls the original) costs per call over the plain call. Map and Unmap of a dynamic
// constant buffer on a WARP device, in interleaved blocks hooked and not; the difference per call, and for the ~34,000
// calls of the car hooks per frame in LMU's traffic (race 2026-11-16). WARP's own Map is far dearer than a GPU driver's,
// so only the difference means anything.
#include <windows.h>
#include <d3d11.h>
#include <wrl/client.h>

#include <MinHook.h>

#include <algorithm>
#include <cstdio>
#include <vector>

#include "test_util.h"

using Microsoft::WRL::ComPtr;

namespace {

using MapFn = HRESULT(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Resource*, UINT, D3D11_MAP, UINT,
                                          D3D11_MAPPED_SUBRESOURCE*);
using UnmapFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Resource*, UINT);

MapFn realMap = nullptr;
UnmapFn realUnmap = nullptr;
ID3D11DeviceContext* gameContext = nullptr;
ID3D11Resource* watched = reinterpret_cast<ID3D11Resource*>(1);  // never matches, as most resources in the game
volatile unsigned long long seen = 0;

// As the car hooks: the context compared, a counter, the resource compared, the original called.
HRESULT STDMETHODCALLTYPE map_detour(ID3D11DeviceContext* c, ID3D11Resource* r, UINT sub, D3D11_MAP type, UINT flags,
                                     D3D11_MAPPED_SUBRESOURCE* mapped) {
    if (c != gameContext)
        return realMap(c, r, sub, type, flags, mapped);
    const HRESULT hr = realMap(c, r, sub, type, flags, mapped);
    seen = seen + 1;
    if (r == watched && SUCCEEDED(hr))
        seen = seen + 1;
    return hr;
}
void STDMETHODCALLTYPE unmap_detour(ID3D11DeviceContext* c, ID3D11Resource* r, UINT sub) {
    if (c == gameContext) {
        seen = seen + 1;
        if (r == watched)
            seen = seen + 1;
    }
    realUnmap(c, r, sub);
}

double now_ms() {
    LARGE_INTEGER t, f;
    QueryPerformanceCounter(&t);
    QueryPerformanceFrequency(&f);
    return 1000.0 * static_cast<double>(t.QuadPart) / static_cast<double>(f.QuadPart);
}

// `pairs` Map/Unmap pairs through the context's vtable (as the game calls them), ms.
double block(ID3D11DeviceContext* c, ID3D11Buffer* b, int pairs) {
    const double t0 = now_ms();
    for (int i = 0; i < pairs; ++i) {
        D3D11_MAPPED_SUBRESOURCE m{};
        if (SUCCEEDED(c->Map(b, 0, D3D11_MAP_WRITE_DISCARD, 0, &m)))
            static_cast<float*>(m.pData)[0] = static_cast<float>(i);
        c->Unmap(b, 0);
    }
    return now_ms() - t0;
}

double median(std::vector<double> v) {
    std::sort(v.begin(), v.end());
    return v.empty() ? 0 : v[v.size() / 2];
}

}  // namespace

int SIM_QVFR_TEST_MAIN() {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &device, nullptr,
                                 &context))) {
        check(false, "a WARP device");
        return test_result();
    }
    gameContext = context.Get();
    D3D11_BUFFER_DESC d{};
    d.ByteWidth = 256;
    d.Usage = D3D11_USAGE_DYNAMIC;
    d.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    d.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    ComPtr<ID3D11Buffer> buffer;
    check(SUCCEEDED(device->CreateBuffer(&d, nullptr, &buffer)), "a dynamic constant buffer");

    void** vtable = *reinterpret_cast<void***>(context.Get());
    void* const mapTarget = vtable[14];    // ID3D11DeviceContext::Map
    void* const unmapTarget = vtable[15];  // ::Unmap
    check(MH_Initialize() == MH_OK || MH_Initialize() == MH_ERROR_ALREADY_INITIALIZED, "MinHook started");
    const bool created = MH_CreateHook(mapTarget, reinterpret_cast<void*>(&map_detour), reinterpret_cast<void**>(&realMap)) ==
                             MH_OK &&
                         MH_CreateHook(unmapTarget, reinterpret_cast<void*>(&unmap_detour),
                                       reinterpret_cast<void**>(&realUnmap)) == MH_OK;
    check(created, "Map and Unmap detoured");

    constexpr int kPairs = 20000, kRounds = 15;
    block(context.Get(), buffer.Get(), kPairs);  // warm up
    std::vector<double> plain, hooked;
    for (int r = 0; r < kRounds; ++r) {
        plain.push_back(block(context.Get(), buffer.Get(), kPairs));
        MH_EnableHook(MH_ALL_HOOKS);
        hooked.push_back(block(context.Get(), buffer.Get(), kPairs));
        MH_DisableHook(MH_ALL_HOOKS);
    }
    MH_RemoveHook(mapTarget);
    MH_RemoveHook(unmapTarget);
    const double p = median(plain), h = median(hooked);
    const double perCallNs = (h - p) * 1e6 / (2.0 * kPairs);
    std::printf("     plain %.3f ms, detoured %.3f ms per %d Map/Unmap pairs (medians of %d): %+.1f ns per call; "
                "for 34000 calls a frame %+.3f ms\n",
                p, h, kPairs, kRounds, perCallNs, perCallNs * 34000 / 1e6);
    check(seen > 0, "the detours ran");
    return test_result();
}
