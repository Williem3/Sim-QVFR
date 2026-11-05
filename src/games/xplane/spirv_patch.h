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

}  // namespace xplane
