# LMU: Road focus, from a road corridor to depth focus (2026-11-22/23)

Road focus (fixed foveation only) keeps a world-space corridor along the road sharp, plus the player's car, and takes
the periphery's image everywhere else in the focus view. This note records the ideas for growing it, what was built,
what was measured and what is still open.

Code: `src/games/lmu/corridor_shader.h`, `src/games/lmu/mvr_pass_corridor.h`, `src/games/lmu/qvfr_frame.cpp`,
`src/quad/qvfr_settings.h` (`[quad] focus_corridor*`), `src/quad/road_focus_shape.h` (presets A/B).

## Shipped (on development)

- **Road widening** (`[quad] focus_corridor_spread`, 10-25 degrees, default 15, live): how much the corridor opens
  with distance, so bends far ahead stay sharp. At Spa a 1-degree run lost Les Combes to Pouhon; 15 degrees with 5 m
  width and 8 m height held them. Below about 10 the corridor is a straight band again; above 25 it takes in most of the
  far scenery.
- **Road edge blend** steps 0.1 m in the headset menu (was 0.5 m).
- **The preset capped to the runtime's focus region once it is known**: switching Road focus on before any session
  had reported the region (a first launch) left preset A at 70% x 60%, about 40% of it rendered and never shown under
  native quad views. The layer now caps an untouched preset at the region's first locate, in the run's ini and the
  launcher's.
- **The road and cars past a crest**: past a crest the far road shows as a strip too thin to read as road, so
  everything beyond it was measured against the crest and fell below the corridor's floor. The floor now drops by the
  steepest grade over the distance to the last road seen (`[experiment] lmu_corridor_drop_pct`, default 15). WARP dip
  scene: the car beyond it 0% -> 100% kept.

## Parked: the corridor along a recorded road path

Branch `claude/road-focus-path`, not merged. The car's path recorded from the camera's world position (no game data;
LMU's track files hold the AI line and edges but are encrypted, and decrypting them is ruled out), saved per track,
recognised by shape on later sessions, the pit lane as branches, fill-in laps widening each point to where the car
drove; the corridor then a tube along the path.

It worked as designed at Laguna Seca (lap and pit lane recorded, recognised in a new session, filled edge to edge),
but it doesn't pay. A/B at periphery 25%, focus density 120%, 72 fps, path (filled widths + 1.5 m) against the depth
corridor (5 m, 8 m, 15 degrees): **the path 0.13 ms a frame slower**, in all 8 windows. Its own test costs about
0.34 ms a frame (road pass 0.26 against 0.09 ms an eye); it saves only about 0.2 ms of rendering, because the depth
corridor at those settings is already tight. A stress run at higher density would at best break even, short of the
0.5 ms a frame set as the bar for the per-track recording work. Revisit only if the path test's cost can be cut a lot.

Colour-based auto-fill of the road's edges (instead of fill-in laps) was judged not worth it: fragile (shadows, wet
tarmac, kerbs, gravel) for little time saved.

## Dropped: a self-fitting box

A focus box fitted each frame to the corridor's kept area. With the car kept sharp the kept rows already fill 73-100%
of the box, NGX's output size is fixed per feature (a new size remakes it and resets its history), and the measured
DLSS lever was about 0.5 ms a pass for a 60% band. Not worth it next to the preset plus the mask.

## Open: a depth range beside the road

"Also sharp from X to Y m": a texel kept when it lies in the corridor or within a distance range, for braking boards,
kerbs and signs beside the road. One more term in `corridor_margin`; the near edge must apply only when near > 0
(otherwise the wheel and dash, nearer than the near blend, fade). It spends frame time (a 20-60 m range across the box
returns much of the scenery the corridor drops), so: `[experiment]` keys with a log of the box share kept first, a
player setting only if testers ask for more sharp scenery beside the road.
