// Offline check of the quad compositor on a WARP device: frame A (outer) and frame B (focus) are
// double-wide images cleared to a different colour per eye; the composed output must show the
// outer colour outside each eye's focus box and the focus colour inside it, with the boxes placed
// where the per-eye focus rectangles say.
#include <cmath>
#include <cstdio>
#include <cstring>

#include <dxgi1_6.h>
#include <wrl/client.h>

#include "quad/compositor.h"
#include "quad/views4_layout.h"

using Microsoft::WRL::ComPtr;
using namespace aceqv;

namespace {

constexpr UINT kAppW = 128, kAppH = 32;  // double-wide app images (64 per eye)
constexpr UINT kOutW = 256, kOutH = 64;  // double-wide output (128 per eye)
constexpr DXGI_FORMAT kTypeless = DXGI_FORMAT_R8G8B8A8_TYPELESS;
constexpr DXGI_FORMAT kView = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;

int failures = 0;

struct Gpu {
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12Fence> fence;
    UINT64 fenceValue = 0;
    ComPtr<ID3D12DescriptorHeap> rtvHeap;

    void flush() {
        list->Close();
        ID3D12CommandList* lists[] = {list.Get()};
        queue->ExecuteCommandLists(1, lists);
        queue->Signal(fence.Get(), ++fenceValue);
        HANDLE e = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        fence->SetEventOnCompletion(fenceValue, e);
        WaitForSingleObject(e, INFINITE);
        CloseHandle(e);
        allocator->Reset();
        list->Reset(allocator.Get(), nullptr);
    }
};

ComPtr<ID3D12Resource> make_image(Gpu& gpu, UINT w, UINT h) {
    D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
    D3D12_RESOURCE_DESC d = {};
    d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    d.Width = w;
    d.Height = h;
    d.DepthOrArraySize = 1;
    d.MipLevels = 1;
    d.Format = kTypeless;
    d.SampleDesc.Count = 1;
    d.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    ComPtr<ID3D12Resource> r;
    gpu.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr,
                                        IID_PPV_ARGS(&r));
    return r;
}

// Clears each eye half of a double-wide image to its own colour.
void clear_eyes(Gpu& gpu, ID3D12Resource* image, const float (&left)[4], const float (&right)[4]) {
    D3D12_RENDER_TARGET_VIEW_DESC v = {};
    v.Format = kView;
    v.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = gpu.rtvHeap->GetCPUDescriptorHandleForHeapStart();
    gpu.device->CreateRenderTargetView(image, &v, rtv);
    const LONG w = static_cast<LONG>(image->GetDesc().Width), h = static_cast<LONG>(image->GetDesc().Height);
    D3D12_RECT l{0, 0, w / 2, h}, r{w / 2, 0, w, h};
    gpu.list->ClearRenderTargetView(rtv, left, 1, &l);
    gpu.list->ClearRenderTargetView(rtv, right, 1, &r);
    gpu.flush();
}

// Clears the whole image to `fill`, then each rect to its colour.
void clear_rects(Gpu& gpu, ID3D12Resource* image, const float (&fill)[4], const D3D12_RECT* rects,
                 const float (*colors)[4], int count) {
    D3D12_RENDER_TARGET_VIEW_DESC v = {};
    v.Format = kView;
    v.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = gpu.rtvHeap->GetCPUDescriptorHandleForHeapStart();
    gpu.device->CreateRenderTargetView(image, &v, rtv);
    gpu.list->ClearRenderTargetView(rtv, fill, 0, nullptr);
    for (int i = 0; i < count; ++i)
        gpu.list->ClearRenderTargetView(rtv, colors[i], 1, &rects[i]);
    gpu.flush();
}

struct Pixels {
    std::vector<uint8_t> data;
    UINT pitch = 0;
    const uint8_t* at(UINT x, UINT y) const { return &data[y * pitch + x * 4]; }
};

Pixels read_back(Gpu& gpu, ID3D12Resource* image) {
    const D3D12_RESOURCE_DESC d = image->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp;
    UINT64 total = 0;
    gpu.device->GetCopyableFootprints(&d, 0, 1, 0, &fp, nullptr, nullptr, &total);
    D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_READBACK};
    D3D12_RESOURCE_DESC b = {};
    b.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    b.Width = total;
    b.Height = b.DepthOrArraySize = b.MipLevels = 1;
    b.SampleDesc.Count = 1;
    b.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> buffer;
    gpu.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &b, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                        IID_PPV_ARGS(&buffer));
    D3D12_RESOURCE_BARRIER barrier{D3D12_RESOURCE_BARRIER_TYPE_TRANSITION};
    barrier.Transition = {image, 0, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE};
    gpu.list->ResourceBarrier(1, &barrier);
    D3D12_TEXTURE_COPY_LOCATION dst{buffer.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
    dst.PlacedFootprint = fp;
    D3D12_TEXTURE_COPY_LOCATION src{image, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
    src.SubresourceIndex = 0;
    gpu.list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
    gpu.list->ResourceBarrier(1, &barrier);
    gpu.flush();
    Pixels p;
    p.pitch = fp.Footprint.RowPitch;
    p.data.resize(static_cast<size_t>(total));
    void* mapped = nullptr;
    buffer->Map(0, nullptr, &mapped);
    std::memcpy(p.data.data(), mapped, p.data.size());
    buffer->Unmap(0, nullptr);
    return p;
}

void check(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    failures += ok ? 0 : 1;
}

void expect(const Pixels& p, int eye, float u, float v, const char* name, uint8_t r, uint8_t g, uint8_t b) {
    const UINT x = static_cast<UINT>((eye + u) * (kOutW / 2)), y = static_cast<UINT>(v * kOutH);
    const uint8_t* px = p.at(x, y);
    const bool ok = px[0] == r && px[1] == g && px[2] == b;
    std::printf("%s eye %d (%.2f,%.2f) %-6s got %3u,%3u,%3u\n", ok ? "ok  " : "FAIL", eye, u, v, name, px[0], px[1],
                px[2]);
    failures += ok ? 0 : 1;
}

}  // namespace

int main() {
    Gpu gpu;
    ComPtr<IDXGIFactory4> factory;
    ComPtr<IDXGIAdapter> warp;
    if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory))) || FAILED(factory->EnumWarpAdapter(IID_PPV_ARGS(&warp))) ||
        FAILED(D3D12CreateDevice(warp.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&gpu.device)))) {
        std::printf("FAIL: no WARP device\n");
        return 1;
    }
    D3D12_COMMAND_QUEUE_DESC qd{D3D12_COMMAND_LIST_TYPE_DIRECT};
    gpu.device->CreateCommandQueue(&qd, IID_PPV_ARGS(&gpu.queue));
    gpu.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&gpu.allocator));
    gpu.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, gpu.allocator.Get(), nullptr,
                                  IID_PPV_ARGS(&gpu.list));
    gpu.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gpu.fence));
    D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1};
    gpu.device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&gpu.rtvHeap));

    auto frameA = make_image(gpu, kAppW, kAppH);
    auto frameB = make_image(gpu, kAppW, kAppH);
    auto output = make_image(gpu, kOutW, kOutH);

    const float red[4] = {1, 0, 0, 1}, blue[4] = {0, 0, 1, 1}, green[4] = {0, 1, 0, 1}, yellow[4] = {1, 1, 0, 1},
                black[4] = {0, 0, 0, 1};
    clear_eyes(gpu, frameA.Get(), red, blue);
    clear_eyes(gpu, frameB.Get(), green, yellow);

    QuadCompositor compositor;
    if (!compositor.initialize(gpu.device.Get(), gpu.queue.Get(), kView) || !compositor.capture_outer(frameA.Get())) {
        std::printf("FAIL: compositor setup\n");
        return 1;
    }
    // The app reuses its swapchain image after release: the captured outer copy must not change.
    clear_eyes(gpu, frameA.Get(), black, black);

    // Both eyes' frame A spans tangents -1..1. Eye 0: frame B looks straight ahead with tangents
    // -0.5..0.5, so its box is uv 0.25..0.75. Eye 1: frame B is turned right towards A tangent 0.5
    // (yaw atan 0.5) with tangents -0.2..0.2, so its box spans A tangents tan(atan 0.5 -+ atan 0.2)
    // = 0.273..0.778 across (uv 0.636..0.889) and is centred vertically.
    EyeComposition eyes[2] = {};
    const float yaw = std::atan(0.5f), c = std::cos(yaw), s = std::sin(yaw);
    const float rotations[2][9] = {{1, 0, 0, 0, 1, 0, 0, 0, 1}, {c, 0, s, 0, 1, 0, -s, 0, c}};
    const float focusTan[2][4] = {{-0.5f, 0.5f, 0.5f, -0.5f}, {-0.2f, 0.2f, 0.2f, -0.2f}};
    for (int e = 0; e < 2; ++e) {
        const float outerTan[4] = {-1, 1, 1, -1};
        std::memcpy(eyes[e].outerTan, outerTan, sizeof(outerTan));
        std::memcpy(eyes[e].focusTan, focusTan[e], sizeof(focusTan[e]));
        std::memcpy(eyes[e].rotation, rotations[e], sizeof(rotations[e]));
        const float src[4] = {e * 0.5f, 0.0f, 0.5f, 1.0f};
        std::memcpy(eyes[e].outerSource, src, sizeof(src));
        std::memcpy(eyes[e].focusSource, src, sizeof(src));
        eyes[e].feather = 0.1f;
    }
    // Compose twice, as the layer does every frame, to exercise slot reuse.
    for (int i = 0; i < 2; ++i) {
        if (!compositor.compose(frameB.Get(), output.Get(), eyes)) {
            std::printf("FAIL: compose\n");
            return 1;
        }
    }
    const Pixels p = read_back(gpu, output.Get());

    expect(p, 0, 0.05f, 0.05f, "outer", 255, 0, 0);
    expect(p, 0, 0.95f, 0.95f, "outer", 255, 0, 0);
    expect(p, 0, 0.50f, 0.50f, "focus", 0, 255, 0);
    expect(p, 1, 0.75f, 0.50f, "focus", 255, 255, 0);
    expect(p, 1, 0.60f, 0.50f, "outer", 0, 0, 255);  // left of the turned box
    expect(p, 1, 0.92f, 0.50f, "outer", 0, 0, 255);  // right of it
    expect(p, 1, 0.75f, 0.25f, "outer", 0, 0, 255);  // above it
    expect(p, 1, 0.25f, 0.50f, "outer", 0, 0, 255);  // straight ahead

    // Feather: just inside the eye-0 box edge the colour is a blend of red and green.
    const uint8_t* edge = p.at(static_cast<UINT>(0.26f * (kOutW / 2)), kOutH / 2);
    const bool blended = edge[0] > 0 && edge[1] > 0;
    std::printf("%s eye 0 feather edge got %3u,%3u,%3u\n", blended ? "ok  " : "FAIL", edge[0], edge[1], edge[2]);
    failures += blended ? 0 : 1;

    // Debug captures: three PNGs of the left eye.
    wchar_t temp[MAX_PATH];
    GetTempPathW(MAX_PATH, temp);
    const std::wstring prefix = std::wstring(temp) + L"aceqv-compositor-test";
    const bool saved = compositor.save_captures(frameB.Get(), output.Get(), prefix);
    WIN32_FILE_ATTRIBUTE_DATA info{};
    const bool exists = GetFileAttributesExW((prefix + L"-output.png").c_str(), GetFileExInfoStandard, &info) &&
                        info.nFileSizeLow > 0;
    std::printf("%s debug captures written (%ls-*.png)\n", saved && exists ? "ok  " : "FAIL", prefix.c_str());
    failures += saved && exists ? 0 : 1;

    // views4 layout: equal quarters reproduce the old `quarter = width / 4` rects, unequal ones abut.
    {
        const Views4Layout equal = views4_layout(0.4, 0.4);
        bool same = true;
        for (const int w : {3228, 1614, 807})
            for (int v = 0; v < 4; ++v) {
                const ViewRect r = views4_rect(equal, v, w, 1350);
                same &= r.left == v * (w / 4) && r.width == w / 4 && r.height == 1350;
            }
        check(same, "equal layout gives the old quarters (3228, 1614 and 807 wide)");
        const Views4Layout l = views4_layout(0.4, 0.31);  // periphery 40%, focus 100% density x 31%
        const ViewRect r0 = views4_rect(l, 0, 5731, 1350), r2 = views4_rect(l, 2, 5731, 1350),
                       r3 = views4_rect(l, 3, 5731, 1350);
        std::printf("     unequal 5731x1350: outer %dx%d, focus at x=%d %dx%d, view 3 at x=%d\n", r0.width, r0.height,
                    r2.left, r2.width, r2.height, r3.left);
        check(r0.width == 1614 && r0.height == 1350 && r2.left == 2 * 1614 && r2.width == 1251 && r2.height == 1046 &&
                  r3.left == r2.left + r2.width && r3.left + r3.width <= 5731,
              "unequal layout: 1614x1350 outer, 1251x1046 focus views, abutting");
        check(std::abs(l.eye_width() - 0.71) < 1e-9 && l.eye_height() == 0.4, "offered eye is 0.71 x 0.40 native");
    }

    // Unequal sources: outer views 64x64 and focus views 32x32 in a 192x64 image, the rows below the
    // focus views unused (magenta). Neither a neighbour view nor the unused rows may bleed into the output.
    {
        const Views4Layout l = views4_layout(0.5, 0.25);
        constexpr int kW = 192, kH = 64;
        D3D12_RECT rects[4];
        for (int v = 0; v < 4; ++v) {
            const ViewRect r = views4_rect(l, v, kW, kH);
            rects[v] = {r.left, 0, r.left + r.width, r.height};
        }
        check(rects[1].left == 64 && rects[2].left == 128 && rects[2].bottom == 32 && rects[3].right == 192,
              "test layout is 64 | 64 | 32 | 32 wide, focus 32 high");
        const float magenta[4] = {1, 0, 1, 1};
        const float colors[4][4] = {{1, 0, 0, 1}, {0, 0, 1, 1}, {0, 1, 0, 1}, {1, 1, 0, 1}};
        auto views = make_image(gpu, kW, kH);
        clear_rects(gpu, views.Get(), magenta, rects, colors, 4);
        QuadCompositor c4;
        if (!c4.initialize(gpu.device.Get(), gpu.queue.Get(), kView) || !c4.capture_outer(views.Get())) {
            std::printf("FAIL: views4 compositor setup\n");
            return 1;
        }
        EyeComposition v4[2] = {};
        for (int e = 0; e < 2; ++e) {
            const float outerTan[4] = {-1, 1, 1, -1}, focusTan[4] = {-0.5f, 0.5f, 0.5f, -0.5f};
            const float identity[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
            std::memcpy(v4[e].outerTan, outerTan, sizeof(outerTan));
            std::memcpy(v4[e].focusTan, focusTan, sizeof(focusTan));
            std::memcpy(v4[e].rotation, identity, sizeof(identity));
            views4_uv(l, e, kW, kH, v4[e].outerSource);
            views4_uv(l, e + 2, kW, kH, v4[e].focusSource);
            v4[e].feather = 0;  // hard edge: any bleed at the box edge would show
        }
        if (!c4.compose(views.Get(), output.Get(), v4)) {
            std::printf("FAIL: views4 compose\n");
            return 1;
        }
        const Pixels q = read_back(gpu, output.Get());
        expect(q, 0, 0.05f, 0.05f, "outer", 255, 0, 0);
        expect(q, 0, 0.99f, 0.50f, "outer", 255, 0, 0);  // right edge, next to view 1 (blue)
        expect(q, 0, 0.50f, 0.99f, "outer", 255, 0, 0);  // bottom edge
        expect(q, 0, 0.50f, 0.50f, "focus", 0, 255, 0);
        expect(q, 0, 0.26f, 0.50f, "focus", 0, 255, 0);  // box left edge, next to view 1
        expect(q, 0, 0.50f, 0.74f, "focus", 0, 255, 0);  // box bottom edge, above the unused rows
        expect(q, 1, 0.01f, 0.50f, "outer", 0, 0, 255);  // left edge, next to view 0 (red)
        expect(q, 1, 0.99f, 0.50f, "outer", 0, 0, 255);  // right edge, next to view 2 (green)
        expect(q, 1, 0.50f, 0.50f, "focus", 255, 255, 0);
        expect(q, 1, 0.26f, 0.50f, "focus", 255, 255, 0);  // box left edge, next to view 2
        expect(q, 1, 0.74f, 0.74f, "focus", 255, 255, 0);  // bottom right corner of the box
        c4.shutdown();
    }

    compositor.shutdown();
    std::printf("%s (%d failures)\n", failures ? "FAILED" : "PASSED", failures);
    return failures ? 1 : 0;
}
