// Part of mvr_pass.cpp's translation unit (included once, in order): the hooks' table (which entries, the functions
// each holds, call_real), the real calls past the hooks, the sampled section timer, the held game objects. Included
// only by mvr_pass.cpp, inside its anonymous namespace (lmu::<anonymous>), after the state definitions; never include
// it anywhere else: the per-draw path stays in one translation unit (no whole-program optimization). No includes of its
// own: mvr_pass.cpp's.
#pragma once

// How the hooks are reached (mvr_pass.h context_slot): d3d11 rewrites the context's vtable entries as it runs, and a
// hook on one function an entry held misses the calls through the others (nvapi_stereo_check 6: once protection has
// been on, the plain functions are other ones, which the locking ones don't call), so each function an entry is seen
// holding is hooked in d3d11's code (MinHook), a detour of its own each: those found at install (the entries as they
// are, with multithread protection on if it was off, each after a flush and after a command) and any new one at a
// pass's start. Our own calls and the calls passed on go to the original of the function the entry holds now.
enum Hook : int {
    kHookVS, kHookGS, kHookOM, kHookOMUav, kHookSO, kHookVP, kHookSC, kHookResolve, kHookHS, kHookDS, kHookDrawIndexed,
    kHookDraw, kHookDrawIndexedInstanced, kHookDrawInstanced, kHookCopy, kHookDrawAuto, kHookDrawIndexedIndirect,
    kHookDrawInstancedIndirect, kHookPS, kHookVSCB, kHookMap, kHookUnmap, kHookIAVB, kHookIAIB, kHookBlend, kHookDepth,
    kHookExecute, kHookVPGet, kHooks
};
constexpr unsigned kSlotOf[kHooks] = {kVSSetShader,   kGSSetShader,    kOMSetRenderTargets, kOMSetRenderTargetsAndUavs,
                                      kSOSetTargets,  kRSSetViewports, kRSSetScissorRects,  kResolveSubresource,
                                      kHSSetShader,   kDSSetShader,    kDrawIndexed,        kDraw,
                                      kDrawIndexedInstanced,           kDrawInstanced,      kCopySubresourceRegion,
                                      kDrawAuto,      kDrawIndexedInstancedIndirect,        kDrawInstancedIndirect,
                                      kPSSetShader,   kVSSetConstantBuffers, kMap, kUnmap,  kIASetVertexBuffers,
                                      kIASetIndexBuffer, kOMSetBlendState, kOMSetDepthStencilState, kExecuteCommandList,
                                      kRSGetViewports};
constexpr const char* kHookName[kHooks] = {"VSSetShader",    "GSSetShader",       "OMSetRenderTargets",
                                           "OMSetRenderTargetsAndUnorderedAccessViews", "SOSetTargets",
                                           "RSSetViewports", "RSSetScissorRects", "ResolveSubresource",
                                           "HSSetShader",    "DSSetShader",       "DrawIndexed",
                                           "Draw",           "DrawIndexedInstanced", "DrawInstanced",
                                           "CopySubresourceRegion", "DrawAuto", "DrawIndexedInstancedIndirect",
                                           "DrawInstancedIndirect", "PSSetShader", "VSSetConstantBuffers", "Map",
                                           "Unmap", "IASetVertexBuffers", "IASetIndexBuffer", "OMSetBlendState",
                                           "OMSetDepthStencilState", "ExecuteCommandList", "RSGetViewports"};
// Car tracking's hooks (mvr_car_track).
constexpr bool is_car_hook(int h) {
    return h == kHookVSCB || h == kHookMap || h == kHookUnmap || h == kHookIAVB || h == kHookIAIB;
}
// Car slots' (mvr_car_slots).
constexpr bool is_slot_hook(int h) { return h == kHookBlend || h == kHookDepth; }
// The draws: hooked only with the focus post (DrawIndexed: its copy and composite; all four: the overlay trace) or
// with instancing (they're doubled).
constexpr bool is_draw(int h) {
    return h == kHookDrawIndexed || h == kHookDraw || h == kHookDrawIndexedInstanced || h == kHookDrawInstanced;
}
// The draws instancing can't double (no instance count to set): only with it.
constexpr bool is_undoubled_draw(int h) {
    return h == kHookDrawAuto || h == kHookDrawIndexedIndirect || h == kHookDrawInstancedIndirect;
}
// Per entry: 4 seen for each on WARP and on an RTX 5070 Ti (2026-11-09: idle and busy, multithread protection off, on,
// and off again after it was on), room for more.
constexpr int kFunctions = 8;
struct Functions {
    void* target[kFunctions] = {};  // d3d11's
    void* real[kFunctions] = {};    // its original (MinHook's trampoline; the function itself if it couldn't be hooked)
    int count = 0, hooked = 0;
    bool full = false;  // a fifth seen (logged once)
};
Functions functions[kHooks];
// Calls of a hooked function inside the handling of one: ours, the one passed on, and one function of an entry calling
// another (a locking one calling the plain one) go straight through.
thread_local int depth[kHooks] = {};

template <int H, typename... A>
void call_real(ID3D11DeviceContext* context, A... a) {
    void* fn = (*reinterpret_cast<void** const*>(context))[kSlotOf[H]];
    void* real = fn;  // a function not hooked (yet): d3d11's own
    const Functions& f = functions[H];
    for (int i = 0; i < f.count; ++i)
        if (f.target[i] == fn) {
            real = f.real[i];
            break;
        }
    ++depth[H];
    reinterpret_cast<void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, A...)>(real)(context, a...);
    --depth[H];
}

void realVSSetShader(ID3D11DeviceContext* c, ID3D11VertexShader* vs, ID3D11ClassInstance* const* i, UINT n) {
    call_real<kHookVS>(c, vs, i, n);
}
void realPSSetShader(ID3D11DeviceContext* c, ID3D11PixelShader* ps, ID3D11ClassInstance* const* i, UINT n) {
    call_real<kHookPS>(c, ps, i, n);
}
void realOMSetRenderTargets(ID3D11DeviceContext* c, UINT n, ID3D11RenderTargetView* const* rtvs,
                            ID3D11DepthStencilView* dsv) {
    call_real<kHookOM>(c, n, rtvs, dsv);
}
void realOMSetRenderTargetsAndUavs(ID3D11DeviceContext* c, UINT n, ID3D11RenderTargetView* const* rtvs,
                                   ID3D11DepthStencilView* dsv, UINT uavStart, UINT uavCount,
                                   ID3D11UnorderedAccessView* const* uavs, const UINT* counts) {
    call_real<kHookOMUav>(c, n, rtvs, dsv, uavStart, uavCount, uavs, counts);
}
void realRSSetViewports(ID3D11DeviceContext* c, UINT n, const D3D11_VIEWPORT* vps) { call_real<kHookVP>(c, n, vps); }
void realRSSetScissorRects(ID3D11DeviceContext* c, UINT n, const D3D11_RECT* rects) { call_real<kHookSC>(c, n, rects); }
void realSOSetTargets(ID3D11DeviceContext* c, UINT n, ID3D11Buffer* const* buffers, const UINT* offsets) {
    call_real<kHookSO>(c, n, buffers, offsets);
}
// Our own VS buffer binds (the remap at b13) past the hook, so they aren't recorded as the game's.
void realVSSetConstantBuffers(ID3D11DeviceContext* c, UINT start, UINT n, ID3D11Buffer* const* buffers) {
    call_real<kHookVSCB>(c, start, n, buffers);
}
void realResolveSubresource(ID3D11DeviceContext* c, ID3D11Resource* dst, UINT dstSub, ID3D11Resource* src, UINT srcSub,
                            DXGI_FORMAT format) {
    call_real<kHookResolve>(c, dst, dstSub, src, srcSub, format);
}
// A state guard's setters past the hooks (simqvfr::d3d11_state::Guard<RealCalls>): every setter hooked here goes to
// the original, so a restore isn't seen as the game's; the rest (VSSetConstantBuffers1 too: not hooked) as is.
struct RealCalls : simqvfr::d3d11_state::DirectCalls {
    static void IASetVertexBuffers(C* c, UINT s, UINT n, ID3D11Buffer* const* b, const UINT* st, const UINT* o) {
        call_real<kHookIAVB>(c, s, n, b, st, o);
    }
    static void IASetIndexBuffer(C* c, ID3D11Buffer* b, DXGI_FORMAT f, UINT o) { call_real<kHookIAIB>(c, b, f, o); }
    static void VSSetShader(C* c, ID3D11VertexShader* s, ID3D11ClassInstance* const* i, UINT n) { realVSSetShader(c, s, i, n); }
    static void PSSetShader(C* c, ID3D11PixelShader* s, ID3D11ClassInstance* const* i, UINT n) { realPSSetShader(c, s, i, n); }
    static void GSSetShader(C* c, ID3D11GeometryShader* s) {
        call_real<kHookGS>(c, s, static_cast<ID3D11ClassInstance* const*>(nullptr), 0u);
    }
    static void HSSetShader(C* c, ID3D11HullShader* s) {
        call_real<kHookHS>(c, s, static_cast<ID3D11ClassInstance* const*>(nullptr), 0u);
    }
    static void DSSetShader(C* c, ID3D11DomainShader* s) {
        call_real<kHookDS>(c, s, static_cast<ID3D11ClassInstance* const*>(nullptr), 0u);
    }
    static void VSSetConstantBuffers(C* c, UINT s, UINT n, ID3D11Buffer* const* b) { realVSSetConstantBuffers(c, s, n, b); }
    static void OMSetRenderTargets(C* c, UINT n, ID3D11RenderTargetView* const* r, ID3D11DepthStencilView* d) {
        realOMSetRenderTargets(c, n, r, d);
    }
    static void OMSetRenderTargetsAndUnorderedAccessViews(C* c, UINT n, ID3D11RenderTargetView* const* r,
                                                          ID3D11DepthStencilView* d, UINT us, UINT un,
                                                          ID3D11UnorderedAccessView* const* u, const UINT* k) {
        realOMSetRenderTargetsAndUavs(c, n, r, d, us, un, u, k);
    }
    static void OMSetBlendState(C* c, ID3D11BlendState* b, const FLOAT f[4], UINT m) { call_real<kHookBlend>(c, b, f, m); }
    static void OMSetDepthStencilState(C* c, ID3D11DepthStencilState* d, UINT r) { call_real<kHookDepth>(c, d, r); }
    static void RSSetViewports(C* c, UINT n, const D3D11_VIEWPORT* v) { realRSSetViewports(c, n, v); }
    static void RSSetScissorRects(C* c, UINT n, const D3D11_RECT* r) { realRSSetScissorRects(c, n, r); }
    static void SOSetTargets(C* c, UINT n, ID3D11Buffer* const* b, const UINT* o) { realSOSetTargets(c, n, b, o); }
};

// mvr_time_hooks: the sections are timed in 1 frame in kTimedEvery (timeHooks that frame), so the timing's own cost
// (two time-stamp reads per section, thousands of sections a frame) stays off the other frames; the log's figures are
// per timed frame.
bool timeHooksOn = false;
bool timeHooks = false;
inline constexpr unsigned long long kTimedEvery = 16;

// A timed section: its ticks added to `sum`, less those of sections inside it that are taken out of their parents
// (`takeOut`: the post-processing's start, DLSS among it, is not the hooks' or the tracker's own work).
struct Timed {
    uint64_t* const sum;
    const uint64_t t0;
    Timed* const parent;
    const bool takeOut;
    uint64_t excluded = 0;
    static inline thread_local Timed* current = nullptr;  // this thread's innermost running one
    explicit Timed(uint64_t& s, bool takeOutOfParents = false)
        : sum(timeHooks ? &s : nullptr), t0(sum ? __rdtsc() : 0), parent(sum ? current : nullptr),
          takeOut(takeOutOfParents) {
        if (sum)
            current = this;
    }
    ~Timed() {
        if (!sum)
            return;
        const uint64_t d = __rdtsc() - t0;
        *sum += d > excluded ? d - excluded : 0;
        current = parent;
        if (takeOut)
            for (Timed* p = parent; p; p = p->parent)
                p->excluded += d;
    }
    Timed(const Timed&) = delete;
    Timed& operator=(const Timed&) = delete;
};

// A game object recorded in State (`held`, its reference ours) replaced by `p`: referenced only when it changes.
template <typename T>
void hold(T*& held, T* p) {
    if (p == held)
        return;
    if (p)
        p->AddRef();
    if (held)
        held->Release();
    held = p;
}

// The class instances likewise: `count` of `src` (null: none) in place of the `heldCount` held (nothing to do with none,
// LMU's case). `src` mustn't be `held`.
void hold_instances(ID3D11ClassInstance** held, UINT& heldCount, ID3D11ClassInstance* const* src, UINT count) {
    for (UINT i = 0; src && i < count; ++i)
        if (src[i])
            src[i]->AddRef();
    for (UINT i = 0; i < heldCount; ++i)
        if (held[i])
            held[i]->Release();
    for (UINT i = 0; i < count; ++i)
        held[i] = src ? src[i] : nullptr;
    for (UINT i = count; i < heldCount; ++i)
        held[i] = nullptr;
    heldCount = count;
}
