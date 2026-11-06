// ACC's per-view screen-space effects, shared from each eye with its focus view (docs/recon-acc.md, "Screen-space
// effects"). UE 4.26 computes them per view in that view's own screen, so the focus views came out differently
// from the eyes round them, most at their edges, where the box meets the periphery:
//  - reflections (SSR): traced on the view's own screen and faded towards its edges, so a narrow focus view lost
//    most of them, and lost them at its border (a capture: 0.04-0.15 of its eye's near the box edge);
//  - ambient occlusion: the focus views' AO occluded a flat, open floor by a quarter (0.73 against the eye's 1.00)
//    and less so at their edges, where samples fall off-screen: the box darker than the periphery, its rim lighter
//    on a dark floor and darker on a light one. (The focus views' texels are some 2.2 times taller than wide, the
//    eye column's width over a short box, which UE's AO doesn't expect.)
// Each eye computes them as before; then its focus view's are refilled from its eye's, through the focus camera's
// geometry (quad/d3d11_view_resample.h), before anything reads them.
//
// Found at the D3D11 level, on ACC's immediate context (its RHI records passes and runs them later, so the
// engine's own pass code runs before anything reaches D3D11):
//  - SSR: the denoiser's dispatch writes a target-sized RGBA16F texture, and the draw right after it that reads
//    that texture with a cube array bound (the reflection captures) is the reflection composite of the view its
//    viewport is: just before a focus view's, its SSR texture is refilled from its eye's.
//  - AO: the one pass drawing into a target-sized R8_UNORM texture (render targets are followed as they are set),
//    a draw per view into one texture: right after a focus view's, its region is refilled from its eye's.
// Dispatch, DrawIndexed and the two OMSetRenderTargets are hooked through the context's vtable; everything else
// passes straight through.
#pragma once

struct ID3D11Device;

namespace acc {

// [experiment] acc_ssr_share, acc_ao_share (default on), read at xrCreateInstance.
void view_share_configure(bool ssr, bool ao);
// The session's D3D11 device (xrCreateSession): hooks its immediate context once per vtable, and shares while the
// four views run with a focus box (views_focused).
void view_share_start(ID3D11Device* device);
void view_share_stop();
// Every xrEndFrame while QVFR runs: its counters into the log every 900 frames.
void view_share_frame_end();

// Which view (0-3) a viewport is, given the views' rects (x, y, width, height; have[v] false: unknown); -1: none.
inline int view_at(const int (&rects)[4][4], const bool (&have)[4], float x, float y, float width, float height) {
    const auto within = [](float a, int b) { return a > b - 1.0f && a < b + 1.0f; };
    for (int v = 0; v < 4; ++v)
        if (have[v] && within(x, rects[v][0]) && within(y, rects[v][1]) && within(width, rects[v][2]) &&
            within(height, rects[v][3]))
            return v;
    return -1;
}

}  // namespace acc
