// The showcase: a live switch between Stock and QVFR in one session ([quad] stock, live; GameFeatures::showcase), for
// recording the same corner both ways, and the overlay's A/B table ([osd] ab_table) of each side's numbers.
//
// Stock: the game's two eyes at the native size, no focus views drawn, no composition, the frame submitted as the game
// made it (each game's path, below the layer's hooks, which stay installed). The layer sets the wish (stock_wanted);
// the game takes it at a frame boundary and says when its frames are stock (stock_active), which the badge and the
// table go by. A game that can't (ACE launched below the native size) leaves stock_active off and says why in the log.
// Pure: no runtime, device or file.
#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

#include "osd.h"

namespace simqvfr {

// The state below is the process's one copy, in the core (quad/shared_state.cpp), never in a header: a game mod's own
// DLL reaches it through the host table (mod_api.h), so the layer and the game see the same switch.

// The wish, as the layer applied it last (live_apply), read from any thread.
bool stock_wanted();
void set_stock_wanted(bool on);

// What the game renders: set by the game at the frame boundary where it took the wish.
bool stock_active();
void set_stock_active(bool on);

// Frames that drew the track (the game's scene, either side), counted by the game at its frame's end: the A/B table
// leaves out the rest (menus, loading, pause), which would otherwise skew whichever side was showing.
void note_scene_frame();
uint64_t scene_frames();

// ---- The A/B table ---------------------------------------------------------------------------------------------------

// Windows of frames within this long of a switch (or of the table's start, or of the track showing again after a menu
// or loading) are left out: the first frames on the new side carry the old one's history (DLSS, the game's TAA and
// exposure) and sizes.
inline constexpr int64_t kAbSettleMs = 1500;
// Frame times binned (for the 1% low without keeping every frame): kAbBinMs wide up to kAbBins * kAbBinMs, slower
// frames in the last bin.
inline constexpr double kAbBinMs = 0.05;
inline constexpr int kAbBins = 4000;
inline constexpr double kAbSceneShare = 0.9;

struct AbSide {
    uint64_t frames = 0;
    double frameMsSum = 0;
    std::array<uint32_t, kAbBins> bins{};
    double cpuMsSum = 0, gpuMsSum = 0;  // per frame times the window's frames
    uint64_t cpuFrames = 0, gpuFrames = 0;
};

struct AbStats {
    bool valid = false;  // frames sampled
    double seconds = 0, fps = 0, low1Fps = 0;
    double cpuMs = -1, gpuMs = -1;  // -1: unknown
};
AbStats ab_stats(const AbSide& side);

class AbTable {
public:
    void reset();
    // One overlay window: its frames' intervals (ms), its CPU and GPU time per frame (-1: unknown), at `nowMs`, on the
    // side the game rendered (stock_active); `sceneFrames` of its frames drew the track. Left out within kAbSettleMs of
    // a switch, of the reset or of the track showing again; a window with fewer than kAbSceneShare of its frames on the
    // track is left out and starts the settle again.
    void add(bool stock, const std::vector<double>& frameMs, double cpuMs, double gpuMs, int64_t nowMs,
             uint64_t sceneFrames);
    const AbSide& side(bool stock) const { return sides_[stock ? 0 : 1]; }
    bool settling(int64_t nowMs) const { return last_ < 0 || nowMs < settleUntil_; }

private:
    std::array<AbSide, 2> sides_{};  // stock, QVFR
    int last_ = -1;                  // the side of the last window (-1: none since the reset)
    int64_t settleUntil_ = 0;
};

// What the pixel rows say, from the headset menu's model (menu_stats): both eyes' pixels at the native size and as QVFR
// renders them, pixels per degree at the centre (native, the focus views') and in the periphery. 0: unknown.
struct AbPixels {
    double stockPixels = 0, qvfrPixels = 0;
    double stockPpd = 0, focusPpd = 0, peripheryPpd = 0;
};

// A row of the table: its label, each side's value and the difference (QVFR against Stock), coloured by whether QVFR
// is better (Good), worse (Warn) or it's the trade (Neutral: the periphery's sharpness).
struct AbLine {
    std::wstring label, stock, qvfr, delta;
    OsdTone tone = OsdTone::Neutral;
};
std::vector<AbLine> ab_lines(const AbStats& stock, const AbStats& qvfr, const AbPixels& pixels);

// The card's heading: which side shows now and how long each was sampled.
struct AbHeading {
    bool stockNow = false, settling = false;
    std::wstring qvfrName = L"QVFR";  // "QVFR · MVR": the badge's way
    double stockSeconds = 0, qvfrSeconds = 0;
};

// RGBA8 pixels (sRGB, premultiplied alpha), kOsdWidth x kOsdHeight: the overlay's card as the A/B table.
std::vector<uint32_t> draw_ab_panel(const AbHeading& heading, const std::vector<AbLine>& lines);

}  // namespace simqvfr
