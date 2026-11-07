# X-Plane: known bugs

Open issues in X-Plane's QVFR path with per-view sizes (on by default since 1.0.1-beta; `[experiment]
xplane_view_sizes=0` turns them off), found while testing live resolution (2026-11-06). Each view renders in a rect at the top-left of its layer; the layer keeps its launch size.
The detailed measurements are in the local recon notes (`docs/recon-xplane.md`, not in the repository).

Fixed in `3fe687a` (for reference): the auto exposure, the sun's shadows (`u_viewport_size`), the depth chains and SSR
all read each view's rect now.

## Ambient occlusion (CACAO) computed over the whole layer

**Symptom.** With the periphery below its launch value, the focus views show grey blocky blobs on bright surfaces (the
wing at 25%). Ctrl+Alt+2 (AO off, with `xplane_diag=1`) removes them.

**Cause.** CACAO runs once per view, but its constants describe the whole layer (its depth buffer size), so a view
smaller than its layer gets its positions, and so its occlusion, from the wrong place. The focus views also blend in
their eye's AO near their edges (the AO edge band), and that band is wide for small focus boxes: at a 15% box it covers
about 24% from each edge. So the eye's wrong AO reaches most of the focus view.

**Fix direction.** Give CACAO each view's rect: either its per-view constants (filled on the CPU for each view) or its
shaders' screen-to-view mapping, the way the other passes now take the push block's view shares. Then recheck the AO
edge band at small focus boxes.

## Compute passes still process whole layers

**Symptom.** None seen directly yet: GPU time spent on undrawn pixels, and possible seams at a view's edge.

**Cause.** Several compute passes dispatch over the full layer for every view (SSR, clouds, bloom, the lit miptree,
FSR sharpen) and do not know the view's rect. Raster passes already draw only the rect. A smaller view's undrawn part
is computed for nothing, and blur kernels near the rect's edge read what lies outside it.

**Fix direction.** Skip invocations outside the view's rect and clamp kernel reads to it. Take care with shaders that
synchronise their workgroup (barriers, shared memory), where returning early is not safe.

## SSR in the focus views' own screens is not adapted

**Symptom.** Only with `[experiment] xplane_ssr_from_eye=0` (not the default): the focus views' reflections are likely
wrong once their rect is smaller than the layer.

**Cause.** The SSR fix scales SSR's projection to the eye's rect, and applies only where the focus views march in their
eye's layer (the default). In own-screen mode the focus-box projection edit sits on the same matrix, so that mode is
left unedited.

**Fix direction.** Apply the rect scale after the focus-box projection, with the focus share, and test with the mode on.

## SSR reflections end without a fade at a smaller view's edge

**Symptom.** Not confirmed in the headset: reflections near the edge of a view smaller than its layer may stop
abruptly instead of fading.

**Cause.** SSR fades its hits near the screen's edge (uv 0 and 1). It now marches in the layer's coordinates, so for a
smaller view that fade sits at the layer's edge, past the view's own.

**Fix direction.** Check in a capture or frame shot at a low periphery. If it shows, move the fade to the view's rect.
