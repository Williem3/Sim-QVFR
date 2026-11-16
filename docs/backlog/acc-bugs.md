# Assetto Corsa Competizione: known bugs

Issues in ACC's QVFR path; none is open. The detailed measurements are in the local recon notes (`docs/recon-acc.md`,
not in the repository).

## Fixed: the focus region was darker than the periphery in sunlight

**Symptom.** The focus region shows up as a slightly darker rectangle in the headset, most visibly on dark, sunlit
surfaces such as the dash (2026-11-06, Spa, focus box 20%, eye tracking on). Frame shots put the focus views at 0.87-0.94
of their eyes' brightness over the same region. Once (frame 7132) the dash inside the focus region also showed dark
rectangular blocks with straight edges; they didn't appear in a later capture.

**What it isn't.**
- SSR and ambient occlusion: switching either off (`acc_diag=1`, Ctrl+Alt+1 / Ctrl+Alt+2) leaves the gap. Switching
  the focus views' sharing of their eye's SSR and AO off (Ctrl+Alt+4) makes it worse, so that sharing helps.
- Where the sun's shadows fall: a capture shows each focus view's shadow mask matching its eye's (about 1% of pixels
  differ).
- Our recent changes: ACC's rendering code is unchanged since before the X-Plane work.

**What it is, as far as measured.** Switching the sun's shadows off (Ctrl+Alt+3) brings the focus views to their eyes'
brightness, because everything is then lit alike. In the capture, scene colour matches before lighting; the sun's
direct-light pass then gives sunlit pixels in the focus views about 9% less light than their eyes. It's noisy per pixel
with no trend across the view, so it doesn't look like a position or projection error. More likely the materials respond
differently at the focus views' higher resolution (finer normal, roughness or colour detail), but a G-buffer comparison
on the dark dash was too noisy to confirm it.

**Fixed (2026-11-08), confirmed in the headset (2026-11-15).** Three changes fit the symptom:
- UE's contact shadows are held off while QVFR runs (`acc_contact_shadows_off`, default on; f6ea2aa). They march each
  view's own depth, so a sunlit band on the dash below the windscreen base could be lit in one view and shadowed in the
  other. They touch only the sun's direct light, which matches the measurement above (Ctrl+Alt+3 removed the gap).
- Every view takes the left eye's auto exposure and pre-exposure (`acc_exposure_share`, default on; f6ea2aa). The views
  were 6-11% apart before.
- The focus views take their eye's bloom and dirt mask (`acc_bloom_share`, default on; 70069a1).
