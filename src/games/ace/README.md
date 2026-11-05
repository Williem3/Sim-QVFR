# Assetto Corsa EVO

Kunos's own engine, D3D12, OpenXR VR with `-vr`. Pinned to one `AssettoCorsaEVO.exe` build (`game.h`);
every address in `abi.h` comes from that build, with how it was found.

| File | What it does |
| --- | --- |
| `game.h` | Identity: exe, Steam app id 3058630, the supported build's SHA-256 |
| `abi.h` | Renderer offsets, function RVAs and anchor bytes, with notes from static analysis and test runs |
| `adapter.cpp` | ACE's `GameAdapter`: forwards QVFR to views4 and reads ACE's own diagnostics from the ini |
| `views4.cpp` | QVFR: the renderer draws 4 views per VR frame (see below) |
| `stubs.h` | A tiny x64 assembler for the code stubs views4 patches in |
| `views3.cpp`, `double_render.cpp` | Early research experiments (`mode=views3`, `mode=double_render`, which crashes ACE) |
| `batch_watch.cpp` | Log-only watch on the instance visibility batch against ACE's limits |

## How QVFR works in ACE (views4)

ACE's VR path keeps its stereo view mode (mode 1); only the view count grows to 4 where it counts:

- `viewCount()` returns 4 to an allowlist of call sites (`views4_callers` picks them).
- The viewport function lays the four views out in ACE's target (`quad/views4_layout.h`).
- Per-view records, the 3-slot camera block and the history slots fold views 2–3 onto the eyes'
  slots, or point at "phantom renderers" in layer memory for the focus views' own cameras.
- Tiled light culling, fog, the lighting loops and DLSS get per-view state for all four views.
- The layer doubles the eye width ACE sees, so its double-wide target holds four eye-sized views.

The comments in `abi.h` and `views4.h` explain each patch and the run that called for it.

## ACE-only ini keys

`[experiment]` `views4_callers`, `views4_borrow`, `views4_eyes_only_advance`,
`views4_shadow_view1_plain`, `views4_screen_size_legacy` and `batch_watch` are read by `adapter.cpp`,
not the layer. `manifest/sim-qvfr.ini` documents them.

The static-analysis notes (`docs/recon.md`) are kept out of the repository.
