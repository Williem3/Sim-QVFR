# Third-party code

Copied unmodified from the upstream repositories; only the files this project builds or includes.

Not here, ever: NVIDIA's DLSS SDK (NGX headers, libraries, `nvngx_*.dll`). Its licence doesn't allow it in a public
repository, so it stays local (gitignored as `third_party/nvidia-dlss-sdk/`, or anywhere outside the checkout): see
CONTRIBUTING.md, "DLSS in LMU".

| Folder | Upstream | Version | Licence |
|---|---|---|---|
| `minhook/` | https://github.com/TsudaKageyu/minhook | v1.3.4 (`c3fcafdc10146beb5919319d0683e44e3c30d537`) | BSD 2-Clause, with the bundled HDE notice (`minhook/LICENSE.txt`) |
| `openxr/include/openxr/` | https://github.com/KhronosGroup/OpenXR-SDK | release-1.1.49 (`977f6675bc0057d5a54ed290cb5c71c699b1c0ab`) | Apache-2.0 OR MIT (SPDX headers; `openxr/LICENSES/`) |
| `nvapi/` | https://github.com/NVIDIA/nvapi | main of 2026-09-18 (`70d337db9186e968eab622f7e786de7e437faf3d`) | MIT (SPDX headers; `nvapi/License.txt`) |
| `fidelityfx-cas/` | https://github.com/GPUOpen-Effects/FidelityFX-CAS | `ffx_cas.h` 1.20190610 | MIT (`fidelityfx-cas/LICENSE.txt`, the header's notice): the licence only; the filter is ported into `src/quad/compose_shader.h` |
| `fonts/jost/` | https://github.com/indestructible-type/Jost | `35f141c970538f1ed0f235789c19156b3ce2a762` | SIL OFL 1.1 (`fonts/jost/OFL.txt`) |
| `fonts/inter/` | https://github.com/rsms/inter | 4.1 (`353b61b9f4430d5f420d56605a6e7993e0941470`) | SIL OFL 1.1 (`fonts/inter/LICENSE.txt`) |

Files: MinHook's `include/MinHook.h`, `src/*.c`, `src/*.h`, `src/hde/*`, `LICENSE.txt`,
`AUTHORS.txt`; OpenXR's `openxr.h`, `openxr_platform.h`, `openxr_platform_defines.h`,
`openxr_loader_negotiation.h`, `LICENSE`, and `LICENSES/Apache-2.0.txt`, `LICENSES/MIT.txt`; NVAPI's
`nvapi.h`, `nvapi_interface.h`, `nvapi_lite_*.h` and `License.txt` (headers only: the layer reaches the driver's
`nvapi64.dll` through `nvapi_QueryInterface`, no NVAPI library is linked). The launcher's fonts, embedded in
`sim-qvfr-launcher.exe` (`src/launcher/launcher.rc`): Jost's `fonts/ttf/Jost-700-Bold.ttf`, `Jost-800-Heavy.ttf`,
`Jost-900-Black.ttf` and `OFL.txt`; Inter's `docs/font-files/Inter-Regular.woff2`, `Inter-Medium.woff2`,
`Inter-SemiBold.woff2`, `Inter-Bold.woff2` and `LICENSE.txt`.
