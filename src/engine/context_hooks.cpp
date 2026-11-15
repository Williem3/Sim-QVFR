#include "context_hooks.h"

#include <windows.h>
#include <d3d11_4.h>
#include <wrl/client.h>

#include <MinHook.h>

#include <mutex>
#include <tuple>
#include <utility>

#include "../log.h"

namespace simqvfr::context_hooks {
namespace {

using Microsoft::WRL::ComPtr;

// d3d11.h order: IUnknown 3 + ID3D11DeviceChild 4, then ID3D11DeviceContext's own.
constexpr unsigned kSlot[kEntries] = {33, 34, 12, 13, 20, 21, 39, 40, 58, 44, 45, 119, 123, 15};
constexpr const char* kName[kEntries] = {"OMSetRenderTargets",   "OMSetRenderTargetsAndUnorderedAccessViews",
                                         "DrawIndexed",          "Draw",
                                         "DrawIndexedInstanced", "DrawInstanced",
                                         "DrawIndexedInstancedIndirect", "DrawInstancedIndirect",
                                         "ExecuteCommandList",   "RSSetViewports",
                                         "RSSetScissorRects",    "VSSetConstantBuffers1",
                                         "PSSetConstantBuffers1", "Unmap"};
constexpr int kFunctions = 4;  // per entry: LMU's measurements found 4 on WARP and an RTX 5070 Ti

struct Functions {
    void* target[kFunctions] = {};
    void* real[kFunctions] = {};
    int count = 0;
    bool full = false;
};

struct State {
    ID3D11DeviceContext* context = nullptr;  // raw: the game's, alive while its session is
    uint32_t wanted = 0;
    Observers observers;
    Functions functions[kEntries];
    Stats stats;
    std::mutex mutex;  // install, rescan and uninstall (the game's thread, and the layer's at session end)
} g;

// Calls inside an observer, and one d3d11 function calling another of the same entry, go straight through.
thread_local int depth = 0;

template <Entry E, typename... A>
struct Detours {
    template <int N>
    static void STDMETHODCALLTYPE fn(ID3D11DeviceContext* context, A... a) {
        using Fn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, A...);
        const Fn real = reinterpret_cast<Fn>(g.functions[E].real[N]);
        if (depth != 0 || context != g.context) {
            ++depth;
            real(context, a...);
            --depth;
            return;
        }
        if constexpr (E == kRSSetViewports || E == kRSSetScissorRects) {
            filter<E>(real, context, a...);
        } else if constexpr (E == kVSSetConstantBuffers1 || E == kPSSetConstantBuffers1) {
            const auto args = std::make_tuple(a...);
            const UINT start = std::get<0>(args), count = std::get<1>(args);
            ID3D11Buffer* out[D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT];
            ++depth;
            if (g.observers.constant_buffers && std::get<2>(args) && count <= D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT &&
                g.observers.constant_buffers(context, E == kPSSetConstantBuffers1, start, count, std::get<2>(args), out))
                real(context, start, count, out, std::get<3>(args), std::get<4>(args));
            else
                real(context, a...);
            --depth;
        } else {
            ++depth;
            observe<E>(context, a...);
            bool replaced = false;
            if constexpr (E == kDrawIndexed || E == kDraw || E == kDrawIndexedInstanced || E == kDrawInstanced) {
                if (g.observers.draw_replace)
                    replaced = g.observers.draw_replace(context, E, call<E>(a...));
            }
            if (!replaced)
                real(context, a...);
            if constexpr (E == kDrawIndexed || E == kDraw || E == kDrawIndexedInstanced || E == kDrawInstanced ||
                          E == kDrawIndexedInstancedIndirect || E == kDrawInstancedIndirect) {
                if (g.observers.draw_after)
                    after<E>(context, a...);
            }
            --depth;
        }
    }

    template <Entry X, typename Fn>
    static void filter(Fn real, ID3D11DeviceContext* c, A... a) {
        const auto args = std::make_tuple(a...);
        const UINT count = std::get<0>(args);
        ++depth;
        if constexpr (X == kRSSetViewports) {
            D3D11_VIEWPORT out[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE];
            if (g.observers.viewports && count <= D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE &&
                g.observers.viewports(c, count, std::get<1>(args), out))
                real(c, count, out);
            else
                real(c, a...);
        } else {
            D3D11_RECT out[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE];
            if (g.observers.scissors && count <= D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE &&
                g.observers.scissors(c, count, std::get<1>(args), out))
                real(c, count, out);
            else
                real(c, a...);
        }
        --depth;
    }

    template <Entry X>
    static DrawCall call(A... a) {
        const auto args = std::make_tuple(a...);
        DrawCall d;
        d.count = std::get<0>(args);
        if constexpr (X == kDrawIndexed) {
            d.start = std::get<1>(args);
            d.baseVertex = std::get<2>(args);
        } else if constexpr (X == kDraw) {
            d.start = std::get<1>(args);
        } else if constexpr (X == kDrawIndexedInstanced) {
            d.instances = std::get<1>(args);
            d.start = std::get<2>(args);
            d.baseVertex = std::get<3>(args);
            d.startInstance = std::get<4>(args);
        } else if constexpr (X == kDrawInstanced) {
            d.instances = std::get<1>(args);
            d.start = std::get<2>(args);
            d.startInstance = std::get<3>(args);
        }
        return d;
    }

    template <Entry X>
    static void after(ID3D11DeviceContext* c, A... a) {
        if constexpr (X == kDraw) {
            const auto args = std::make_tuple(a...);
            g.observers.draw_after(c, X, std::get<0>(args), 1, std::get<1>(args));
        } else if constexpr (X == kDrawIndexed) {
            g.observers.draw_after(c, X, std::get<0>(std::make_tuple(a...)), 1, 0);
        } else if constexpr (X == kDrawIndexedInstanced || X == kDrawInstanced) {
            const auto args = std::make_tuple(a...);
            g.observers.draw_after(c, X, std::get<0>(args), std::get<1>(args), 0);
        } else {
            g.observers.draw_after(c, X, 0, 0, 0);
        }
    }
    template <std::size_t... N>
    static void* get(int n, std::index_sequence<N...>) {
        void* const table[] = {reinterpret_cast<void*>(&fn<static_cast<int>(N)>)...};
        return table[n];
    }
    static void* get(int n) { return get(n, std::make_index_sequence<kFunctions>{}); }

    template <Entry X>
    static void observe(ID3D11DeviceContext* c, A... a) {
        const Observers& o = g.observers;
        if constexpr (X == kOMSetRenderTargets || X == kOMSetRenderTargetsAndUavs) {
            const auto args = std::make_tuple(a...);
            const UINT count = std::get<0>(args);
            if (o.targets && count != D3D11_KEEP_RENDER_TARGETS_AND_DEPTH_STENCIL)
                o.targets(c, count, std::get<1>(args), std::get<2>(args));
        } else if constexpr (X == kDrawIndexed || X == kDraw) {
            if (o.draw)
                o.draw(c, X, std::get<0>(std::make_tuple(a...)), 1);
        } else if constexpr (X == kDrawIndexedInstanced || X == kDrawInstanced) {
            const auto args = std::make_tuple(a...);
            if (o.draw)
                o.draw(c, X, std::get<0>(args), std::get<1>(args));
        } else if constexpr (X == kDrawIndexedInstancedIndirect || X == kDrawInstancedIndirect) {
            if (o.draw)
                o.draw(c, X, 0, 0);
        } else if constexpr (X == kExecuteCommandList) {
            if (o.execute)
                o.execute(c, std::get<0>(std::make_tuple(a...)));
        } else if constexpr (X == kUnmap) {
            const auto args = std::make_tuple(a...);
            if (o.unmap)
                o.unmap(c, std::get<0>(args), std::get<1>(args));
        }
    }
};

void* detour_for(int e, int n) {
    using RTVs = ID3D11RenderTargetView* const*;
    switch (e) {
        case kOMSetRenderTargets:
            return Detours<kOMSetRenderTargets, UINT, RTVs, ID3D11DepthStencilView*>::get(n);
        case kOMSetRenderTargetsAndUavs:
            return Detours<kOMSetRenderTargetsAndUavs, UINT, RTVs, ID3D11DepthStencilView*, UINT, UINT,
                           ID3D11UnorderedAccessView* const*, const UINT*>::get(n);
        case kDrawIndexed: return Detours<kDrawIndexed, UINT, UINT, INT>::get(n);
        case kDraw: return Detours<kDraw, UINT, UINT>::get(n);
        case kDrawIndexedInstanced: return Detours<kDrawIndexedInstanced, UINT, UINT, UINT, INT, UINT>::get(n);
        case kDrawInstanced: return Detours<kDrawInstanced, UINT, UINT, UINT, UINT>::get(n);
        case kDrawIndexedInstancedIndirect: return Detours<kDrawIndexedInstancedIndirect, ID3D11Buffer*, UINT>::get(n);
        case kDrawInstancedIndirect: return Detours<kDrawInstancedIndirect, ID3D11Buffer*, UINT>::get(n);
        case kExecuteCommandList: return Detours<kExecuteCommandList, ID3D11CommandList*, BOOL>::get(n);
        case kRSSetViewports: return Detours<kRSSetViewports, UINT, const D3D11_VIEWPORT*>::get(n);
        case kRSSetScissorRects: return Detours<kRSSetScissorRects, UINT, const D3D11_RECT*>::get(n);
        case kVSSetConstantBuffers1:
            return Detours<kVSSetConstantBuffers1, UINT, UINT, ID3D11Buffer* const*, const UINT*, const UINT*>::get(n);
        case kPSSetConstantBuffers1:
            return Detours<kPSSetConstantBuffers1, UINT, UINT, ID3D11Buffer* const*, const UINT*, const UINT*>::get(n);
        case kUnmap: return Detours<kUnmap, ID3D11Resource*, UINT>::get(n);
        default: return nullptr;
    }
}

// The function entry `e` holds now, hooked if new. Under g.mutex.
void hook_entry(int e) {
    if (!(g.wanted & (1u << e)))
        return;
    void* fn = (*reinterpret_cast<void** const*>(g.context))[kSlot[e]];
    Functions& f = g.functions[e];
    for (int i = 0; i < f.count; ++i)
        if (f.target[i] == fn)
            return;
    if (f.count == kFunctions) {
        if (!f.full) {
            f.full = true;
            ++g.stats.full;
            logf("context hooks: the context's %s entry holds a fifth function: calls through it aren't seen", kName[e]);
        }
        return;
    }
    const int n = f.count;
    f.target[n] = fn;
    if (MH_CreateHook(fn, detour_for(e, n), &f.real[n]) == MH_OK && MH_EnableHook(fn) == MH_OK) {
        ++g.stats.functions;
    } else {
        f.real[n] = fn;
        ++g.stats.failed;
        logf("context hooks: the context's %s function %p couldn't be hooked", kName[e], fn);
    }
    ++f.count;  // last: the detours read real[] up to it
}

void hook_entries() {
    for (int e = 0; e < kEntries; ++e)
        hook_entry(e);
}

}  // namespace

bool install(ID3D11DeviceContext* context, uint32_t wanted, const Observers& observers) {
    std::lock_guard lock(g.mutex);
    if (!context || g.context)
        return g.context == context && context;
    if (const MH_STATUS init = MH_Initialize(); init != MH_OK && init != MH_ERROR_ALREADY_INITIALIZED) {
        log_line("context hooks: MH_Initialize failed: off");
        return false;
    }
    g.context = context;
    g.wanted = wanted;
    g.observers = observers;
    ComPtr<ID3D11Device> device;
    context->GetDevice(&device);
    // A command of ours that changes nothing the game sees: a 16-byte constant buffer of our own updated.
    ComPtr<ID3D11Buffer> scratch;
    const D3D11_BUFFER_DESC bd = {16, D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0};
    if (device)
        device->CreateBuffer(&bd, nullptr, &scratch);
    const float zero[4] = {};
    const auto idle_and_busy = [&] {
        hook_entries();
        context->Flush();
        hook_entries();
        if (scratch) {
            ++depth;  // our own command: not observed
            context->UpdateSubresource(scratch.Get(), 0, nullptr, zero, 0, 0);
            --depth;
        }
        hook_entries();
    };
    idle_and_busy();
    ComPtr<ID3D11Multithread> multithread;
    if (SUCCEEDED(context->QueryInterface(IID_PPV_ARGS(&multithread))) && !multithread->GetMultithreadProtected()) {
        multithread->SetMultithreadProtected(TRUE);
        idle_and_busy();
        multithread->SetMultithreadProtected(FALSE);
        hook_entries();
        g.stats.protectionTried = true;
    }
    logf("context hooks: %d d3d11 functions hooked for %d entries%s%s", g.stats.functions,
         [&] {
             int n = 0;
             for (int e = 0; e < kEntries; ++e)
                 n += (wanted >> e) & 1;
             return n;
         }(),
         g.stats.failed ? " (some refused)" : "", g.stats.protectionTried ? "" : " (protection already on: its plain functions at rescans)");
    return true;
}

void rescan() {
    std::lock_guard lock(g.mutex);
    if (g.context)
        hook_entries();
}

// The hooks stay in d3d11's code (a game thread may be inside one): with no context they pass every call straight on.
void uninstall() {
    std::lock_guard lock(g.mutex);
    g.context = nullptr;
}

Bypass::Bypass() { ++depth; }
Bypass::~Bypass() { --depth; }

Stats stats() {
    std::lock_guard lock(g.mutex);
    return g.stats;
}

bool installed() {
    std::lock_guard lock(g.mutex);
    return g.context != nullptr;
}

}  // namespace simqvfr::context_hooks
