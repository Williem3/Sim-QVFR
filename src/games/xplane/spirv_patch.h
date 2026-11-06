// Small edits to SPIR-V words (the Vulkan shader format), applied at runtime to X-Plane's own shaders so that no
// copy of them ships with Sim-QVFR (src/games/xplane/README.md).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace xplane {

// Adds `add` to the value of the `int` input variable named `name` where the shader loads it: the one OpLoad of
// that variable gets a new id, and an OpIAdd of a new constant gives the old id, so every use sees the sum.
// False (and `words` unchanged) unless the module has exactly one such load. `why` says what was missing.
bool spirv_add_to_input(std::vector<std::uint32_t>& words, const char* name, std::int32_t add, std::string* why);

// For the uniform blocks named in `blocks` (their variables' names, e.g. "u_gbuffer_data_var"): every indexed access
// into a member that is an array of two (X-Plane's per-eye arrays) takes `index & 1` instead of `index`, so views
// 2-3 read their eye's entry. Returns how many accesses were edited (0: `words` unchanged); `skipped` counts
// accesses whose index type couldn't be found (left as they were).
int spirv_fold_eye_arrays(std::vector<std::uint32_t>& words, const std::vector<std::string>& blocks, int* skipped);

// Compute shaders that take their eye from the dispatch depth as `eye = (gl_GlobalInvocationID.z == 1) ? 1 : 0` and
// write to layer `eye` (X-Plane's cloud render): the test becomes `(z & 1) == 1` (views 2-3 act as eyes 0-1) and the
// written layer becomes z (a 3-component int coordinate whose last component was `eye` now takes z). True if edited.
bool spirv_layer_from_dispatch_z(std::vector<std::uint32_t>& words);

// X-Plane's final resolve in eye columns (README.md, "Composition"): its output is two layers tall per eye column, and
// it samples at `v = u_y_offset - y * u_offset.y` in layer `v_eye`. A pixel of the lower row has v <= 0 there: it takes
// v + 1 (in layer v_eye, the eye's view); the upper row takes layer v_eye + 2 (the focus view). X-Plane's blit into the
// D3D11 image flips it vertically, so there the eye's view is on top and its focus view below. The test sits right after the subtraction
// (an OpFSub of the loaded `u_y_offset`), the layer at the one load of `v_eye`, which must come after it. False (and
// `words` unchanged) if any piece is missing; `why` says which.
bool spirv_eye_columns_resolve(std::vector<std::uint32_t>& words, std::string* why);

// The light billboards' size (README.md, "Measured: focus views darker"): X-Plane sizes each light as a few pixels by
// projecting its centre with `transform.projection[i]`, offsetting it a pixel and un-projecting with the per-eye
// `block.member[i]` (u_ogl_light_data.u_proj_inv). For the focus views that pair must be the eye's (index & 1), so a
// light covers the same angle as in its eye (a focus pixel is a fraction of the eye's); its placement keeps the view's
// own projection. So: in each basic block that reads `block.member`, the projection accesses before that read take
// `index & 1`. Blocks and members by their types' names. Returns how many accesses (0: `words` unchanged).
int spirv_eye_projection_beside(std::vector<std::uint32_t>& words, const char* block, const char* member,
                                const char* transform, const char* projection);

// Per-view post-processing taken from the eye (README.md "Measured: bloom per view"): a vertex shader that writes
// `v_eye` (the view) and binds u_transform_block_fat gets a flat vec4 output at `location`: the affine map from the
// view's uv to its eye's (scale xy, offset zw), from projection_matrix[v] and [v & 1] (identity for the eyes).
bool spirv_view_to_eye_map_out(std::vector<std::uint32_t>& words, std::uint32_t location, std::string* why);
// A fragment shader that samples at gl_FragCoord.xy * (scale) in layer v_eye (X-Plane's bloom apply): it reads that map
// at `location`, samples at the position mapped into the eye's view, in layer v_eye & 1.
bool spirv_sample_eye_box(std::vector<std::uint32_t>& words, std::uint32_t location, std::string* why);

// X-Plane's shading-rate compute (VRS; README.md "Measured: the focus views shaded coarsely"): every tile starts at
// u_fill_rate (coarse) and only the shapes of its layer refine it, and X-Plane makes shapes for layers 0-1 only. In
// layers >= firstLayer every tile gets rate 0 (1x1, full). Returns the image writes edited (0: `words` unchanged).
int spirv_full_rate_from_layer(std::vector<std::uint32_t>& words, std::uint32_t firstLayer, std::string* why);

// Per-eye projection coefficients (README.md "Measured: the sky and positions in the focus views"): `member` is a
// vec4[2] (xy scale, zw offset: the view ray's tangents at screen position 0..1), which the fold reads at index & 1.
// After each such load, views 2-3 (index >= 2) get the focus box's coefficients from their eye's: (fx c.x, fy c.y,
// -fx c.x / 2, -fy c.y / 2), the box centred on the eye's forward direction. Returns loads edited; `skipped` counts
// reads of single components (left as they are).
int spirv_focus_proj_coefficients(std::vector<std::uint32_t>& words, const char* member, float fx, float fy,
                                  int* skipped);

// Per-eye projection matrices (README.md "Measured: the wing lit differently in the focus views"): `block.member` is a
// mat4[2] (u_ssr_data.u_proj: SSR projects its reflected rays with it), which the fold reads at index & 1. After each
// such load, views 2-3 (index >= 2) get the focus box's projection from their eye's, as focus_projection builds it:
// col0.x / fx, col1.y / fy, col2.x = col2.y = 0 (the box centred on the eye's forward direction). Returns loads edited.
int spirv_focus_projection_matrix(std::vector<std::uint32_t>& words, const char* block, const char* member, float fx,
                                  float fy);

// Reads along a screen-space ray from the eye (README.md "Measured: focus views reflect only what is in the box"): in a
// compute shader indexed by the dispatch z (one layer per view), the reads of the images named in `images` whose
// coordinate's last component (the layer) is z, or z converted to float, take z & 1: views 2-3 march and fetch in their
// eye's layer. Each read gets its own coordinate (the original is left for other reads). Returns the reads edited.
int spirv_ray_reads_from_eye(std::vector<std::uint32_t>& words, const std::vector<std::string>& images);

// Alpha tests matched across views (README.md "Measured: a 'wall' missing in the focus view"). X-Plane's alpha-tested
// materials (fences, foliage) discard where the texture's alpha <= u_alpha_cut, the texture sampled at an implicit LOD:
// a periphery pixel (1 / fraction the focus view's angular density) reads a coarser mip, so a chain-link fence is a
// solid band there and see-through in the focus view.
// A vertex shader that writes gl_Layer also writes it to a flat int output at `location` (returns true if added).
bool spirv_layer_out(std::vector<std::uint32_t>& words, std::uint32_t location);
// A fragment shader with such an alpha test, binding u_transform_block_fat: the implicit-LOD samples whose alpha reaches
// the test get a second sample with LOD bias `bias` (log2 of the focus fraction: the focus view's mip) whose alpha
// replaces theirs, in views 0-1 of a four-view pass (the layer read at `location`, u_transform_count == 4); colour keeps
// its own mip. Returns the samples edited (0: `words` unchanged; `why` says what was missing).
int spirv_alpha_test_lod_bias(std::vector<std::uint32_t>& words, std::uint32_t location, float bias, std::string* why);

// Tiled lights (README.md "Measured: right-eye popping near the wing"): deferred shading picks its light tile as
// `int(gl_FragCoord.xy) / u_light_tile_size` in one grid laid out in eye-screen space, shared by all views. For views
// 2-3 (v_eye >= 2) each divided pixel coordinate becomes its eye's: (uv0 - f / 2) * size + xy * f, uv0 = -c.zw / c.xy
// from the eye's u_gbuffer_data.u_proj_coefficients[v_eye & 1] (the focus box's centre, tangent 0), size =
// u_viewport_size, f = fx / fy. Returns the divisions edited (0: `words` unchanged; `why` says what was missing).
int spirv_focus_light_tiles(std::vector<std::uint32_t>& words, float fx, float fy, std::string* why);

// The eye tests beside a per-eye member that isn't an array (README.md "Measured: dark shadow edges in the focus
// views"): deferred shading brings the right eye's view-space position into the left eye's for the shadow lookup with
// `block.member` (u_shadow_data.u_eye_right_to_left) when `input == 1` (v_eye), so view 3 (the right focus view) looked
// its shadows up from the left eye, 6 cm off. In a module that reads `block.member`, every integer equality test of a
// loaded `input` against the constant 0 or 1 compares `input & 1` instead: views 2-3 act as their eyes. Returns the
// tests edited (0: `words` unchanged).
int spirv_eye_parity_test(std::vector<std::uint32_t>& words, const char* block, const char* member, const char* input);

}  // namespace xplane
