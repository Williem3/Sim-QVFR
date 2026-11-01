# FormSection

Grouped settings box in the macOS Settings style: a header, rows separated by inset hairlines, and an optional footer.

**Consumer provides:** `children` (`FormRow` elements), optional `title`, `footer`.

**Use**
- One section per topic: Rendering, Per-eye resolution, Focus region, Focus overlay.
- Section titles are sentence case. Footers explain consequences ("Applies at next launch.") in one or two sentences.
- Stack sections with `space-6` between them; the content area keeps `space-5` margins.

**Don't**
- Don't nest sections or put a section inside a row.
