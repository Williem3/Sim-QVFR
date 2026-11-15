// The game's large constant buffers mirrored with their pairs rewritten, and one draw's constant range rewritten for
// the corner or the focus view, on the GPU (corner_rules.h; corner_internal.h).
#include "corner_internal.h"

#include <algorithm>
#include <cstring>

#include "../lmu/mvr_probe.h"
#include "../../log.h"

namespace scs {
namespace detail {

Mirror* mirror_of_game(ID3D11Buffer* b) {
    for (Mirror& m : g.mirrors)
        if (m.game.Get() == b)
            return &m;
    return nullptr;
}

// The game's buffer behind one of our copies (or `b` itself).
ID3D11Buffer* game_of(ID3D11Buffer* b) {
    for (Mirror& m : g.mirrors)
        if (m.copy.Get() == b)
            return m.game.Get();
    return b;
}

bool big_scratch(UINT bytes) {
    if (bytes <= g.bigBytes)
        return true;
    g.bigIn.Reset(), g.bigOut.Reset(), g.bigInView.Reset(), g.bigOutView.Reset();
    const D3D11_BUFFER_DESC in = {bytes, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE, 0,
                                  D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS, 0};
    D3D11_BUFFER_DESC out = in;
    out.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
    D3D11_SHADER_RESOURCE_VIEW_DESC sv{};
    sv.Format = DXGI_FORMAT_R32_TYPELESS;
    sv.ViewDimension = D3D11_SRV_DIMENSION_BUFFEREX;
    sv.BufferEx = {0, bytes / 4, D3D11_BUFFEREX_SRV_FLAG_RAW};
    D3D11_UNORDERED_ACCESS_VIEW_DESC uv{};
    uv.Format = DXGI_FORMAT_R32_TYPELESS;
    uv.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
    uv.Buffer = {0, bytes / 4, D3D11_BUFFER_UAV_FLAG_RAW};
    if (FAILED(g.device->CreateBuffer(&in, nullptr, &g.bigIn)) || FAILED(g.device->CreateBuffer(&out, nullptr, &g.bigOut)) ||
        FAILED(g.device->CreateShaderResourceView(g.bigIn.Get(), &sv, &g.bigInView)) ||
        FAILED(g.device->CreateUnorderedAccessView(g.bigOut.Get(), &uv, &g.bigOutView)))
        return false;
    g.bigBytes = bytes;
    return true;
}

// Runs `cs` from `in` to `out` with `params` (64 bytes), the game's compute state kept and put back.
void run_cs(ID3D11ComputeShader* cs, ID3D11ShaderResourceView* in, ID3D11UnorderedAccessView* out, const void* params,
            UINT groups) {
    ComPtr<ID3D11ComputeShader> gameCs;
    ComPtr<ID3D11ShaderResourceView> srv;
    ComPtr<ID3D11UnorderedAccessView> uav;
    ComPtr<ID3D11Buffer> cb;
    g.context->CSGetShader(&gameCs, nullptr, nullptr);
    g.context->CSGetShaderResources(0, 1, &srv);
    g.context->CSGetUnorderedAccessViews(0, 1, &uav);
    g.context->CSGetConstantBuffers(0, 1, &cb);
    g.context->UpdateSubresource(g.params.Get(), 0, nullptr, params, 0, 0);
    ID3D11ShaderResourceView* const ins[] = {in};
    ID3D11UnorderedAccessView* const outs[] = {out};
    ID3D11Buffer* const ps[] = {g.params.Get()};
    g.context->CSSetShader(cs, nullptr, 0);
    g.context->CSSetShaderResources(0, 1, ins);
    g.context->CSSetUnorderedAccessViews(0, 1, outs, nullptr);
    g.context->CSSetConstantBuffers(0, 1, ps);
    g.context->Dispatch(groups, 1, 1);
    ID3D11ShaderResourceView* const s0[] = {srv.Get()};
    ID3D11UnorderedAccessView* const u0[] = {uav.Get()};
    ID3D11Buffer* const c0[] = {cb.Get()};
    g.context->CSSetShader(gameCs.Get(), nullptr, 0);
    g.context->CSSetShaderResources(0, 1, s0);
    g.context->CSSetUnorderedAccessViews(0, 1, u0, nullptr);
    g.context->CSSetConstantBuffers(0, 1, c0);
}

// This frame's copy of a mirrored buffer: the game's data copied and every eye-shaped pair rewritten. Copied again
// after the game writes it: on the drawing thread (on_unmap clears refreshedFrame) or on another (its generation).
void refresh(Mirror& m) {
    const uint32_t gen = m.id >= 0 ? g.mirrorGen[m.id].load(std::memory_order_acquire) : 0;
    if ((m.refreshedFrame == g.stats.frames && gen == m.seenGen) || !big_scratch(m.bytes))
        return;
    m.refreshedFrame = g.stats.frames;
    m.seenGen = gen;
    const Timed timed(State::kTimeMirrors);
    const D3D11_BOX box = {0, 0, 0, m.bytes, 1, 1};
    g.context->CopySubresourceRegion(g.bigIn.Get(), 0, 0, 0, 0, m.game.Get(), 0, &box);
    const struct {
        float fx, fy;
        UINT count, pad;
        float w, h, p0, p1;
        float unused[8];
    } p = {g.fx, g.fy, m.bytes / 16, 0, static_cast<float>(g.eyeW), static_cast<float>(g.eyeH), 0, 0, {}};
    static_assert(sizeof(p) == 64, "run_cs's params are 64 bytes");
    run_cs(g.mirrorCs.Get(), g.bigInView.Get(), g.bigOutView.Get(), &p, (m.bytes / 16 + 63) / 64);
    g.context->CopySubresourceRegion(m.copy.Get(), 0, 0, 0, 0, g.bigOut.Get(), 0, &box);
    ++g.mirrorRefreshes;
}

// A large constant buffer seen for the first time: our copy made.
Mirror* learn(ID3D11Buffer* b) {
    if (!b)
        return nullptr;
    if (Mirror* m = mirror_of_game(b))
        return m;
    if (game_of(b) != b)
        return nullptr;  // one of ours
    D3D11_BUFFER_DESC d;
    b->GetDesc(&d);
    if (d.ByteWidth < kMirrorMin || !(d.BindFlags & D3D11_BIND_CONSTANT_BUFFER))
        return nullptr;
    Mirror m;
    m.game = b;
    m.bytes = d.ByteWidth & ~15u;
    const D3D11_BUFFER_DESC cd = {m.bytes, D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0};
    if (FAILED(g.device->CreateBuffer(&cd, nullptr, &m.copy))) {
        simqvfr::logf("scs corner: a copy of the game's %u-byte constant buffer couldn't be made", d.ByteWidth);
        return nullptr;
    }
    if (const int n = g.mirrorIdCount.load(std::memory_order_relaxed); n < State::kMirrorIds) {
        m.id = n;
        g.mirrorGen[n].store(0, std::memory_order_relaxed);
        g.mirrorIds[n].store(b, std::memory_order_relaxed);
        g.mirrorIdCount.store(n + 1, std::memory_order_release);
    }
    g.mirrors.push_back(m);
    simqvfr::logf("scs corner: the game's constant buffer %p (%u bytes, usage %u) mirrored (%zu so far)",
                  static_cast<void*>(b), d.ByteWidth, static_cast<unsigned>(d.Usage), g.mirrors.size());
    return &g.mirrors.back();
}

// The vertex and pixel stages' bound constant buffers swapped to our copies (on) or back to the game's (off), ranges kept.
void swap_bound(bool on) {
    constexpr UINT n = D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT;
    for (int stage = 0; stage < 2; ++stage) {
        ID3D11Buffer* bufs[n] = {};
        UINT first[n] = {}, num[n] = {};
        if (stage)
            g.context->PSGetConstantBuffers1(0, n, bufs, first, num);
        else
            g.context->VSGetConstantBuffers1(0, n, bufs, first, num);
        bool changed = false;
        ID3D11Buffer* next[n];
        for (UINT i = 0; i < n; ++i) {
            next[i] = bufs[i];
            if (!bufs[i])
                continue;
            if (on) {
                if (Mirror* m = learn(bufs[i])) {
                    refresh(*m);
                    next[i] = m->copy.Get();
                }
            } else {
                next[i] = game_of(bufs[i]);
            }
            changed |= next[i] != bufs[i];
        }
        if (changed) {
            if (stage)
                g.context->PSSetConstantBuffers1(0, n, next, first, num);
            else
                g.context->VSSetConstantBuffers1(0, n, next, first, num);
            ++g.mirrorSwaps;
        }
        for (ID3D11Buffer* b : bufs)
            if (b)
                b->Release();
    }
}

// The game wrote a buffer: a mirrored one is copied again at its next bind (its pool's buffers can be filled mid-frame,
// after this frame's copy was made: the stale copy flickered on the cab roof, S1 run 2). From another thread: through
// the buffer's generation (refresh), since the mirror list is the drawing thread's.
void on_unmap(ID3D11DeviceContext*, ID3D11Resource* resource, UINT) {
    if (const DWORD self = GetCurrentThreadId(); self != g.thread) {
        const int n = g.mirrorIdCount.load(std::memory_order_acquire);
        for (int i = 0; i < n; ++i)
            if (g.mirrorIds[i].load(std::memory_order_relaxed) == resource) {
                g.mirrorGen[i].fetch_add(1, std::memory_order_release);
                g.otherWrites.fetch_add(1, std::memory_order_relaxed);
                if (g.activeNow.load(std::memory_order_relaxed))
                    g.otherWritesActive.fetch_add(1, std::memory_order_relaxed);
                g.otherWriter.store(self, std::memory_order_relaxed);
                break;
            }
        return;
    }
    for (Mirror& m : g.mirrors)
        if (static_cast<ID3D11Resource*>(m.game.Get()) == resource) {
            if (g.active && m.refreshedFrame == g.stats.frames)
                ++g.midFrameWrites;
            m.refreshedFrame = ~0ull;
            return;
        }
}

bool on_constant_buffers(ID3D11DeviceContext*, bool, UINT, UINT count, ID3D11Buffer* const* in, ID3D11Buffer** out) {
    if (!g.active || g.mirrorsOff || GetCurrentThreadId() != g.thread)
        return false;
    bool changed = false;
    for (UINT i = 0; i < count; ++i) {
        out[i] = in[i];
        if (Mirror* m = learn(in[i])) {
            refresh(*m);
            out[i] = m->copy.Get();
            changed = true;
        }
    }
    return changed;
}

// The game's slot-0 range of one stage rewritten into `ours` (the GPU's copy) and bound in its place for this draw
// (put_back undoes it). False: left as the game bound it.
bool rewrite(Range& r, ID3D11Buffer* ours, bool pixel, bool focus) {
    ID3D11Buffer* buf = nullptr;
    UINT first = 0, count = 0;
    if (pixel)
        g.context->PSGetConstantBuffers1(0, 1, &buf, &first, &count);
    else
        g.context->VSGetConstantBuffers1(0, 1, &buf, &first, &count);
    r.game.Attach(buf);
    r.first = first;
    r.count = count;
    r.ours = false;
    if (!buf)
        return false;
    D3D11_BUFFER_DESC d;
    buf->GetDesc(&d);
    const UINT total = d.ByteWidth / 16;
    if (count == 0) {  // a plain bind: the whole buffer
        first = 0, count = total;
    } else if (first + count > total) {  // a range bound past the buffer's end (it reads zeros there): what is in it
        if (first >= total) {
            r = Range{};
            return false;
        }
        count = total - first;
    }
    count = std::min(count, kMaxVectors);
    if (!count)
        return false;
    const D3D11_BOX box = {first * 16, 0, 0, (first + count) * 16, 1, 1};
    g.context->CopySubresourceRegion(g.rawIn.Get(), 0, 0, 0, 0, game_of(buf), 0, &box);  // the game's own data
    // The rules read only what the shader declares: a range is bound in blocks of 16 vectors, and past the shader's own
    // lie the pooled buffer's leftovers (S5 run 4: a leftover matrix pair there had the focus rule rewrite the depth of
    // field's weights: far terrain black for single frames).
    UINT declared = count;
    {
        ComPtr<ID3D11DeviceChild> shader;
        if (pixel) {
            ID3D11PixelShader* s = nullptr;
            g.context->PSGetShader(&s, nullptr, nullptr);
            shader.Attach(s);
        } else {
            ID3D11VertexShader* s = nullptr;
            g.context->VSGetShader(&s, nullptr, nullptr);
            shader.Attach(s);
        }
        UINT vectors = 0, size = sizeof(vectors);
        if (shader && SUCCEEDED(shader->GetPrivateData(lmu::kCb0VectorsGuid, &size, &vectors))) {
            if (!vectors) {  // the shader reads no constants from slot 0: nothing of its to rewrite
                r = Range{};
                return false;
            }
            declared = std::min(count, vectors);
        } else {
            ++g.undeclared;
        }
    }
    // The focus view: the middle focusPercent of the eye's screen on each axis; its clip is the eye's scaled about the
    // centre (the remap the scene draws' twins use).
    const int eye = current_eye();
    struct {
        float fx, fy;
        UINT count, flags;
        float box[4], remap[4];
        float px, py, pad[2];
    } p = {focus ? twin_fx() : g.fx, focus ? twin_fy() : g.fy, declared, (pixel ? 0u : 1u) | (focus ? 2u : 0u),
           {g.box[eye][0], g.box[eye][1], beta_x(), beta_y()}, {1, 0, 1, 0}, focus ? focus_fx() : g.fx,
           focus ? focus_fy() : g.fy, {}};
    static_assert(sizeof(p) == 64, "run_cs's params are 64 bytes");
    if (focus)  // the reprojections: into the frame before's box (Ctrl+Alt+H: this frame's)
        box_remap(g.historyCurrentBox ? g.box[eye] : g.prevBox[eye], p.remap);
    run_cs(g.patchCs.Get(), g.rawInView.Get(), g.rawOutView.Get(), &p, 1);
    const D3D11_BOX outBox = {0, 0, 0, count * 16, 1, 1};
    g.context->CopySubresourceRegion(ours, 0, 0, 0, 0, g.rawOut.Get(), 0, &outBox);
    const UINT zero = 0, n = ((count + 15) / 16) * 16;  // ranged binds: multiples of 16 constants
    if (pixel)
        g.context->PSSetConstantBuffers1(0, 1, &ours, &zero, &n);
    else
        g.context->VSSetConstantBuffers1(0, 1, &ours, &zero, &n);
    r.ours = true;
    return true;
}

void put_back(Range& r, bool pixel) {
    if (r.ours) {
        ID3D11Buffer* const b[] = {r.game.Get()};
        if (r.count)
            pixel ? g.context->PSSetConstantBuffers1(0, 1, b, &r.first, &r.count)
                  : g.context->VSSetConstantBuffers1(0, 1, b, &r.first, &r.count);
        else
            pixel ? g.context->PSSetConstantBuffers(0, 1, b) : g.context->VSSetConstantBuffers(0, 1, b);
    }
    r = Range{};
}

}  // namespace detail

}  // namespace scs
