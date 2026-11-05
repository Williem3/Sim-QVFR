// Offline check of the VRAM census (probe/vram_census.h) on a WARP device: which sizes follow a target,
// render targets recorded at their allocation size and sorted, plain textures ignored, and a freed
// texture taken off again.
#include <cstdio>

#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#include "probe/vram_census.h"

using namespace simqvfr;
using Microsoft::WRL::ComPtr;

namespace {

int failures = 0;

void check(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    failures += ok ? 0 : 1;
}

ComPtr<ID3D12Resource> texture(ID3D12Device* device, UINT64 width, UINT height, D3D12_RESOURCE_FLAGS flags) {
    D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = width;
    desc.Height = height;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Flags = flags;
    ComPtr<ID3D12Resource> r;
    device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COMMON, nullptr,
                                    IID_PPV_ARGS(&r));
    return r;
}

}  // namespace

int main() {
    check(vram_follows(4837, 814, 4837, 814) && vram_follows(2419, 407, 4837, 814) && vram_follows(2418, 814, 4837, 814) &&
              vram_follows(605, 102, 4837, 814),
          "follows: the target, its halves (rounded either way) and an eighth, per axis");
    check(!vram_follows(2048, 2048, 4837, 814) && !vram_follows(1920, 1080, 6434, 1083) && !vram_follows(4837, 814, 0, 0),
          "a shadow map, the desktop mirror and an unknown target do not");

    ComPtr<IDXGIFactory4> factory;
    ComPtr<IDXGIAdapter> warp;
    ComPtr<ID3D12Device> device;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) || FAILED(factory->EnumWarpAdapter(IID_PPV_ARGS(&warp))) ||
        FAILED(D3D12CreateDevice(warp.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)))) {
        std::puts("RESULT: FAIL (no WARP device)");
        return 1;
    }
    check(vram_census_start(device.Get()), "the census patches the device");
    const VramCensus before = vram_census(800, 400, 1200, 600);
    {
        auto render = texture(device.Get(), 800, 400, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
        auto half = texture(device.Get(), 400, 200, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        auto output = texture(device.Get(), 1200, 600, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
        auto shadow = texture(device.Get(), 512, 512, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
        auto plain = texture(device.Get(), 800, 400, D3D12_RESOURCE_FLAG_NONE);
        check(render && half && output && shadow && plain, "textures made through the patched device");
        const VramCensus c = vram_census(800, 400, 1200, 600);
        check(c.scaledTextures - before.scaledTextures == 3 && c.otherTextures - before.otherTextures == 1,
              "render, half-size and output targets follow; the shadow map does not; a plain texture is not counted");
        check(c.scaledBytes - before.scaledBytes >= (800ull * 400 + 400 * 200 + 1200 * 600) * 4,
              "at least their pixels' bytes (allocation sizes)");
        const VramCensus unknown = vram_census(0, 0, 0, 0);
        check(unknown.scaledTextures == 0 && unknown.otherTextures == c.scaledTextures + c.otherTextures,
              "with the target sizes unknown, everything is other");
        output.Reset();
        const VramCensus freed = vram_census(800, 400, 1200, 600);
        check(freed.scaledTextures == c.scaledTextures - 1 && freed.scaledBytes < c.scaledBytes,
              "a freed texture is taken off");
    }
    const VramCensus after = vram_census(800, 400, 1200, 600);
    check(after.scaledTextures == before.scaledTextures && after.otherTextures == before.otherTextures,
          "all freed: back to where it started");
    std::puts(failures ? "RESULT: FAIL" : "RESULT: pass");
    return failures ? 1 : 0;
}
