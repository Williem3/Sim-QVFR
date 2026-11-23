// The game's large constant buffers mirrored with their pairs rewritten, and one draw's constant range rewritten for
// the corner or the focus view, on the GPU (corner_rules.h; corner_internal.h).
#include "corner_internal.h"

#include <algorithm>
#include <cstdio>
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

// Runs `cs` from `in` to `out` with `params` (64 bytes), the game's compute state kept and put back (b0 with its range).
void run_cs(ID3D11ComputeShader* cs, ID3D11ShaderResourceView* in, ID3D11UnorderedAccessView* out, const void* params,
            UINT groups, const void* rows, ID3D11UnorderedAccessView* log) {
    d3s::Spec spec;
    spec.parts = d3s::kCS;
    spec.csSrv = {0, 1};
    spec.csUavs = {0, log ? 2u : 1u};
    spec.csCb = {0, rows ? 2u : 1u};
    spec.instances = false;
    const simqvfr::D3D11StateGuard keep(g.context.Get(), spec);
    g.context->UpdateSubresource(g.params.Get(), 0, nullptr, params, 0, 0);
    ID3D11ShaderResourceView* const ins[] = {in};
    ID3D11UnorderedAccessView* const outs[] = {out};
    ID3D11Buffer* const ps[] = {g.params.Get()};
    g.context->CSSetShader(cs, nullptr, 0);
    g.context->CSSetShaderResources(0, 1, ins);
    g.context->CSSetUnorderedAccessViews(0, 1, outs, nullptr);
    if (log) {  // u1: the mirror pass's anomaly log (corner_anomaly.cpp)
        ID3D11UnorderedAccessView* const logs[] = {log};
        g.context->CSSetUnorderedAccessViews(1, 1, logs, nullptr);
    }
    g.context->CSSetConstantBuffers(0, 1, ps);
    if (rows) {  // b1: the shader's rows read by a fixed index (kCornerPatchCs)
        g.context->UpdateSubresource(g.rowsCb.Get(), 0, nullptr, rows, 0, 0);
        ID3D11Buffer* const rb[] = {g.rowsCb.Get()};
        g.context->CSSetConstantBuffers(1, 1, rb);
    }
    g.context->Dispatch(groups, 1, 1);
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
    struct {  // (pad: the anomaly log's tag)
        float fx, fy;
        UINT count, pad;
        float w, h, p0, p1;
        float unused[8];
    } p = {g.fx, g.fy, m.bytes / 16, 0, static_cast<float>(g.eyeW), static_cast<float>(g.eyeH), 0, 0, {}};
    static_assert(sizeof(p) == 64, "run_cs's params are 64 bytes");
    ++m.serial;  // this copy's (the anomaly log's tag carries it, and the draws that read it)
    ID3D11UnorderedAccessView* const log = anomaly_uav();
    if (log)
        p.pad = anomaly_tag(m);
    run_cs(g.mirrorCs.Get(), g.bigInView.Get(), g.bigOutView.Get(), &p, (m.bytes / 16 + 63) / 64, nullptr, log);
    g.context->CopySubresourceRegion(m.copy.Get(), 0, 0, 0, 0, g.bigOut.Get(), 0, &box);
    mirror_diag_refresh(m);
    ++g.mirrorRefreshes;
}

// A large constant buffer seen for the first time: our copy made.
Mirror* learn(ID3D11Buffer* b) {
    if (!b)
        return nullptr;
    if (Mirror* m = mirror_of_game(b)) {
        m->usedFrame = g.stats.frames;
        return m;
    }
    if (game_of(b) != b)
        return nullptr;  // one of ours
    D3D11_BUFFER_DESC d;
    b->GetDesc(&d);
    if (d.ByteWidth < kMirrorMin || !(d.BindFlags & D3D11_BIND_CONSTANT_BUFFER))
        return nullptr;
    // A slot for other threads' writes: a freed one (evict_mirrors) or the next. None: not mirrored (the game's own
    // buffer stays bound, its pairs not rewritten for the corner) rather than a copy other threads' writes miss.
    const int n = g.mirrorIdCount.load(std::memory_order_relaxed);
    int id = -1;
    for (int i = 0; i < n && id < 0; ++i)
        if (!g.mirrorIds[i].load(std::memory_order_relaxed))
            id = i;
    if (id < 0 && n < State::kMirrorIds)
        id = n;
    if (id < 0) {
        ++g.mirrorNoId;
        if (!g.mirrorNoIdLogged) {
            g.mirrorNoIdLogged = true;
            simqvfr::logf("scs corner: the game's constant buffer %p (%u bytes) not mirrored: all %d mirrors in use (it "
                          "stays the game's own until one is let go)",
                          static_cast<void*>(b), d.ByteWidth, State::kMirrorIds);
        }
        return nullptr;
    }
    Mirror m;
    m.game = b;
    m.bytes = d.ByteWidth & ~15u;
    m.usedFrame = g.stats.frames;
    const D3D11_BUFFER_DESC cd = {m.bytes, D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0};
    if (FAILED(g.device->CreateBuffer(&cd, nullptr, &m.copy))) {
        simqvfr::logf("scs corner: a copy of the game's %u-byte constant buffer couldn't be made", d.ByteWidth);
        return nullptr;
    }
    m.id = id;  // published before the first copy (refresh), so no other thread's write after it goes unseen
    g.mirrorGen[id].store(0, std::memory_order_relaxed);
    g.mirrorIds[id].store(b, std::memory_order_release);
    if (id == n)
        g.mirrorIdCount.store(n + 1, std::memory_order_release);
    g.mirrors.push_back(m);
    simqvfr::logf("scs corner: the game's constant buffer %p (%u bytes, usage %u) mirrored (%zu so far)",
                  static_cast<void*>(b), d.ByteWidth, static_cast<unsigned>(d.Usage), g.mirrors.size());
    return &g.mirrors.back();
}

// Mirrors no bind has used for kEvictFrames frames, let go with their slot (as evict_unused does twins): the game
// makes new buffers at level loads, and each mirror holds the game's old one alive with our copy (256 KB and more
// each). At the frame's end, outside the corner (our copies swapped back out); one still bound anyway is kept, so
// game_of can still map it back.
// `orphans`: only those whose game buffer the game has let go (ours the last reference), whenever they were used: every
// frame (release_orphans), so the slots come back as soon as the game remakes its buffers, not 900 frames later.
void evict_mirrors(bool orphans) {
    if (g.active || g.mirrors.empty())
        return;
    if (orphans && std::none_of(g.mirrors.begin(), g.mirrors.end(), [](const Mirror& m) { return only_ours(m.game.Get()); }))
        return;  // the usual frame: nothing to look up in the context
    constexpr UINT n = D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT;
    ID3D11Buffer* bound[2][n] = {};
    g.context->VSGetConstantBuffers(0, n, bound[0]);
    g.context->PSGetConstantBuffers(0, n, bound[1]);
    const auto held = [&](const Mirror& m) {
        for (const auto& stage : bound)
            for (ID3D11Buffer* b : stage)
                if (b && b == m.copy.Get())
                    return true;
        return false;
    };
    const uint64_t now = g.stats.frames;
    size_t gone = 0;
    for (auto it = g.mirrors.begin(); it != g.mirrors.end();)
        if ((orphans ? only_ours(it->game.Get()) : now - it->usedFrame > kEvictFrames) && !held(*it)) {
            if (it->id >= 0)  // first: other threads stop matching it (before the game's buffer, and its address, go)
                g.mirrorIds[it->id].store(nullptr, std::memory_order_release);
            it = g.mirrors.erase(it);
            ++gone;
        } else {
            ++it;
        }
    for (const auto& stage : bound)
        for (ID3D11Buffer* b : stage)
            if (b)
                b->Release();
    if (gone) {
        g.mirrorsEvicted += gone;
        g.mirrorNoIdLogged = false;  // a buffer left unmirrored is logged again if it still finds no slot
        char why[64];
        if (orphans)
            snprintf(why, sizeof(why), "whose game buffers are gone");
        else
            snprintf(why, sizeof(why), "unused for %llu frames", static_cast<unsigned long long>(kEvictFrames));
        simqvfr::logf("scs corner: %zu mirrors %s let go (%zu left; %llu let go, %llu binds left unmirrored for want of a "
                      "slot so far)",
                      gone, why, g.mirrors.size(), static_cast<unsigned long long>(g.mirrorsEvicted),
                      static_cast<unsigned long long>(g.mirrorNoId));
    }
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
    if (session_ending())
        return;
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

// Our copies only for draws into eye-shaped targets (the corner's scale is theirs), not into anything else: the UI's own
// 2880x1800 texture (its white elements turned violet: only they, not the panel's map, so there and not at its
// composite onto the headset image), shadow maps.
// The copies rewrite every eye-shaped pair by value, so a white colour (1,1,1,1) followed by such a size became the
// corner's (0.4, 0.4, 2.5, 2.5): the one-frame violet UI and ground of 2026-11-18.
void mirrors_for_target() {
    const bool want = g.active && !g.mirrorsOff && g.cut;
    if (want == g.mirrorsBound)
        return;
    g.mirrorsBound = want;
    swap_bound(want);
    ++g.mirrorTargetSwaps;
}

bool on_constant_buffers(ID3D11DeviceContext*, bool, UINT, UINT count, ID3D11Buffer* const* in, ID3D11Buffer** out) {
    if (session_ending() || !g.active || !g.mirrorsBound || g.mirrorsOff || GetCurrentThreadId() != g.thread)
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
bool rewrite(Range& r, bool pixel, bool focus, const int* position, bool tinted) {
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
    r.position = nullptr;
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
    if (g.rewriteSets.empty()) {
        r = Range{};
        return false;
    }
    const State::RewriteSet& set = g.rewriteSets[g.nextRewriteSet++ % g.rewriteSets.size()];  // a set no draw in flight reads
    const D3D11_BOX box = {first * 16, 0, 0, (first + count) * 16, 1, 1};
    g.context->CopySubresourceRegion(set.in.Get(), 0, 0, 0, 0, game_of(buf), 0, &box);  // the game's own data
    // The rules read only what the shader declares: a range is bound in blocks of 16 vectors, and past the shader's own
    // lie the pooled buffer's leftovers (S5 run 4: a leftover matrix pair there had the focus rule rewrite the depth of
    // field's weights: far terrain black for single frames).
    UINT declared = count;
    uint64_t rows[64];
    bool haveRows = false;  // the shader's rows read by a fixed index (lmu::kCb0StaticRowsGuid): the focus matrix rule's limit
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
        if (shader && pixel && focus) {
            UINT rowsSize = sizeof(rows);
            haveRows = SUCCEEDED(shader->GetPrivateData(lmu::kCb0StaticRowsGuid, &rowsSize, rows)) && rowsSize == sizeof(rows);
        }
        if (tinted) {  // the rows it reads by a fixed index, less its colour rows
            uint64_t tint[2][64];
            UINT tintSize = sizeof(tint);
            if (!shader || FAILED(shader->GetPrivateData(lmu::kCb0TintRowsGuid, &tintSize, tint)) || tintSize != sizeof(tint)) {
                r = Range{};
                return false;
            }
            for (int k = 0; k < 64; ++k)
                rows[k] = tint[0][k] & ~tint[1][k];
            haveRows = true;
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
        float px, py;
        uint32_t rectRow, sizeRow;
    } p = {focus ? twin_fx() : g.fx, focus ? twin_fy() : g.fy, declared,
           (pixel || tinted ? 0u : 1u) | (focus ? 2u : 0u) | (haveRows ? 4u : 0u),
           {g.box[eye][0], g.box[eye][1], beta_x(), beta_y()}, {1, 0, 1, 0}, focus ? focus_fx() : g.fx,
           focus ? focus_fy() : g.fy, ~0u, ~0u};
    const bool positioned = position && !pixel && count <= kPositionAt;  // the eye's pass: its size row alone
    if (positioned) {
        p.flags |= 8u;
        p.rectRow = position[0] >= 0 ? static_cast<uint32_t>(position[0]) : ~0u;
        p.sizeRow = position[1] >= 0 ? static_cast<uint32_t>(position[1]) : ~0u;
    }
    static_assert(sizeof(p) == 64, "run_cs's params are 64 bytes");
    if (focus)  // the reprojections: into the frame before's box (Ctrl+Alt+H: this frame's)
        box_remap(g.historyCurrentBox ? g.box[eye] : g.prevBox[eye], p.remap);
    run_cs(g.patchCs.Get(), set.inView.Get(), set.outView.Get(), &p, 1, haveRows ? rows : nullptr);
    const D3D11_BOX outBox = {0, 0, 0, count * 16, 1, 1};
    ID3D11Buffer* ours = set.cb.Get();
    g.context->CopySubresourceRegion(ours, 0, 0, 0, 0, set.out.Get(), 0, &outBox);
    if (positioned) {  // the twin's two vectors, bound as their own 16-constant block
        const D3D11_BOX at = {kPositionAt * 16, 0, 0, (kPositionAt + 16) * 16, 1, 1};
        g.context->CopySubresourceRegion(ours, 0, kPositionAt * 16, 0, 0, set.out.Get(), 0, &at);
        if (focus)
            r.position = ours;
    }
    const UINT zero = 0, n = ((count + 15) / 16) * 16;  // ranged binds: multiples of 16 constants
    if (pixel)
        g.context->PSSetConstantBuffers1(0, 1, &ours, &zero, &n);
    else
        g.context->VSSetConstantBuffers1(0, 1, &ours, &zero, &n);
    r.ours = true;
    return true;
}

// A scene draw while our copies are bound: a stage whose shader reads a constant row as a colour gets its range rewritten
// from the game's own data with those rows left alone (rewrite, tinted); put back after the draw with the rest.
void tint_draw() {
    if (!g.tintRows || !g.mirrorsBound || g.vs.game || g.ps.game)
        return;
    const auto tinted = [](ID3D11DeviceChild* s) {
        UINT size = 0;
        return s && s->GetPrivateData(lmu::kCb0TintRowsGuid, &size, nullptr) != DXGI_ERROR_NOT_FOUND && size;
    };
    bool any = false;
    {
        ID3D11VertexShader* vs = nullptr;
        g.context->VSGetShader(&vs, nullptr, nullptr);
        ComPtr<ID3D11VertexShader> keep;
        keep.Attach(vs);
        if (tinted(vs))
            any |= rewrite(g.vs, false, false, nullptr, true);
    }
    {
        ID3D11PixelShader* ps = nullptr;
        g.context->PSGetShader(&ps, nullptr, nullptr);
        ComPtr<ID3D11PixelShader> keep;
        keep.Attach(ps);
        if (tinted(ps))
            any |= rewrite(g.ps, true, false, nullptr, true);
    }
    g.tintDraws += any ? 1 : 0;
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
