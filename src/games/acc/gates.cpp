#include "gates.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include "../../log.h"
#include "../../probe/d3d11_capture.h"
#include "../../probe/run_exit.h"
#include "gate_compare.h"
#include "views.h"

namespace acc {
namespace {

GateRun cfg;
bool active = false;
bool armed = false;          // the samples' count started (the hotkey, or the first four-view frame)
bool keyHeld = false;
unsigned long long armedAt = 0;  // views_rendered_frames() then
int taken = 0;
bool finished = false;
unsigned long long endFrames = 0;
bool savedOut[2] = {};  // each eye's first out-of-range sample saved (besides the first sample)
// The images of a sample: the first one, and each eye's first out of range.
bool save_now(int eye, bool within) {
    if (taken == 0)
        return true;
    if (within || savedOut[eye])
        return false;
    savedOut[eye] = true;
    return true;
}
// Per sample and eye: copies, the share of 8x8 blocks off and the shift; focus, the flat blocks' median and the
// offset.
struct Sample {
    double value[2] = {};
    int dx[2] = {}, dy[2] = {};
    bool valid[2] = {};
    bool within[2] = {};  // copies: CopiesResult::within
    double edge[2] = {}, middle[2] = {};  // focus: the edge ring and the middle; copies: edge = the worst band's gain
    int darker[2] = {}, brighter[2] = {};  // focus: the largest light/shadow flips
    int state = 0;                         // the contact shadow A/B's state (GateRun::abContact)
};
std::vector<Sample> samples;

// The contact shadow A/B (gate_ab_contact=1): the samples take turns at contact shadows held off (the default fix) and
// on (the game's own), each state's flips summed up at the end (docs/recon-acc.md "The dash shadow mismatch").
constexpr int kAbStates = 2;
const char* const kAbLabels[kAbStates] = {"contact shadows off", "contact shadows ON (the game's own)"};
int abState = 0;
void set_ab_state(int state) {
    abState = state;
    views_set_diag(3, state == 0);  // kDiagCvars[3]: r.ContactShadows
    simqvfr::logf("acc focus: the A/B's next sample with %s", kAbLabels[state]);
}
std::string stateBuffer;
const char* state_label() {
    if (!cfg.abContact)
        return "";
    stateBuffer = std::string(" (") + kAbLabels[abState] + ")";
    return stateBuffer.c_str();
}

const char* tag() { return cfg.copies ? "acc copies" : "acc focus"; }

std::wstring captures_folder() {
    const std::filesystem::path dir = std::filesystem::path(simqvfr::log_directory()) / L"captures";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    return dir.wstring();
}

// A rect of the image as its own picture.
std::vector<std::uint8_t> crop(const GateImage& img, const GateRect& r) {
    std::vector<std::uint8_t> out(static_cast<std::size_t>(r.w) * r.h * 4);
    for (int y = 0; y < r.h; ++y)
        std::memcpy(out.data() + static_cast<std::size_t>(y) * r.w * 4, img.at(r.x, r.y + y), static_cast<std::size_t>(r.w) * 4);
    return out;
}

void save(const GateImage& img, const GateRect& r, const wchar_t* what, int eye) {
    wchar_t name[96];
    swprintf_s(name, L"\\%ls-frame%llu-%ls%d.png", cfg.copies ? L"acc-copies" : L"acc-focus", endFrames, what, eye);
    const std::wstring path = captures_folder() + name;
    simqvfr::write_png_bgra(path, crop(img, r), static_cast<unsigned>(r.w), static_cast<unsigned>(r.h));
}

void sample_copies(const GateImage& img, const GateRect (&views)[4], Sample& s) {
    for (int eye = 0; eye < 2; ++eye) {
        const GateRect& a = views[eye];
        const GateRect& b = views[eye + 2];
        if (a.w != b.w || a.h != b.h) {
            simqvfr::logf("acc copies: frame %llu: eye %d is %dx%d, its copy %dx%d: not the copies layout; not compared",
                          endFrames, eye, a.w, a.h, b.w, b.h);
            continue;
        }
        const CopiesResult r = compare_copies(img, a, b);
        if (!r.valid) {
            simqvfr::logf("acc copies: frame %llu: eye %d: rects outside the %dx%d image; not compared", endFrames, eye,
                          img.width, img.height);
            continue;
        }
        s.valid[eye] = true;
        s.value[eye] = r.block_share();
        s.dx[eye] = r.dx;
        s.dy[eye] = r.dy;
        s.within[eye] = r.within();
        s.edge[eye] = r.worst_band();
        char bands[256] = {};
        int n = 0;
        for (int i = 0; i < 8; ++i)
            if (r.bandCount[i])
                n += std::snprintf(bands + n, sizeof(bands) - n, "%s%d-%d %.4f (%.1f%%)", n ? ", " : "", i * 32, i * 32 + 31,
                                   r.bandRatio[i], 100.0 * static_cast<double>(r.bandCount[i]) / static_cast<double>(r.pixels));
        if (r.differing == 0 && r.dx == 0 && r.dy == 0)
            simqvfr::logf("acc copies: frame %llu: view %d equals eye %d pixel for pixel (%dx%d at %d,%d and %d,%d)", endFrames,
                          eye + 2, eye, a.w, a.h, a.x, a.y, b.x, b.y);
        else
            simqvfr::logf("acc copies: frame %llu: view %d %s eye %d: 8x8 blocks off by more than 8 steps %.3f%% (largest "
                          "%.1f); pixels: %llu (%.3f%%) differ, %.3f%% by more than one step, %.3f%% by more than 16, max %d, "
                          "mean %.2f where different, within x %d-%d y %d-%d of %dx%d; best shift %+d,%+d; copy minus eye "
                          "summed R %+lld G %+lld B %+lld; copy / eye luminance by the eye's brightness: %s",
                          endFrames, eye + 2, r.within() ? "within range of" : "DIFFERS from", eye, r.block_share(),
                          r.blockMax, static_cast<unsigned long long>(r.differing), r.percent(), r.share(r.over1),
                          r.share(r.over16), r.maxDiff, r.meanDiff, r.differingBox.x, r.differingBox.x + r.differingBox.w - 1, r.differingBox.y,
                          r.differingBox.y + r.differingBox.h - 1, a.w, a.h, r.dx, r.dy, r.signedSum[0], r.signedSum[1],
                          r.signedSum[2], bands);
        if (save_now(eye, r.within())) {
            save(img, a, L"eye", eye);
            save(img, b, L"copy", eye);
        }
    }
}

void sample_focus(const GateImage& img, const GateRect (&views)[4], Sample& s) {
    simqvfr::Views4Frame frame{};
    if (!views_latest_frame(frame)) {
        simqvfr::logf("acc focus: frame %llu: no record of the views yet; not compared", endFrames);
        return;
    }
    for (int eye = 0; eye < 2; ++eye) {
        const XrQuaternionf& t = frame.turn[eye];
        if (std::fabs(t.w) < 0.99999f) {
            simqvfr::logf("acc focus: frame %llu: eye %d's focus camera is turned (acc_focus_turn=1): the box isn't a "
                          "rectangle of its eye; not compared", endFrames, eye);
            continue;
        }
        double box[4];
        if (!box_in_eye(frame.outerTan[eye], frame.focusTan[eye], box)) {
            simqvfr::logf("acc focus: frame %llu: eye %d: no box in the record; not compared", endFrames, eye);
            continue;
        }
        const FocusResult r = compare_focus(img, views[eye], views[eye + 2], box);
        if (!r.valid) {
            simqvfr::logf("acc focus: frame %llu: eye %d: nothing to compare (rects outside the image or a black box)",
                          endFrames, eye);
            continue;
        }
        s.valid[eye] = true;
        s.within[eye] = r.within();
        s.value[eye] = r.flat;
        s.dx[eye] = r.dx;
        s.dy[eye] = r.dy;
        s.edge[eye] = r.edge;
        s.middle[eye] = r.middle;
        s.darker[eye] = r.darkerCore;
        s.brighter[eye] = r.brighterCore;
        simqvfr::logf("acc focus: frame %llu: eye %d %s%s: flat blocks focus / eye %.3f (%d of %d blocks), edge ring %.3f, "
                      "middle %.3f, by the eye's brightness dark %.3f (%d), mid %.3f (%d), bright %.3f (%d); light/shadow "
                      "flips (focus 2x off, eye px after erosion): darker %d at x %d-%d y %d-%d, brighter %d at x %d-%d y "
                      "%d-%d of the box; detailed blocks %.3f (%d; the focus view resolves what the eye averages); all "
                      "blocks median %.3f (p10 %.3f, p90 %.3f), over the box %.3f; best offset %+d,%+d eye pixels "
                      "(difference %.2f there, %.2f at 0,0); box x %.4f-%.4f y %.4f-%.4f of the eye's %dx%d (%dx%d whole "
                      "px), focus view %dx%d",
                      endFrames, eye, r.within() ? "within range" : "OUT OF RANGE", state_label(), r.flat, r.flatBlocks,
                      r.blocks, r.edge, r.middle, r.band[0], r.bandBlocks[0], r.band[1], r.bandBlocks[1], r.band[2],
                      r.bandBlocks[2], r.darkerCore, r.darkerAt[0], r.darkerAt[2], r.darkerAt[1], r.darkerAt[3],
                      r.brighterCore, r.brighterAt[0], r.brighterAt[2], r.brighterAt[1], r.brighterAt[3], r.detailed,
                      r.detailedBlocks, r.median, r.p10, r.p90, r.overall, r.dx, r.dy, r.sad, r.sadAtZero, box[0], box[2],
                      box[1], box[3], views[eye].w, views[eye].h, r.boxW, r.boxH, views[eye + 2].w, views[eye + 2].h);
        if (save_now(eye, r.within())) {
            save(img, views[eye], L"eye", eye);
            save(img, views[eye + 2], L"focus", eye);
            simqvfr::logf("acc focus: images in %ls: python tools/focus_compare.py <eye png> <focus png> %.5f %.5f %.5f %.5f",
                          captures_folder().c_str(), box[0], box[1], box[2], box[3]);
        }
    }
}

void verdict() {
    finished = true;
    const bool copies = cfg.copies;
    int compared = 0, out = 0;
    double worst = copies ? 0.0 : 1.0, worstBand = 0;
    for (const Sample& s : samples)
        for (int eye = 0; eye < 2; ++eye) {
            if (!s.valid[eye])
                continue;
            ++compared;
            const bool shifted = std::abs(s.dx[eye]) > (copies ? 0 : 1) || std::abs(s.dy[eye]) > (copies ? 0 : 1);
            bool bad = shifted;
            if (copies) {
                bad = !s.within[eye];
                worst = (std::max)(worst, s.value[eye]);  // percent of pixels off by more than one step
                worstBand = std::fabs(s.edge[eye]) > std::fabs(worstBand) ? s.edge[eye] : worstBand;
            } else {
                bad = !s.within[eye];
                if (std::fabs(s.value[eye] - 1) > std::fabs(worst - 1))
                    worst = s.value[eye];
            }
            out += bad ? 1 : 0;
        }
    if (cfg.abContact && !copies)
        for (int st = 0; st < kAbStates; ++st) {
            int n = 0, bad = 0, darker = 0, brighter = 0;
            for (const Sample& s : samples)
                for (int eye = 0; eye < 2; ++eye)
                    if (s.valid[eye] && s.state == st) {
                        ++n;
                        bad += s.within[eye] ? 0 : 1;
                        darker = (std::max)(darker, s.darker[eye]);
                        brighter = (std::max)(brighter, s.brighter[eye]);
                    }
            simqvfr::logf("%s: A/B, %s: %d comparisons, %d out of range; largest light/shadow flips darker %d, brighter %d "
                          "eye pixels",
                          tag(), kAbLabels[st], n, bad, darker, brighter);
        }
    if (!compared)
        simqvfr::logf("%s: done: NOTHING COMPARED in %d samples (see the lines above)", tag(), taken);
    else if (copies)
        simqvfr::logf("%s: done: %d comparisons, %s (worst: %.3f%% of 8x8 blocks off by more than 8 steps, a band %+.2f%% "
                      "brighter; tolerance 0.1%%, 1%%, no shift)",
                      tag(), compared, out ? "OUT OF RANGE" : "all within range", worst, worstBand * 100);
    else
        simqvfr::logf("%s: done: %d comparisons, %s (worst flat-block median %.3f; tolerance 1.00 +- 0.02, edge ring within "
                      "0.03 of the middle, offset at most 1 eye pixel, light/shadow flips under 20 eye pixels)",
                      tag(), compared, out ? "OUT OF RANGE" : "all within range", worst);
    if (cfg.exitWhenDone)
        simqvfr::request_game_exit("the gate has its samples (exit_when_done=1)");
}

}  // namespace

GateRun gate_run(const wchar_t* ini) {
    GateRun g;
    if (!ini)
        return g;
    g.copies = GetPrivateProfileIntW(L"experiment", L"acc_copies", 0, ini) != 0;
    g.focus = !g.copies && GetPrivateProfileIntW(L"experiment", L"acc_focus", 0, ini) != 0;
    g.first = (std::max)(1, static_cast<int>(GetPrivateProfileIntW(L"experiment", L"gate_first", 300, ini)));
    g.every = (std::max)(1, static_cast<int>(GetPrivateProfileIntW(L"experiment", L"gate_every", 300, ini)));
    g.count = std::clamp(static_cast<int>(GetPrivateProfileIntW(L"experiment", L"gate_samples", 3, ini)), 1, 20);
    g.hotkey = GetPrivateProfileIntW(L"experiment", L"gate_hotkey", 1, ini) != 0;
    g.exitWhenDone = GetPrivateProfileIntW(L"experiment", L"exit_when_done", 0, ini) != 0;
    g.abContact = g.focus && GetPrivateProfileIntW(L"experiment", L"gate_ab_contact", 0, ini) != 0;
    return g;
}

void gates_start(const GateRun& run) {
    cfg = run;
    active = run.on();
    if (!active)
        return;
    simqvfr::logf("%s: gate on: %d samples, the first %d four-view frames after %s, then every %d%s%s", tag(), run.count,
                  run.first, run.hotkey ? "Ctrl+Alt+G (press it once in the cockpit)" : "the first four-view frame", run.every,
                  run.exitWhenDone ? "; the game is asked to close after the last (exit_when_done=1)" : "",
                  run.abContact ? "; an A/B of contact shadows: the samples take turns at off and on (gate_ab_contact=1)"
                                : "");
    if (run.abContact) {
        if (!views_diag_start())
            simqvfr::log_line("acc focus: the A/B can't switch contact shadows off on this build");
        set_ab_state(0);
    }
}

bool gates_on() { return active; }

void gates_image(ID3D11Texture2D* image, const int (&rects)[4][4]) {
    if (!active || finished)
        return;
    ++endFrames;
    const unsigned long long rendered = views_rendered_frames();
    if (!armed) {
        bool start = false;
        if (cfg.hotkey) {
            const auto down = [](int key) { return (GetAsyncKeyState(key) & 0x8000) != 0; };
            const bool key = down(VK_CONTROL) && down(VK_MENU) && down('G');
            start = key && !keyHeld;
            keyHeld = key;
        } else {
            start = rendered > 0;
        }
        if (!start)
            return;
        armed = true;
        armedAt = rendered;
        simqvfr::logf("%s: frame %llu: samples counted from here (%s)", tag(), endFrames,
                      cfg.hotkey ? "Ctrl+Alt+G" : "the first four-view frame");
    }
    const unsigned long long due = armedAt + static_cast<unsigned long long>(cfg.first) +
                                   static_cast<unsigned long long>(taken) * static_cast<unsigned long long>(cfg.every);
    if (rendered < due || !image)
        return;
    std::vector<std::uint8_t> px;
    unsigned w = 0, h = 0;
    if (!simqvfr::d3d11_read_bgra8(image, px, w, h)) {
        simqvfr::logf("%s: frame %llu: the game's image can't be read back (format); the gate stops", tag(), endFrames);
        finished = true;
        return;
    }
    const GateImage img{px.data(), static_cast<int>(w), static_cast<int>(h)};
    GateRect views[4];
    for (int v = 0; v < 4; ++v)
        views[v] = {rects[v][0], rects[v][1], rects[v][2], rects[v][3]};
    Sample s;
    s.state = abState;
    if (cfg.copies)
        sample_copies(img, views, s);
    else
        sample_focus(img, views, s);
    samples.push_back(s);
    ++taken;
    if (cfg.abContact && taken < cfg.count)
        set_ab_state((abState + 1) % kAbStates);  // gate_every frames to settle before the next
    if (taken >= cfg.count)
        verdict();
}

}  // namespace acc
