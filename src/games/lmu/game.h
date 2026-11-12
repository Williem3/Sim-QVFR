// Le Mans Ultimate's identity (../game.h). Known by its identity alone so far: the layer only logs there
// (docs/recon-lmu.md). It renders with D3D11 and reaches OpenXR on its own (+XR, a Khronos loader linked into the exe).
#pragma once

#include <iterator>

#include "../game.h"

namespace lmu {

// 1.4.2.0 (Core\Version.txt 1.4200), Steam build of 2026-10-23.
inline constexpr const char* kBuilds[] = {"66942337813b301b87d12edfa330f33354a2a4b8cd9b048885cbf77eb6f940cf"};

// Steam starts LMU through EasyAntiCheat's bootstrapper. Sim-QVFR starts the exe itself (offline sessions only), and the
// layer only logs while either of these runs, unless the user chose to run with EAC ([launcher] anticheat=1): then the
// launcher starts the bootstrapper and the layer runs as usual.
inline constexpr const wchar_t* kAntiCheat[] = {L"start_protected_game.exe", L"EasyAntiCheat_EOS.exe", nullptr};

inline constexpr simqvfr::games::GameInfo kGame = {
    "lmu", L"Le Mans Ultimate", L"LMU", L"Le Mans Ultimate.exe", L"Le Mans Ultimate", L"", L"2399420",
    L"+XR",   // its own OpenXR (Steam's "openxr" launch option); +VR is OpenVR
    nullptr,  // OpenXR of its own
    nullptr,  // no DLSS (FSR 1 only)
    // The helmet overlay (the layer's, over the composed views: crop-aware), the FOV crop (through the located FOV) and
    // NVIDIA multi-view rendering (mvr_pass.h), runtime-composed quad views on a Pimax or Varjo runtime, sharpening, and
    // Sim-QVFR's own DLSS on its views (engine/dlss_views.h; with multi-view).
    {false, true, false, true, false, false, true, true, true, true},
    kBuilds, std::size(kBuilds),
    false,
    nullptr,  // crash reports: BugSplat's, not looked at yet
    kAntiCheat,
    L"start_protected_game.exe",
};

}  // namespace lmu
