#pragma once

// RF2 diagnostics, log only: what the game does around the eye passes' resolves, and how the OpenXR swapchain images
// are filled. It changes no state. Lines start with "rf2 trace:".
//
// 1. swapchain images: which device they are on (against the renderer's), their size, format and flags.
// 2. copies into those images (the first 16, then every 600th): which function, which source, which context/device.
// 3. up to 3 sampled frames (the first once the compositions run, then 1500 frames apart), every event of the
//    tracked context: render-target binds (target, depth, viewport, the full-scene pass number), the draws between
//    them, resolves, copies, ExecuteCommandList. It shows what runs after an eye's resolve (post-processing).

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <vector>

#include <d3d11.h>
#include <wrl/client.h>

#include "../../log.h"

namespace simqvfr::games::rf2 {

class Trace {
public:
    static const char* format_name(int f) {
        switch (f) {
            case 2: return "R32G32B32A32_FLOAT";
            case 9: return "R16G16B16A16_TYPELESS";
            case 10: return "R16G16B16A16_FLOAT";
            case 19: return "R32G8X24_TYPELESS";
            case 20: return "D32_FLOAT_S8X24_UINT";
            case 23: return "R10G10B10A2_TYPELESS";
            case 24: return "R10G10B10A2_UNORM";
            case 26: return "R11G11B10_FLOAT";
            case 27: return "R8G8B8A8_TYPELESS";
            case 28: return "R8G8B8A8_UNORM";
            case 29: return "R8G8B8A8_UNORM_SRGB";
            case 34: return "R16G16_FLOAT";
            case 39: return "R32_TYPELESS";
            case 40: return "D32_FLOAT";
            case 41: return "R32_FLOAT";
            case 44: return "R24G8_TYPELESS";
            case 45: return "D24_UNORM_S8_UINT";
            case 46: return "R24_UNORM_X8_TYPELESS";
            case 49: return "R8G8_UNORM";
            case 54: return "R16_FLOAT";
            case 55: return "D16_UNORM";
            case 61: return "R8_UNORM";
            case 87: return "B8G8R8A8_UNORM";
            case 90: return "B8G8R8A8_TYPELESS";
            case 91: return "B8G8R8A8_UNORM_SRGB";
            default: return "?";
        }
    }

    static void describe(ID3D11Resource* r, char* out, size_t n) {
        if (!r) {
            std::snprintf(out, n, "null");
            return;
        }
        Microsoft::WRL::ComPtr<ID3D11Texture2D> t;
        if (FAILED(r->QueryInterface(IID_PPV_ARGS(&t))) || !t) {
            std::snprintf(out, n, "%p non-2D", static_cast<void*>(r));
            return;
        }
        D3D11_TEXTURE2D_DESC d{};
        t->GetDesc(&d);
        std::snprintf(out, n, "%p %ux%u %d(%s) x%u mips=%u arr=%u bind=0x%x misc=0x%x", static_cast<void*>(r),
                      d.Width, d.Height, static_cast<int>(d.Format), format_name(static_cast<int>(d.Format)),
                      d.SampleDesc.Count, d.MipLevels, d.ArraySize, static_cast<unsigned>(d.BindFlags),
                      static_cast<unsigned>(d.MiscFlags));
    }

    // ---- the OpenXR swapchain images (any thread) ----
    void note_swapchain_images(ID3D11Device* renderer, ID3D11Texture2D* const* images, uint32_t count) {
        if (!images || !count || !images[0]) return;
        unsigned onRenderer = 0;
        {
            std::lock_guard<std::mutex> lock(imagesMutex_);
            renderer_ = renderer;
            for (uint32_t i = 0; i < count; ++i) {
                if (!images[i]) continue;
                Microsoft::WRL::ComPtr<ID3D11Device> device;
                images[i]->GetDevice(&device);
                if (device.Get() == renderer) ++onRenderer;
                if (images_.size() < 64) images_.push_back(images[i]);
            }
        }
        Microsoft::WRL::ComPtr<ID3D11Device> first;
        images[0]->GetDevice(&first);
        char d[200];
        describe(images[0], d, sizeof d);
        simqvfr::logf("rf2 trace: swapchain images: count=%u on the renderer's device=%u/%u (first image's device=%p, "
                      "renderer=%p); first=[%s]",
                      count, onRenderer, count, static_cast<void*>(first.Get()), static_cast<void*>(renderer), d);
    }

    // ---- copies, on any context (the d3d11 functions are shared by every device) ----
    void on_copy(ID3D11DeviceContext* context, ID3D11DeviceContext* tracked, bool replaying, ID3D11Resource* dst,
                 ID3D11Resource* src, const char* what) {
        if (replaying) return;
        ID3D11Device* renderer = nullptr;
        int index = -1;
        {
            std::lock_guard<std::mutex> lock(imagesMutex_);
            renderer = renderer_;
            for (size_t i = 0; i < images_.size(); ++i)
                if (images_[i] == dst) {
                    index = static_cast<int>(i);
                    break;
                }
        }
        if (index >= 0) {
            const uint64_t n = ++swapchainCopies_;
            if (n > 16 && n % 600 != 0) return;
            Microsoft::WRL::ComPtr<ID3D11Device> device;
            dst->GetDevice(&device);
            char a[200], b[200];
            describe(dst, a, sizeof a);
            describe(src, b, sizeof b);
            simqvfr::logf("rf2 trace: %s INTO swapchain image #%d (copy %llu, frame %llu): ctx=%s device=%s; "
                          "dst=[%s] src=[%s]",
                          what, index, static_cast<unsigned long long>(n),
                          static_cast<unsigned long long>(frame_.load()), context == tracked ? "tracked" : "other",
                          device.Get() == renderer ? "renderer's" : "other", a, b);
            return;
        }
        if (!active_.load() || context != tracked) return;
        flush_draws();
        char a[200], b[200];
        describe(dst, a, sizeof a);
        describe(src, b, sizeof b);
        line("rf2 trace: f%llu %s dst=[%s] src=[%s]", static_cast<unsigned long long>(frame_.load()), what, a, b);
    }

    // ---- the sampled frames (the tracked context's thread) ----
    void on_draw(bool trackedContext) {
        if (!active_.load()) return;
        if (trackedContext) ++draws_;
        else ++otherDraws_;
    }

    void on_bind(ID3D11DeviceContext* context, UINT n, ID3D11RenderTargetView* const* rtvs,
                 ID3D11DepthStencilView* dsv, unsigned pass, bool scene) {
        if (!active_.load()) return;
        flush_draws();
        Microsoft::WRL::ComPtr<ID3D11Resource> resource;
        if (n && rtvs && rtvs[0]) rtvs[0]->GetResource(&resource);
        D3D11_VIEWPORT vp{};
        UINT count = 1;
        context->RSGetViewports(&count, &vp);
        if (!count) vp = {};
        if (haveKey_ && keyRtv_ == resource.Get() && keyDsv_ == dsv && keyW_ == vp.Width && keyH_ == vp.Height) return;
        haveKey_ = true;
        keyRtv_ = resource.Get();
        keyDsv_ = dsv;
        keyW_ = vp.Width;
        keyH_ = vp.Height;
        char d[200];
        describe(resource.Get(), d, sizeof d);
        line("rf2 trace: f%llu bind#%u after-scene-pass=%u%s rtv0=[%s] n=%u dsv=%d vp=%.0fx%.0f",
             static_cast<unsigned long long>(frame_.load()), ++binds_, pass, scene ? " SCENE" : "", d,
             static_cast<unsigned>(n), dsv ? 1 : 0, vp.Width, vp.Height);
    }

    void on_resolve(ID3D11Resource* dst, ID3D11Resource* src) {
        if (!active_.load()) return;
        flush_draws();
        char a[200], b[200];
        describe(src, a, sizeof a);
        describe(dst, b, sizeof b);
        line("rf2 trace: f%llu RESOLVE src=[%s] -> dst=[%s]", static_cast<unsigned long long>(frame_.load()), a, b);
    }

    void on_execute(bool restore) {
        if (!active_.load()) return;
        flush_draws();
        ++execs_;
        line("rf2 trace: f%llu ExecuteCommandList restore=%d", static_cast<unsigned long long>(frame_.load()),
             restore ? 1 : 0);
    }

    // The tracked context's frame end. `compositions`: the eyes composed so far.
    void frame_end(unsigned long long frame, uint64_t compositions) {
        if (active_.load()) {
            flush_draws();
            simqvfr::logf("rf2 trace: f%llu END: binds=%u tracked-context draws=%u other-context draws=%u "
                          "cmdlists=%u (%u lines)",
                          frame + 1, binds_, totalDraws_, otherDraws_.load(), execs_, lines_);
            active_ = false;
            lastTraced_ = frame;
            ++traced_;
        }
        frame_ = frame + 1;
        if (compositions >= 400 && traced_ < 3 && frame >= lastTraced_ + 1500) {
            lines_ = binds_ = execs_ = 0;
            draws_ = totalDraws_ = 0;
            otherDraws_ = 0;
            haveKey_ = false;
            active_ = true;
            simqvfr::logf("rf2 trace: f%llu BEGIN (sample %u of 3)", frame + 1, traced_ + 1);
        }
    }

private:
    void flush_draws() {
        if (draws_) {
            line("rf2 trace:     draws=%u", draws_);
            totalDraws_ += draws_;
            draws_ = 0;
        }
    }

    template <class... A>
    void line(const char* format, A... a) {
        if (lines_ >= kMaxLines) {
            if (lines_ == kMaxLines) {
                ++lines_;
                simqvfr::log_line("rf2 trace: (this frame's lines truncated)");
            }
            return;
        }
        ++lines_;
        simqvfr::logf(format, a...);
    }

    static constexpr unsigned kMaxLines = 400;
    std::mutex imagesMutex_;
    std::vector<ID3D11Resource*> images_;
    ID3D11Device* renderer_ = nullptr;
    std::atomic<uint64_t> swapchainCopies_{0};
    std::atomic<bool> active_{false};
    std::atomic<unsigned long long> frame_{0};
    std::atomic<unsigned> otherDraws_{0};
    unsigned draws_ = 0, totalDraws_ = 0, binds_ = 0, execs_ = 0, lines_ = 0, traced_ = 0;
    unsigned long long lastTraced_ = 0;
    bool haveKey_ = false;
    ID3D11Resource* keyRtv_ = nullptr;
    ID3D11DepthStencilView* keyDsv_ = nullptr;
    float keyW_ = 0, keyH_ = 0;
};

}  // namespace simqvfr::games::rf2
