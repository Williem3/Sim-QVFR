# Third-party components

- MinHook 1.3.4: BSD 2-Clause, with the bundled HDE notice. See `third_party/minhook/LICENSE.txt`.
- Khronos OpenXR SDK headers (release-1.1.49): Apache-2.0 OR MIT. See `third_party/openxr/LICENSES/`.
- NVIDIA NVAPI headers (`70d337d`): MIT. See `third_party/nvapi/License.txt` and each header's SPDX notice.
- AMD FidelityFX CAS (Contrast Adaptive Sharpening, `ffx_cas.h` 1.20190610): MIT, Copyright (c) 2017-2019 Advanced
  Micro Devices, Inc. Its filter is ported, not copied, into the compositor's shader (`src/quad/compose_shader.h`,
  `cas_sample`). See `third_party/fidelityfx-cas/LICENSE.txt`.
- NVIDIA DLSS SDK 310.9.1 (github.com/NVIDIA/DLSS): Copyright (c) NVIDIA Corporation. For NVIDIA DLSS in Le Mans
  Ultimate, releases built with DLSS contain its NGX library, statically linked into the layer (`sim-qvfr.dll`),
  and its DLSS runtime, `layer/nvngx_dlss.dll`, as NVIDIA ships it (signed by NVIDIA, unchanged). Neither is in
  this repository. Licensed to Sim-QVFR under the NVIDIA RTX SDKs License, shipped with those releases as
  `licenses/NVIDIA-RTX-SDKs-LICENSE.txt`.

**Your terms for the NVIDIA components.** They are NVIDIA's, and the project's licence (PolyForm Noncommercial)
does not cover them. You may use them under these terms, which keep NVIDIA's licence above in force for them:
- Use them only as part of Sim-QVFR, on your own PC: not to provide a hosted or cloud service to others.
- Don't distribute them, on their own or with Sim-QVFR. To share Sim-QVFR, share the link to its release.
- Don't modify, reverse engineer, decompile or disassemble them, or remove their copyright or other notices.
- Don't bypass, disable or circumvent their technical limitations, encryption, security or authentication.
- Don't use them where their use or failure could threaten personal injury, death or catastrophic loss (real
  avionics, navigation, medical or life-support systems, for example), or in a way that would make them subject to
  an open-source licence.
- Comply with the U.S. and international export laws that apply to them.
- NVIDIA provides them as is, without warranties of any kind. To the maximum extent the law allows, NVIDIA and its
  affiliates aren't liable for any damages arising from them.

NVIDIA's DLSS neural rendering runtime (`nvngx_dlssnr.dll`) is not part of NVIDIA's DLSS SDK and is in no release of
Sim-QVFR nor in this repository: DLSS-NR in Le Mans Ultimate runs only with a copy the player already has (a game
that ships it), loaded from where it is through NVIDIA's NGX.

NVIDIA, RTX and DLSS are trademarks and/or registered trademarks of NVIDIA Corporation in the U.S. and other
countries. Sim-QVFR is not sponsored or endorsed by NVIDIA.

Versions and upstream commits are in `third_party/README.md`. The repository contains no Assetto
Corsa EVO executable, assets or memory dumps. Original project code is licensed under PolyForm
Noncommercial 1.0.0; see `LICENSE.md`. The third-party licences above remain applicable to their
files and are not replaced by the project's licence.
