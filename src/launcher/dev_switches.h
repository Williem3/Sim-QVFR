// The Developer tab's switches ([launcher] developer=1 shows the tab; ui/developer.cpp): ini keys the player's cards
// leave out, each on or off, read and written with the other settings (Settings::dev, at the next launch). The
// showcase's live Stock/QVFR switch and A/B table are not here: the page drives them itself (the running game's ini,
// [osd] ab_table). Diagnostic switches exist in development builds only (dev_build.h): a release's page leaves them out.
#pragma once

#include <array>

namespace simqvfr::launcher {

enum class DevGroup { Showcase, Measure, Diagnostic };

struct DevSwitch {
    DevGroup group;
    const char* game;  // the game it is for (GameInfo::id); nullptr: every game
    const wchar_t* section;
    const wchar_t* key;
    int on;  // the value it writes when on (an A/B's frames each way); off writes 0
    const wchar_t* label;
    const wchar_t* help;
};

// English, as identities: the page shows them through tr().
inline constexpr DevSwitch kDevSwitches[] = {
    // The showcase: what a recording of Stock against QVFR wants at launch.
    {DevGroup::Showcase, "lmu", L"experiment", L"views4_menu_scale", 100, L"Launch at the native size",
     L"Stock renders at 100% (the game's eye images made at the headset's size); costs the VRAM of native-size "
     L"targets."},
    {DevGroup::Showcase, "lmu", L"experiment", L"lmu_fsr", 1, L"FSR beside DLSS",
     L"The headset menu's DLSS page gets an Upscaler row, to switch between DLSS and AMD FSR while recording."},
    // A/B measurements: N frames each way, compared in the log.
    {DevGroup::Measure, "lmu", L"experiment", L"lmu_car_vectors_ab", 900, L"Other cars' motion A/B",
     L"900 frames with the cars' motion vectors, then 900 without, in turn; the log compares them."},
    {DevGroup::Measure, "acc", L"experiment", L"acc_focus_skip_ab", 900, L"Focus views' meshes A/B",
     L"900 frames as usual, then 900 with no opaque mesh passes in the focus views, in turn; the log compares them."},
    {DevGroup::Measure, "acc", L"experiment", L"acc_gather_skip_ab", 900, L"Focus views' dynamic meshes A/B",
     L"900 frames as usual, then 900 with no dynamic meshes gathered for the focus views, in turn."},
    // Diagnostics (development builds only).
    {DevGroup::Diagnostic, "lmu", L"experiment", L"lmu_diag", 1, L"Live A/B hotkeys",
     L"Ctrl+Alt+1 the multi-view shadow fix, Ctrl+Alt+2 DLSS's depth before the glass; the log says when."},
    {DevGroup::Diagnostic, "lmu", L"experiment", L"lmu_mvr_timing", 1, L"Multi-view CPU timing",
     L"The CPU spent in the multi-view context hooks, logged every 900 frames."},
    {DevGroup::Diagnostic, "lmu", L"experiment", L"lmu_mvr_probe", 1, L"Multi-view shader probe",
     L"Every vertex shader converted and created once, then released: NVAPI's support and the counts in the log."},
    {DevGroup::Diagnostic, "acc", L"experiment", L"acc_diag", 1, L"Pass hotkeys",
     L"Ctrl+Alt+1-6 switch SSR, AO, the sun's shadows, the focus views' sharing, contact shadows and convergence."},
    {DevGroup::Diagnostic, "acc", L"experiment", L"acc_pass_probe", 1, L"Mesh pass probe",
     L"Every 900 frames, the draws and dispatch time of each view's mesh passes in the log."},
    {DevGroup::Diagnostic, "xplane", L"experiment", L"xplane_diag", 1, L"Pass hotkeys",
     L"Ctrl+Alt+1 SSR, Ctrl+Alt+2 the ambient occlusion, Ctrl+Alt+3 the sun's shadows, while flying."},
    {DevGroup::Diagnostic, "ace", L"experiment", L"d3d12_debug", 1, L"D3D12 debug layer",
     L"Direct3D's validation messages in the log; slower."},
    {DevGroup::Diagnostic, "ace", L"experiment", L"dred", 1, L"DRED breadcrumbs",
     L"A GPU hang names its last pass; costs GPU and CPU time on every command list."},
};

inline constexpr int kDevSwitchCount = static_cast<int>(sizeof(kDevSwitches) / sizeof(kDevSwitches[0]));

// Each switch's value as the ini has it (0: off; another value than DevSwitch::on is kept as it is while on).
using DevValues = std::array<int, kDevSwitchCount>;

}  // namespace simqvfr::launcher
