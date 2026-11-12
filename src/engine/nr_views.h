// NVIDIA's DLSS neural rendering ("DLSS-NR") on the views Sim-QVFR's own DLSS works on (engine/dlss_views.h), for a
// game that ships neither: the model changes a view's lighting and materials from its colour, depth and motion vectors.
// The same API-level rule as DLSS here: nothing of the game's code is touched; the inputs are the ones dlss_views
// already makes. Called by dlss_views.cpp for each view, either on DLSS's output or (NrSettings::beforeDlss) on the
// view as drawn, before DLSS.
//   - where: the focus views; with NrSettings::periphery the eyes' views too, all of each or a part about the focus
//     view (peripheryCoverage).
//   - at what size: the region's, or smaller (focusScale, peripheryScale). The model's change of the picture (its
//     output less its input, both at the working size) is added to the full-size picture, so a smaller working size
//     keeps the picture's own detail.
//   - the scene is HDR before the game's tone mapping and the model works on 0..1: in through a curve that keeps the
//     hue and the highlights, out through its inverse (nr_math.h encode, decode).
//   - the region's edge fades to the picture as it came over a band (edgeBlend) wherever what lies beyond it isn't
//     processed: the focus view's edge without the periphery, the covered part's edge inside an eye's view.
//   - the runtime is D3D12's: engine/ngx_nr.h carries each view across and back.
// Needs a build with the NGX SDK and the player's own nvngx_dlssnr.dll (NrSettings::folder).
#pragma once

#include "nr_settings.h"

#include <dxgiformat.h>

struct ID3D11DeviceContext;
struct ID3D11Resource;
struct ID3D11ShaderResourceView;

namespace simqvfr {

// `tag`: the log lines' prefix (LMU: "lmu dlss-nr"). Off with settings.on false; live changes through nr_live.
void nr_start(const char* tag, const NrSettings& settings);
// On and not given up for the session.
bool nr_wanted();
// View `index` (0-1 the eyes, 2-3 their focus views) is processed, and before DLSS (else on its output).
bool nr_view_wanted(int index);
bool nr_before_dlss();
// The player's settings changed while the game runs, from any thread: taken at the next nr_frame_end.
void nr_live(const NrSettings& settings);

struct NrView {
    int index = 0;                        // 0-1 the eyes, 2-3 their focus views
    ID3D11Resource* scene = nullptr;      // the texture holding the view's colour (single-sample)
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    unsigned x = 0, y = 0;                // the view in it, as it is now: at (x, y), width x height
    unsigned width = 0, height = 0;
    // The view's own depth and motion vectors (to where the pixel was, in pixels of this size; x right, y down), both
    // guideWidth x guideHeight (DLSS's inputs: the view as drawn, smaller than its colour after an upscale).
    ID3D11ShaderResourceView* depth = nullptr;
    ID3D11ShaderResourceView* vectors = nullptr;
    unsigned guideWidth = 0, guideHeight = 0;
    bool depthInverted = false;
    bool reset = false;                   // no history for this view (its first frame, a cut)
    // An eye's view: where its focus view sits in it, fractions of the view (v down). Not known: the view's centre.
    bool focusKnown = false;
    double focusU = 0.5, focusV = 0.5;
};
// The view through the model and back into `scene`, on the game's immediate context. The caller holds the context's
// state (dlss_views.cpp's guard) and has a plain pipeline set; this leaves render targets and shader resources unbound.
// False: the view was left as it came.
bool nr_run_view(ID3D11DeviceContext* context, const NrView& view);
void nr_frame_end(unsigned long long frame);
// Every feature released, NGX on D3D12 shut down, nothing of the game's device held.
void nr_shutdown();

// Checks: evaluations that succeeded since the start; view `index`'s working size and region as last run.
unsigned long long nr_evaluations_total();
bool nr_view_for_checks(int index, unsigned region[4], unsigned working[2]);

}  // namespace simqvfr
