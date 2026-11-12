// Offline check of the quad compositor on a WARP device: frame A (outer) and frame B (focus) are
// double-wide images cleared to a different colour per eye; the composed output must show the
// outer colour outside each eye's focus box and the focus colour inside it, with the boxes placed
// where the per-eye focus rectangles say.
#include <cmath>
#include <cstdio>
#include <cstring>

#include <string>
#include <vector>

#include <dxgi1_6.h>
#include <wincodec.h>
#include <wrl/client.h>

#include "quad/compositor.h"
#include "quad/focus_turn.h"
#include "quad/fov_crop.h"
#include "quad/helmet.h"
#include "quad/views4_layout.h"

using Microsoft::WRL::ComPtr;
using namespace simqvfr;

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

// A 1-pixel-wide PNG of `rows` RGBA rows (straight alpha), for the helmet's pieces.
bool write_png(const std::wstring& path, const std::vector<uint32_t>& rows) {
    const UINT height = static_cast<UINT>(rows.size());
    std::vector<uint8_t> bgra(height * 4);
    for (UINT y = 0; y < height; ++y) {
        const uint32_t c = rows[y];  // 0xRRGGBBAA
        bgra[y * 4 + 0] = static_cast<uint8_t>(c >> 8);
        bgra[y * 4 + 1] = static_cast<uint8_t>(c >> 16);
        bgra[y * 4 + 2] = static_cast<uint8_t>(c >> 24);
        bgra[y * 4 + 3] = static_cast<uint8_t>(c);
    }
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    ComPtr<IWICImagingFactory> wic;
    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapEncoder> encoder;
    ComPtr<IWICBitmapFrameEncode> frame;
    WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
    return SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic))) &&
           SUCCEEDED(wic->CreateStream(&stream)) && SUCCEEDED(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE)) &&
           SUCCEEDED(wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder)) &&
           SUCCEEDED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache)) &&
           SUCCEEDED(encoder->CreateNewFrame(&frame, nullptr)) && SUCCEEDED(frame->Initialize(nullptr)) &&
           SUCCEEDED(frame->SetSize(1, height)) && SUCCEEDED(frame->SetPixelFormat(&format)) &&
           SUCCEEDED(frame->WritePixels(height, 4, static_cast<UINT>(bgra.size()), bgra.data())) &&
           SUCCEEDED(frame->Commit()) && SUCCEEDED(encoder->Commit());
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

int SIM_QVFR_TEST_MAIN() {
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
    const std::wstring prefix = std::wstring(temp) + L"simqvfr-compositor-test";
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
        // A wide, short focus box: 100% density x 39% width x 25% height, periphery 50%.
        const Views4Layout box = views4_layout(0.5, 0.39, 0.25);
        const ViewRect b0 = views4_rect(box, 0, 3560, 1000), b2 = views4_rect(box, 2, 3560, 1000);
        std::printf("     focus box 39%% x 25%% in 3560x1000: outer %dx%d, focus at x=%d %dx%d\n", b0.width, b0.height,
                    b2.left, b2.width, b2.height);
        check(b0.width == 1000 && b0.height == 1000 && b2.left == 2000 && b2.width == 780 && b2.height == 500 &&
                  std::abs(box.eye_width() - 0.89) < 1e-9 && box.eye_height() == 0.5 && !box.equal(),
              "focus width and height are independent: 780x500 focus views beside 1000x1000 outer views");

        // Live resolution: the views are laid out again inside the launch target, one row from x = 0.
        const Views4Layout shrunk = views4_live(box, 0.25, 0.39, 0.125);
        const ViewRect s0 = views4_rect(shrunk, 0, 3560, 1000), s1 = views4_rect(shrunk, 1, 3560, 1000),
                       s2 = views4_rect(shrunk, 2, 3560, 1000);
        std::printf("     live 25%% / 39%% x 12.5%%: outer %dx%d, view 1 at x=%d, focus at x=%d %dx%d\n", s0.width,
                    s0.height, s1.left, s2.left, s2.width, s2.height);
        check(s0.width == 500 && s0.height == 500 && s1.left == 500 && s2.left == 1000 && s2.width == 780 &&
                  s2.height == 250 && !shrunk.equal() && shrunk.eye_width() == box.eye_width(),
              "live sizes re-lay the views inside the launch target (the target itself unchanged)");
        // The 14:19 launch (periphery 10%, focus 36% x 30% of a 5190x4168 eye: a 4774x1250 target).
        const Views4Layout run = views4_layout(0.10, 0.36, 0.30);
        const Views4Fit traded = views4_fit(run, 0.22, 0.24, 0.20), over = views4_fit(run, 0.30, 0.24, 0.20),
                        roomy = views4_fit(run, 0.30, 0.15, 0.125);
        const Views4Layout tradedLayout = views4_live(run, 0.22, 0.24, 0.20);
        const ViewRect t0 = views4_rect(tradedLayout, 0, 4774, 1250), t3 = views4_rect(tradedLayout, 3, 4774, 1250);
        check(!traded.outerCapped && !traded.focusCapped && t0.width == 1141 && t0.height == 916 &&
                  t3.left + t3.width <= 4774,
              "a smaller focus area makes room: periphery 22% (above its launch 10%) fits");
        check(over.outerCapped && over.outerByRoom && std::abs(over.outer - 0.22) < 1e-9 && !over.focusCapped,
              "periphery 30% beside a 24% focus area is held at the room left (22%)");
        check(!roomy.outerCapped && std::abs(roomy.outer - 0.30) < 1e-9,
              "with a 15% focus area, periphery 30% (the target's height) fits");
        const Views4Fit grown = views4_fit(box, 0.8, 0.6, 0.6);
        check(grown.focusCapped && std::abs(grown.focus - 0.5) < 1e-9 && std::abs(grown.focusHeight - 0.5) < 1e-9 &&
                  grown.outerCapped && grown.outerByRoom && std::abs(grown.outer - 0.39) < 1e-9,
              "nothing exceeds the largest launch view or the target; focus first, the periphery takes what is left");
        const Views4Layout tiny = views4_live(box, 0.001, 0.39, 0.25);
        check(views4_rect(tiny, 0, 3560, 1000).width == 100, "live sizes keep at least kMinLive of the launch size");
    }

    // FOV crop: the sides are independent, outer is the temple side of each eye, in tangent space.
    {
        const XrFovf fov{std::atan(-1.0f), std::atan(1.0f), std::atan(1.0f), std::atan(-1.0f)};
        const FovCrop crop = fov_crop(20, 10, 25, 5);
        float left[4], right[4];
        fov_tangents(crop_fov(fov, crop, 0), left);
        fov_tangents(crop_fov(fov, crop, 1), right);
        const auto close = [](float a, double b) { return std::abs(a - b) < 1e-5; };
        check(close(left[0], -0.5) && close(left[1], 0.9) && close(left[2], 0.6) && close(left[3], -0.8),
              "crop: left eye loses 25% at its temple (left), 5% at the nose, 20% top, 10% bottom");
        check(close(right[0], -0.9) && close(right[1], 0.5) && close(right[2], 0.6) && close(right[3], -0.8),
              "crop: the right eye mirrors outer and inner");
        const FovCrop moreTop = fov_crop(35, 10, 25, 5);
        float t[4];
        fov_tangents(crop_fov(fov, moreTop, 0), t);
        check(close(t[0], -0.5) && close(t[1], 0.9) && close(t[2], 0.3) && close(t[3], -0.8) &&
                  std::abs(moreTop.kept_width() - crop.kept_width()) < 1e-12,
              "crop: more off the top leaves the bottom and both sides as they were");
        const FovCrop clamped = fov_crop(90, -5, 0, 0);
        check(clamped.top == kCropMax && clamped.bottom == 0 && !FovCrop{}.on() && clamped.on(),
              "crop: each side is clamped to 0..kCropMax on its own");
        check(std::abs(cropped_share(0.31, 0.7) - 0.31 / 0.7) < 1e-12 && cropped_share(0.8, 0.6) == 1.0,
              "crop: the focus box keeps its share of the uncropped eye (at most the whole cropped view)");

        // The outer views shrink by the kept shares and keep their density; the focus views are unchanged.
        const Views4Layout plain = views4_layout(0.5, 0.39, 0.25), cut = views4_layout(0.5, 0.39, 0.25, 0.7, 0.6);
        check(std::abs(cut.outer_width() - 0.35) < 1e-12 && std::abs(cut.outer_height() - 0.30) < 1e-12 &&
                  std::abs(cut.eye_width() - 0.74) < 1e-12 && std::abs(cut.eye_height() - 0.30) < 1e-12,
              "crop layout: outer views 0.35 x 0.30 native, the offered eye 0.74 x 0.30");
        const ViewRect c0 = views4_rect(cut, 0, 2960, 1200), c2 = views4_rect(cut, 2, 2960, 1200);
        std::printf("     crop 70%% x 60%% in 2960x1200: outer %dx%d, focus at x=%d %dx%d\n", c0.width, c0.height, c2.left,
                    c2.width, c2.height);
        check(c0.width == 700 && c0.height == 1200 && c2.left == 1400 && c2.width == 780 && c2.height == 1000 &&
                  views4_rect(plain, 2, 3560, 1000).width == 780,
              "crop layout: 700x1200 outer views beside unchanged 780x1000 focus views");
        // Live: the periphery's density, its views keeping the crop's shape.
        const Views4Layout live = views4_live(cut, 0.25, 0.39, 0.25);
        const ViewRect l0 = views4_rect(live, 0, 2960, 1200);
        check(l0.width == 350 && l0.height == 600, "crop live: periphery 25% gives 350x600 outer views");
        // The helmet's uncropped eye from a cropped view, and its shape.
        float cut0[4], back[4];
        fov_tangents(crop_fov(fov, crop, 0), cut0);
        uncrop_tangents(cut0, crop, 0, back);
        float cut1[4], back1[4];
        fov_tangents(crop_fov(fov, crop, 1), cut1);
        uncrop_tangents(cut1, crop, 1, back1);
        check(close(back[0], -1) && close(back[1], 1) && close(back[2], 1) && close(back[3], -1) && close(back1[0], -1) &&
                  close(back1[1], 1),
              "helmet: uncrop_tangents recovers each eye's uncropped FOV");
        HelmetSettings hs;
        hs.rim = 10;
        hs.corners = 50;
        const HelmetShape shape = helmet_shape(hs, back, cut0);
        check(close(shape.rim, 0.2) && close(shape.radius, 0.5 * 0.5 * 1.4) && close(shape.opening[0], -0.5),
              "helmet: the rim is a share of the eye's height, the corners of half the opening's shorter side");
        const Views4Fit over = views4_fit(cut, 0.8, 0.39, 0.25);
        check(over.outerCapped && std::abs(over.outer - 0.5) < 1e-9,
              "crop live: the periphery's density is capped at its launch target (the cropped views' height)");
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

        // Debug outline in a chosen colour: teal (#00A0A0, linear 0.352) on eye 0's box edge.
        v4[0].border = 0.05f;
        const float teal[4] = {0, 0.352f, 0.352f, 1};
        std::memcpy(v4[0].borderColor, teal, sizeof(teal));
        if (!c4.compose(views.Get(), output.Get(), v4)) {
            std::printf("FAIL: views4 compose with outline\n");
            return 1;
        }
        const Pixels o = read_back(gpu, output.Get());
        const uint8_t* edge = o.at(static_cast<UINT>(0.26f * (kOutW / 2)), kOutH / 2);
        const bool tealEdge = edge[0] == 0 && std::abs(edge[1] - 160) <= 2 && std::abs(edge[2] - 160) <= 2;
        std::printf("%s eye 0 outline at the box edge got %3u,%3u,%3u (teal)\n", tealEdge ? "ok  " : "FAIL", edge[0],
                    edge[1], edge[2]);
        failures += tealEdge ? 0 : 1;
        expect(o, 0, 0.50f, 0.50f, "focus", 0, 255, 0);  // inside the outline: still the focus view

        // Rounded corners: radius 100% turns eye 0's square box (tangents -0.5..0.5) into a circle.
        // Just inside the box's corner is now outside the circle (the outer view), the edge
        // midpoints and the centre are still the focus view, and the outline follows the curve.
        v4[0].border = 0;
        v4[0].cornerRadius = 1.0f;
        if (!c4.compose(views.Get(), output.Get(), v4)) {
            std::printf("FAIL: views4 compose with rounded corners\n");
            return 1;
        }
        const Pixels round = read_back(gpu, output.Get());
        expect(round, 0, 0.27f, 0.27f, "outer", 255, 0, 0);  // the box's top-left corner, cut off
        expect(round, 0, 0.73f, 0.73f, "outer", 255, 0, 0);  // bottom-right
        expect(round, 0, 0.50f, 0.27f, "focus", 0, 255, 0);  // top edge midpoint
        expect(round, 0, 0.27f, 0.50f, "focus", 0, 255, 0);  // left edge midpoint
        expect(round, 0, 0.50f, 0.50f, "focus", 0, 255, 0);
        expect(round, 1, 0.27f, 0.27f, "focus", 255, 255, 0);  // eye 1 keeps square corners
        v4[0].border = 0.05f;
        if (!c4.compose(views.Get(), output.Get(), v4)) {
            std::printf("FAIL: views4 compose with a rounded outline\n");
            return 1;
        }
        const Pixels ring = read_back(gpu, output.Get());
        // Inside the ring (tangent radius 0.45..0.5) at 45 degrees: tangent 0.475 / sqrt 2 = 0.336 on
        // each axis, i.e. 0.168 of the eye's width and height (tangents -1..1) from the centre.
        const uint8_t* arc = ring.at(static_cast<UINT>((0.5f - 0.168f) * (kOutW / 2)), static_cast<UINT>((0.5f - 0.168f) * kOutH));
        const bool onArc = arc[0] == 0 && std::abs(arc[1] - 160) <= 2 && std::abs(arc[2] - 160) <= 2;
        std::printf("%s eye 0 rounded outline on the arc got %3u,%3u,%3u (teal)\n", onArc ? "ok  " : "FAIL", arc[0], arc[1], arc[2]);
        failures += onArc ? 0 : 1;

        // Corner shape: eye 0's top corners pulled 40% in (a trapezoid, the top edge 0.4..0.6 of the box, the
        // left edge from there to the bottom-left corner). It replaces the rounded corners; the outline
        // follows the slanted edge.
        v4[0].border = 0;
        v4[0].cornerInset[0] = 0.4f;  // top left, across
        v4[0].cornerInset[2] = 0.4f;  // top right, across
        if (!c4.compose(views.Get(), output.Get(), v4)) {
            std::printf("FAIL: views4 compose with a corner shape\n");
            return 1;
        }
        const Pixels trapezoid = read_back(gpu, output.Get());
        expect(trapezoid, 0, 0.27f, 0.27f, "outer", 255, 0, 0);  // the box's top-left corner, cut off
        expect(trapezoid, 0, 0.73f, 0.27f, "outer", 255, 0, 0);  // top-right
        expect(trapezoid, 0, 0.27f, 0.50f, "outer", 255, 0, 0);  // left of the slanted edge halfway down
        expect(trapezoid, 0, 0.50f, 0.27f, "focus", 0, 255, 0);  // top edge midpoint
        expect(trapezoid, 0, 0.27f, 0.73f, "focus", 0, 255, 0);  // bottom-left corner, kept
        expect(trapezoid, 0, 0.73f, 0.73f, "focus", 0, 255, 0);  // bottom-right (not rounded)
        expect(trapezoid, 0, 0.40f, 0.50f, "focus", 0, 255, 0);
        expect(trapezoid, 1, 0.27f, 0.27f, "focus", 255, 255, 0);  // eye 1 keeps the box
        v4[0].border = 0.05f;
        if (!c4.compose(views.Get(), output.Get(), v4)) {
            std::printf("FAIL: views4 compose with a corner shape outline\n");
            return 1;
        }
        const Pixels slant = read_back(gpu, output.Get());
        // Halfway down, the left edge is at 0.2 of the box: 0.23 is just inside it, within the outline.
        const uint8_t* side = slant.at(static_cast<UINT>((0.25f + 0.5f * 0.23f) * (kOutW / 2)), kOutH / 2);
        const bool onSide = side[0] == 0 && std::abs(side[1] - 160) <= 2 && std::abs(side[2] - 160) <= 2;
        std::printf("%s eye 0 corner shape outline on the slanted edge got %3u,%3u,%3u (teal)\n", onSide ? "ok  " : "FAIL",
                    side[0], side[1], side[2]);
        failures += onSide ? 0 : 1;
        expect(slant, 0, 0.50f, 0.50f, "focus", 0, 255, 0);
        std::memset(v4[0].cornerInset, 0, sizeof(v4[0].cornerInset));

        // FOV crop: eye 0 drawn into the top-left 50% x 75% of its half; the rest left as it was.
        clear_eyes(gpu, output.Get(), black, black);
        v4[0].border = 0;
        v4[0].cornerRadius = 0;
        v4[0].outputSize[0] = 0.5f;
        v4[0].outputSize[1] = 0.75f;
        if (!c4.compose(views.Get(), output.Get(), v4)) {
            std::printf("FAIL: views4 compose with a crop\n");
            return 1;
        }
        const Pixels cropped = read_back(gpu, output.Get());
        expect(cropped, 0, 0.05f, 0.05f, "outer", 255, 0, 0);
        expect(cropped, 0, 0.25f, 0.375f, "focus", 0, 255, 0);  // the box, centred in the drawn rect
        expect(cropped, 0, 0.75f, 0.30f, "cropped", 0, 0, 0);    // right of the drawn rect
        expect(cropped, 0, 0.20f, 0.90f, "cropped", 0, 0, 0);    // below it
        expect(cropped, 1, 0.50f, 0.50f, "focus", 255, 255, 0);  // eye 1 (no outputSize) fills its half
        expect(cropped, 1, 0.95f, 0.95f, "outer", 0, 0, 255);

        // The helmet: eye 0's output covers tangents -2..2, ACE rendered -1..1 (the opening). Inside the
        // opening beyond the rim the view is untouched; the lip just inside its edge and the shell outside
        // it take the frame's light (eye 0's outer view is red), the lip brighter; eye 1 stays as it was.
        clear_eyes(gpu, output.Get(), black, black);
        v4[0].outputSize[0] = v4[0].outputSize[1] = 0;
        const float wide[4] = {-2, 2, 2, -2}, opening[4] = {-1, 1, 1, -1};
        std::memcpy(v4[0].viewTan, wide, sizeof(wide));
        std::memcpy(v4[0].helmetOpening, opening, sizeof(opening));
        v4[0].helmetRim = 0.1f;
        v4[0].helmetRadius = 0;
        v4[0].helmetLight = 1;
        v4[0].helmet = 1;
        if (!c4.compose(views.Get(), output.Get(), v4)) {
            std::printf("FAIL: views4 compose with the helmet\n");
            return 1;
        }
        const Pixels hq = read_back(gpu, output.Get());
        expect(hq, 0, 0.50f, 0.50f, "focus", 0, 255, 0);
        expect(hq, 0, 0.30f, 0.50f, "outer", 255, 0, 0);  // inside the opening, past the rim
        const uint8_t* lip = hq.at(static_cast<UINT>(0.252f * (kOutW / 2)), kOutH / 2);
        const uint8_t* shell = hq.at(static_cast<UINT>(0.05f * (kOutW / 2)), kOutH / 2);
        const bool lit = lip[0] > 40 && lip[0] < 200 && lip[1] < 12 && lip[2] < 12 && shell[0] > 5 && shell[0] < lip[0] &&
                         shell[1] < 8 && shell[2] < 8;
        std::printf("%s helmet lip %3u,%3u,%3u, shell %3u,%3u,%3u (red light, lip brighter)\n", lit ? "ok  " : "FAIL",
                    lip[0], lip[1], lip[2], shell[0], shell[1], shell[2]);
        failures += lit ? 0 : 1;
        expect(hq, 1, 0.50f, 0.50f, "focus", 255, 255, 0);
        // The light for the log: asked for, copied at the next composition, read once the GPU is done.
        float light[2][4][3] = {};
        const bool none = !c4.read_light(light);
        c4.request_light();
        if (!c4.compose(views.Get(), output.Get(), v4)) {
            std::printf("FAIL: views4 compose for the light copy\n");
            return 1;
        }
        read_back(gpu, output.Get());  // waits for the queue
        const bool readLight = c4.read_light(light);
        bool sides = readLight;
        for (int s = 0; s < 4; ++s)
            sides = sides && std::abs(light[0][s][0] - 1) < 0.01f && light[0][s][1] < 0.01f && light[0][s][2] < 0.01f &&
                    light[1][s][0] < 0.01f && light[1][s][1] < 0.01f && std::abs(light[1][s][2] - 1) < 0.01f;
        std::printf("%s helmet light read back: eye 0 left %.3f/%.3f/%.3f, eye 1 left %.3f/%.3f/%.3f\n",
                    none && sides && !c4.read_light(light) ? "ok  " : "FAIL", light[0][0][0], light[0][0][1],
                    light[0][0][2], light[1][0][0], light[1][0][1], light[1][0][2]);
        failures += none && sides && !c4.read_light(light) ? 0 : 1;
        // The light diagnostic: the shell shows the frame's light itself (eye 0's sides are all red).
        v4[0].helmet = 3;
        if (!c4.compose(views.Get(), output.Get(), v4)) {
            std::printf("FAIL: views4 compose with the helmet's light diagnostic\n");
            return 1;
        }
        const Pixels dq = read_back(gpu, output.Get());
        expect(dq, 0, 0.05f, 0.50f, "the frame's light", 255, 0, 0);

        // The pieces (light 0: as drawn), over the whole output (tangents -2..2, no turn), 8 rows each: the
        // top piece's first quarter is white, the bottom piece's last quarter yellow, the visor's second
        // quarter black; the rest of each is clear (white, so a fringe would show). The visor slid up by a
        // quarter takes its band with it.
        wchar_t tempDir[MAX_PATH];
        GetTempPathW(MAX_PATH, tempDir);
        const std::wstring top = std::wstring(tempDir) + L"simqvfr-helmet-top.png",
                           bottom = std::wstring(tempDir) + L"simqvfr-helmet-bottom.png",
                           visor = std::wstring(tempDir) + L"simqvfr-helmet-visor.png";
        const uint32_t clear = 0xFFFFFF00;
        const bool written =
            write_png(top, {0xFFFFFFFF, 0xFFFFFFFF, clear, clear, clear, clear, clear, clear}) &&
            write_png(bottom, {clear, clear, clear, clear, clear, clear, 0xFFFF00FF, 0xFFFF00FF}) &&
            write_png(visor, {clear, clear, 0x000000FF, 0x000000FF, clear, clear, clear, clear});
        const bool loaded = written && c4.load_helmet_piece(HelmetPiece::Top, top) &&
                            c4.load_helmet_piece(HelmetPiece::Bottom, bottom) && c4.load_helmet_piece(HelmetPiece::Visor, visor);
        check(loaded && c4.has_helmet_pieces() && std::abs(c4.helmet_edge(HelmetPiece::Top) - 0.25) < 1e-9 &&
                  std::abs(c4.helmet_edge(HelmetPiece::Bottom) - 0.25) < 1e-9 && c4.helmet_aspect() == 8,
              "helmet pieces: loaded, each opaque edge found down the middle");
        const float identity[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
        std::memcpy(v4[0].helmetRotation, identity, sizeof(identity));
        v4[0].helmetTanHalf[0] = v4[0].helmetTanHalf[1] = 2;
        v4[0].helmetLight = 0;
        v4[0].visorOffset = 0;
        v4[0].helmet = 2;
        if (!c4.compose(views.Get(), output.Get(), v4)) {
            std::printf("FAIL: views4 compose with the helmet's pieces\n");
            return 1;
        }
        const Pixels pq = read_back(gpu, output.Get());
        expect(pq, 0, 0.30f, 0.05f, "top piece", 255, 255, 255);
        expect(pq, 0, 0.30f, 0.95f, "bottom piece", 255, 255, 0);
        expect(pq, 0, 0.30f, 0.375f, "visor band", 0, 0, 0);
        expect(pq, 0, 0.30f, 0.60f, "the scene through them", 255, 0, 0);
        // Depth: the eye's shift of the images' tangents moves them (up 0.5 here: the top piece reaches lower).
        v4[0].helmetShift[1] = 0.5f;
        if (!c4.compose(views.Get(), output.Get(), v4)) {
            std::printf("FAIL: views4 compose with a depth shift\n");
            return 1;
        }
        const Pixels hs = read_back(gpu, output.Get());
        expect(hs, 0, 0.30f, 0.30f, "top piece, shifted down by depth", 255, 255, 255);
        v4[0].helmetShift[1] = 0;
        const uint8_t* unshifted = pq.at(static_cast<UINT>(0.30f * (kOutW / 2)), static_cast<UINT>(0.30f * kOutH));
        check(!(unshifted[0] == 255 && unshifted[1] == 255 && unshifted[2] == 255),
              "without the shift the top piece does not reach that point");
        v4[0].visorOffset = 0.25f;
        if (!c4.compose(views.Get(), output.Get(), v4)) {
            std::printf("FAIL: views4 compose with the visor slid up\n");
            return 1;
        }
        const Pixels sq = read_back(gpu, output.Get());
        expect(sq, 0, 0.30f, 0.375f, "the scene where the visor was", 255, 0, 0);
        expect(sq, 0, 0.30f, 0.05f, "top piece over the slid visor", 255, 255, 255);
        // The light diagnostic with pieces: their shapes in the frame's light (eye 0's sides are red), the
        // scene where they are clear.
        v4[0].helmet = 3;
        if (!c4.compose(views.Get(), output.Get(), v4)) {
            std::printf("FAIL: views4 compose with the pieces' light diagnostic\n");
            return 1;
        }
        const Pixels cq = read_back(gpu, output.Get());
        expect(cq, 0, 0.30f, 0.05f, "top piece in the frame's light", 255, 0, 0);
        expect(cq, 0, 0.30f, 0.60f, "the scene through the pieces", 255, 0, 0);
        v4[0].helmet = 2;

        // An opaque visor is glass: white passes the scene as it is, a mid-grey band (linear 0.5) halves it;
        // slid up past the band, the scene is whole again; below its image there is no visor.
        const bool glass = write_png(visor, {0xFFFFFFFF, 0xFFFFFFFF, 0xBCBCBCFF, 0xBCBCBCFF, 0xFFFFFFFF, 0xFFFFFFFF,
                                             0xFFFFFFFF, 0xFFFFFFFF}) &&
                           c4.load_helmet_piece(HelmetPiece::Top, L"") && c4.load_helmet_piece(HelmetPiece::Visor, visor);
        v4[0].visorOffset = 0;
        v4[0].visorBlend = 0;
        if (!glass || !c4.compose(views.Get(), output.Get(), v4)) {
            std::printf("FAIL: views4 compose with a glass visor\n");
            return 1;
        }
        const Pixels gq = read_back(gpu, output.Get());
        expect(gq, 0, 0.30f, 0.60f, "clear glass", 255, 0, 0);
        expect(gq, 0, 0.30f, 0.375f, "the tinted band (half the light)", 188, 0, 0);
        v4[0].visorOffset = 0.75f;  // the band above the view, the image's bottom at a quarter
        if (!c4.compose(views.Get(), output.Get(), v4)) {
            std::printf("FAIL: views4 compose with the glass visor open\n");
            return 1;
        }
        const Pixels oq = read_back(gpu, output.Get());
        expect(oq, 0, 0.30f, 0.375f, "no visor below its image", 255, 0, 0);
        // The helmet's own layer (light 0: as drawn), the quad over exactly the images: the top piece white
        // and opaque, the clear middle transparent (nothing drawn, nothing hidden), the glass band letting half
        // the scene through (alpha ~0.5, no colour); edges extend outward from a quad wider than the images.
        const bool layerPieces = write_png(top, {0xFFFFFFFF, 0xFFFFFFFF, clear, clear, clear, clear, clear, clear}) &&
                                 c4.load_helmet_piece(HelmetPiece::Top, top);
        auto layer = make_image(gpu, 64, 64);
        HelmetLayerDraw draw{};
        draw.coverTan[0] = draw.coverTan[1] = draw.imageTan[0] = draw.imageTan[1] = 1;
        std::memcpy(draw.outerSource[0], v4[0].outerSource, sizeof(draw.outerSource[0]));
        std::memcpy(draw.outerSource[1], v4[1].outerSource, sizeof(draw.outerSource[1]));
        v4[0].visorOffset = 0;
        if (!layerPieces || !c4.compose(views.Get(), output.Get(), v4) || !c4.draw_helmet_layer(layer.Get(), draw)) {
            std::printf("FAIL: the helmet layer\n");
            return 1;
        }
        const Pixels lq = read_back(gpu, layer.Get());
        const auto rgba = [&](float u, float v) { return lq.at(static_cast<UINT>(u * 64), static_cast<UINT>(v * 64)); };
        const uint8_t *topPx = rgba(0.5f, 0.1f), *middle = rgba(0.5f, 0.65f), *band = rgba(0.5f, 0.375f);
        const bool layerOk = topPx[0] == 255 && topPx[3] == 255 && middle[0] == 0 && middle[3] == 0 &&
                             band[0] == 0 && std::abs(band[3] - 127) <= 2;
        std::printf("%s helmet layer: top %u/%u, middle %u/%u, glass band %u/%u (colour/alpha)\n", layerOk ? "ok  " : "FAIL",
                    topPx[0], topPx[3], middle[0], middle[3], band[0], band[3]);
        failures += layerOk ? 0 : 1;
        draw.coverTan[0] = draw.coverTan[1] = 2;  // the quad twice the images: the top piece's edge reaches out
        if (!c4.load_helmet_piece(HelmetPiece::Visor, L"") || !c4.draw_helmet_layer(layer.Get(), draw)) {
            std::printf("FAIL: the wider helmet layer\n");
            return 1;
        }
        const Pixels wq = read_back(gpu, layer.Get());
        const uint8_t *corner = wq.at(1, 1), *centre = wq.at(32, 32);
        check(corner[0] == 255 && corner[3] == 255 && centre[3] == 0,
              "helmet layer wider than the images: their edges extend to its corners, the middle stays clear");

        // Only the bands: the layer image cleared magenta, the top quarter drawn; the rest left as it was.
        const float magentaClear[4] = {1, 0, 1, 1};
        clear_eyes(gpu, layer.Get(), magentaClear, magentaClear);
        draw.coverTan[0] = draw.coverTan[1] = 1;
        draw.bands = {{0, 0.25f}};
        if (!c4.draw_helmet_layer(layer.Get(), draw)) {
            std::printf("FAIL: the banded helmet layer\n");
            return 1;
        }
        const Pixels bq = read_back(gpu, layer.Get());
        const uint8_t *drawnTop = bq.at(32, 4), *untouched = bq.at(32, 48);
        check(drawnTop[0] == 255 && drawnTop[1] == 255 && drawnTop[3] == 255 && untouched[0] == 255 && untouched[1] == 0 &&
                  untouched[2] == 255,
              "helmet layer: only its bands are drawn (below the top band the image is untouched)");
        draw.bands.clear();

        // The rows the pieces draw on: the top piece's first quarter; a glass visor only where it tints (its grey
        // band, rows 2-3 of 8: the second quarter), slid up with it; as drawn (alpha), all of it.
        const bool rowPieces = write_png(visor, {0xFFFFFFFF, 0xFFFFFFFF, 0xBCBCBCFF, 0xBCBCBCFF, 0xFFFFFFFF, 0xFFFFFFFF,
                                                 0xFFFFFFFF, 0xFFFFFFFF}) &&
                               c4.load_helmet_piece(HelmetPiece::Visor, visor);
        const auto covered = [](const std::vector<bool>& rows, double v) { return rows[static_cast<size_t>(v * kHelmetRows)]; };
        const std::vector<bool> closedRows = c4.helmet_rows(0, 0), slidRows = c4.helmet_rows(0.25, 0), asDrawn = c4.helmet_rows(0, 2);
        check(rowPieces && covered(closedRows, 0.1) && covered(closedRows, 0.4) && !covered(closedRows, 0.7) &&
                  covered(slidRows, 0.1) && !covered(slidRows, 0.4) && covered(asDrawn, 0.7),
              "helmet rows: the brow, the visor's tinted band (gone when slid up), all of an alpha visor");
        {
        // The bands from the rows: drawn at the top and bottom quarters, clear between (one row of margin).
        std::vector<bool> rows(kHelmetRows, false);
        for (int i = 0; i < kHelmetRows / 4; ++i)
            rows[i] = rows[kHelmetRows - 1 - i] = true;
        const std::vector<HelmetBand> two = helmet_bands(rows, 1, 1);
        check(two.size() == 2 && two[0].top == 0 && std::abs(two[0].bottom - 65.0f / 256) < 1e-6f &&
                  std::abs(two[1].top - 191.0f / 256) < 1e-6f && two[1].bottom == 1,
              "helmet bands: the brow and chin bands, the clear middle left out");
        const std::vector<HelmetBand> wider = helmet_bands(rows, 1, 2);  // the quad twice the images' height
        check(wider.size() == 2 && std::abs(wider[0].bottom - (0.5f - (0.5f - 65.0f / 256) / 2)) < 1e-6f,
              "helmet bands: mapped from the images onto a larger quad");
        std::vector<bool> top(kHelmetRows, false);
        for (int i = 0; i < kHelmetRows / 4; ++i)
            top[i] = true;
        const std::vector<HelmetBand> brow = helmet_bands(top, 1, 1);
        check(brow.size() == 1 && brow[0].top == 0 && brow[0].bottom < 0.3f,
              "helmet bands: a clear bottom row leaves that end of the quad out");
        check(helmet_bands(std::vector<bool>(kHelmetRows, true), 1, 1).size() == 1 &&
                  helmet_bands(std::vector<bool>(kHelmetRows, true), 1, 1)[0].bottom == 1,
              "helmet bands: drawn everywhere (a closed tinted visor), the whole quad");
        std::vector<bool> nearly(kHelmetRows, true);
        for (int i = 120; i < 130; ++i)
            nearly[i] = false;
        check(helmet_bands(nearly, 1, 1).size() == 1, "helmet bands: a clear run under 8% of the quad is kept");
        }
        DeleteFileW(top.c_str());
        DeleteFileW(bottom.c_str());
        DeleteFileW(visor.c_str());

        // Depth: an eye 3.2 cm left of the middle sees the images 8 cm away shifted by -0.4 (tangent); far, not at all.
        HelmetSettings deep;
        deep.depth = 8;
        float shift[2] = {};
        const float leftEye[3] = {-0.032f, 0, 0};
        helmet_parallax(deep, leftEye, shift);
        HelmetSettings distant;
        float noShift[2] = {1, 1};
        helmet_parallax(distant, leftEye, noShift);
        check(std::abs(shift[0] + 0.4f) < 1e-6f && shift[1] == 0 && noShift[0] == 0 && noShift[1] == 0 &&
                  helmet_depth(1) == kHelmetDepthMin && helmet_depth(0) == 0 && helmet_depth(999) == kHelmetDepthMax,
              "helmet depth: the parallax of an eye at 8 cm; none when far; depth clamps (0 is far)");
        // The quad's cover: eyes straight ahead seeing tangents -1..1 need +-1 (and a 5% margin); an eye 3.2 cm
        // left of the middle with the quad 8 cm away needs 0.4 more across.
        const float straight[2][9] = {{1, 0, 0, 0, 1, 0, 0, 0, 1}, {1, 0, 0, 0, 1, 0, 0, 0, 1}};
        const float centred[2][3] = {}, apart[2][3] = {{-0.032f, 0, 0}, {0.032f, 0, 0}};
        const float eyeViews[2][4] = {{-1, 1, 1, -1}, {-1, 1, 1, -1}};
        double coverX = 0, coverY = 0, nearX = 0, nearY = 0;
        helmet_cover(straight, centred, eyeViews, 10, coverX, coverY);
        helmet_cover(straight, apart, eyeViews, 0.08, nearX, nearY);
        check(std::abs(coverX - 1.05) < 1e-6 && std::abs(coverY - 1.05) < 1e-6 && std::abs(nearX - 1.4 * 1.05) < 1e-5 &&
                  std::abs(nearY - 1.05) < 1e-6,
              "helmet layer: the quad covers both eyes' views, wider by the parallax when near");
        // Kept far: each eye's copy of a near quad (8, 30 and 79 cm; a band 0.03 m above the middle) is kHelmetFarM away
        // and shows that eye every point along the same line as the near quad would.
        bool sameLines = true, allFar = true;
        for (const double nearM : {0.08, 0.30, 0.79}) {
            const float rise = 0.03f;
            const double scale = helmet_far_scale(nearM);
            for (const auto& eye : apart) {
                float at[3];
                helmet_far_centre(nearM, rise, eye, at);
                allFar = allFar && std::abs(at[2] + kHelmetFarM) < 1e-4;
                for (const double u : {-0.2, 0.0, 0.15})
                    for (const double v : {-0.05, 0.04}) {
                        const double p[3] = {u, rise + v, -nearM};                                   // on the near quad
                        const double q[3] = {at[0] + u * scale, at[1] + v * scale, at[2]};           // its far copy
                        const double a[3] = {p[0] - eye[0], p[1] - eye[1], p[2] - eye[2]};
                        const double b[3] = {q[0] - eye[0], q[1] - eye[1], q[2] - eye[2]};
                        const double cross[3] = {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2],
                                                 a[0] * b[1] - a[1] * b[0]};
                        const double na = std::sqrt(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]),
                                     nb = std::sqrt(b[0] * b[0] + b[1] * b[1] + b[2] * b[2]);
                        sameLines = sameLines && std::sqrt(cross[0] * cross[0] + cross[1] * cross[1] + cross[2] * cross[2]) /
                                                         (na * nb) < 1e-6;
                    }
            }
        }
        float farAt[3];
        helmet_far_centre(kHelmetFarM, 0.5f, apart[0], farAt);
        check(sameLines && allFar && helmet_far_scale(kHelmetFarM) == 1 && farAt[0] == 0 && farAt[1] == 0.5f,
              "helmet layer: kept far, each eye sees its copy along the same lines as a near quad (far: as it was)");
        // The crop hint: an image 110 degrees wide and as high, its top piece opaque down to a quarter
        // of its height and its bottom piece up from three quarters, over an eye of tangents +-1.
        HelmetSettings pieces;
        int hintTop = 0, hintBottom = 0;
        helmet_crop_hint(pieces, 1.0, 0.25, 0.75, 1, -1, hintTop, hintBottom);
        const double openingTan = std::tan(110 * kHelmetDegrees / 2) * 0.5;  // tangent of the opening's edge
        check(hintTop == std::lround((1 - openingTan) / 2 * 100) && hintBottom == hintTop,
              "helmet: the crop hint meets the pieces' opening");
        c4.shutdown();
    }

    // Sharpening on D3D12 (ACE): the same shader and root constants as the D3D11 compositor's (cas_sharpen_test checks
    // the filter itself). Flat views at 100% give the plain composition's pixels exactly; mid-grey stripes in the outer
    // view (4 texels each, upscaled 2x) come out with more contrast, the stripes' middles unchanged.
    {
        clear_eyes(gpu, frameA.Get(), red, blue);
        bool same = compositor.capture_outer(frameA.Get());
        EyeComposition sharp[2] = {eyes[0], eyes[1]};
        for (EyeComposition& e : sharp)
            e.sharpness = 1.0f;
        same = same && compositor.compose(frameB.Get(), output.Get(), sharp);
        const Pixels flat = read_back(gpu, output.Get());
        same = same && compositor.compose(frameB.Get(), output.Get(), eyes);
        const Pixels plainFlat = read_back(gpu, output.Get());
        for (UINT y = 0; y < kOutH; ++y)
            same = same && std::memcmp(flat.at(0, y), plainFlat.at(0, y), kOutW * 4) == 0;
        check(same, "sharpening (D3D12): flat views at 100% give the plain composition exactly");

        const float dark[4] = {0.2f, 0.2f, 0.2f, 1}, lighter[4] = {0.5f, 0.5f, 0.5f, 1};
        D3D12_RECT stripes[16];
        float colors[16][4];
        for (int i = 0; i < 16; ++i) {
            stripes[i] = {i * 8, 0, i * 8 + 4, static_cast<LONG>(kAppH)};
            std::memcpy(colors[i], lighter, sizeof(lighter));
        }
        clear_rects(gpu, frameA.Get(), dark, stripes, colors, 16);
        Pixels rows[2];
        bool composed = compositor.capture_outer(frameA.Get());
        for (int pass = 0; pass < 2; ++pass) {
            composed = composed && compositor.compose(frameB.Get(), output.Get(), pass ? sharp : eyes);
            rows[pass] = read_back(gpu, output.Get());
        }
        // Eye 0's row 6, above its focus box: all outer view.
        const UINT y = 6;
        int variation[2] = {}, middleOff = 0;
        for (int pass = 0; pass < 2; ++pass)
            for (UINT x = 1; x < kOutW / 2; ++x)
                variation[pass] += std::abs(static_cast<int>(rows[pass].at(x, y)[1]) - static_cast<int>(rows[pass].at(x - 1, y)[1]));
        // The output pixels between a stripe's second and third texels (x = 2s + 3, 2s + 4 for a stripe from s): all
        // their taps inside the stripe, so CAS leaves them as bilinear does.
        for (UINT s = 8; s + 8 < kAppW / 2; s += 8)
            for (const UINT x : {2 * s + 3, 2 * s + 4})
                middleOff = std::max(middleOff, std::abs(static_cast<int>(rows[1].at(x, y)[1]) - static_cast<int>(rows[0].at(x, y)[1])));
        char what[160];
        std::snprintf(what, sizeof(what), "sharpening (D3D12): stripes' contrast up (variation %d against %d), their middles "
                      "as bilinear's (off by %d at most)", variation[1], variation[0], middleOff);
        check(composed && variation[1] > variation[0] * 11 / 10 && middleOff <= 1, what);
    }

    compositor.shutdown();
    std::printf("%s (%d failures)\n", failures ? "FAILED" : "PASSED", failures);
    return failures ? 1 : 0;
}
