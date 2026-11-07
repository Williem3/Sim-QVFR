# X-Plane: performance optimization experiments

Ideas to make X-Plane's QVFR path cheaper, each to be measured before it is kept. They aren't bugs: the image is right
without them. The measurements and the engine details behind them are in the local recon notes (`docs/recon-xplane.md`,
not in the repository), section "Measured: what limits X-Plane with QVFR".

What limits X-Plane with QVFR (2026-11-07, the CPU profiler's slow-frame reports on one flight): without QVFR X-Plane is
GPU-bound (26 ms frames, its main thread waiting about 10 ms of each on the GPU). With QVFR the GPU wait is gone and the
main thread is the limit at about 13 ms of work per frame, mostly recording the frame's rendering and the GPU driver
behind it, then the tree cull and the world draw. QVFR's own code doesn't register. Reaching 90 Hz needs that main
thread under 11.1 ms.

## Object detail without the extra draw distance

**Idea.** Keep the finer tree and object meshes near the viewer that the focus views need, but stop drawing trees and
objects at the distance X-Plane itself would. Today one LOD value sets both.

**Why.** Raising the detail to the focus views' density (Object detail 100%, the default) also draws trees and objects
about 2.5x farther out at the default settings, in all four views. The periphery needs neither, and the main thread
pays for every extra object. Measured with the detail at X-Plane's own (`xplane_focus_lod=0`) on the same flight: slow
frames 16.5% -> 6%, the worst frame 48 ms -> 30 ms, the median about 0.5 ms lower.

**How (to try).** Split the one value in two: the mesh level from the focus views' density as now, the far cutoff from
X-Plane's own, in both the GPU tree cull and the CPU object cull.

**Measure.** The CPU profiler's slow-frame report (`cpu_profile=1`, Ctrl+Alt+C) on the same flight at Object detail
100%, 0% and with the split: main thread ms per frame and the slow frames. Worth keeping if it gets most of the 0%
saving at 100%'s near detail. The Object detail slider (launcher, Resolution pane) gives in-between points: if the cost
falls steeply as the slider goes down, the distance is the expensive part.

**Risk.** Trees appearing at the far cutoff as they do without QVFR, and a visible step where the finer near levels
end; check the focus views at a forest's edge.
