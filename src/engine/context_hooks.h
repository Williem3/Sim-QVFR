// A game's D3D11 immediate context observed at the API level, game-neutral (first used by ATS's pass probe; LMU's
// mvr_pass.cpp has its own copy of the same mechanism, to move here when it is next touched).
//
// d3d11's immediate context keeps its vtable inside itself and rewrites entries as it runs: every command's as the
// context goes idle (a flush) and busy (a command), and ~130 as multithread protection goes on or off. A hook on one
// function an entry held misses the calls through the others, so every function each wanted entry is seen holding is
// hooked in d3d11's code (MinHook), with a detour of its own: those found at install (as is, after a Flush, after a
// command; then the same with protection on) and at every rescan (call it at points the game's mode may have changed:
// each frame end). A vtable copy is wrong for the context (it freezes the entries; docs/recon-lmu.md).
//
// The observers run before the game's call goes on, on the game's thread, and must not call the context's hooked
// functions themselves (calls made from inside an observer, or from one d3d11 function into another, go straight
// through).
#pragma once

#include <d3d11.h>

#include <cstdint>

namespace simqvfr::context_hooks {

// The entries that can be observed (d3d11.h vtable order in kSlot).
enum Entry : int {
    kOMSetRenderTargets,
    kOMSetRenderTargetsAndUavs,
    kDrawIndexed,
    kDraw,
    kDrawIndexedInstanced,
    kDrawInstanced,
    kDrawIndexedInstancedIndirect,
    kDrawInstancedIndirect,
    kExecuteCommandList,
    kEntries
};

struct Observers {
    // OMSetRenderTargets and the render-target half of OMSetRenderTargetsAndUnorderedAccessViews (count
    // D3D11_KEEP_RENDER_TARGETS_AND_DEPTH_STENCIL: unchanged, not reported).
    void (*targets)(ID3D11DeviceContext* context, UINT count, ID3D11RenderTargetView* const* rtvs,
                    ID3D11DepthStencilView* dsv) = nullptr;
    // Any draw: which entry, and its vertex or index count and instance count (1 for the plain ones, 0 if indirect).
    void (*draw)(ID3D11DeviceContext* context, Entry entry, UINT count, UINT instances) = nullptr;
    // ExecuteCommandList, before it runs (d3d11 replays the list through the context's own functions: those calls are
    // seen too, inside it).
    void (*execute)(ID3D11DeviceContext* context, ID3D11CommandList* list) = nullptr;
};

struct Stats {
    int functions = 0;        // d3d11 functions hooked over all entries
    int failed = 0;           // functions MinHook refused (passed on unobserved)
    int full = 0;             // entries that held more functions than the hooks have room for
    bool protectionTried = false;  // install also found the functions multithread protection switches to
};

// Hooks the entries `wanted` (bits of Entry) on `context` (the game's immediate context). False: nothing hooked
// (MinHook unavailable, no context, or already installed on another one).
bool install(ID3D11DeviceContext* context, uint32_t wanted, const Observers& observers);
// The entries' current functions hooked if new (the game's thread, between its commands: an xrEndFrame).
void rescan();
// Observing stops (session end): the hooks stay in d3d11's code and pass every call straight on. A later install on
// the same context hooks again only what's new.
void uninstall();
Stats stats();
bool installed();

}  // namespace simqvfr::context_hooks
