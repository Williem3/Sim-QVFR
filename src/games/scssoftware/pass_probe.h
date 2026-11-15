// ATS's eye passes found from the D3D11 calls alone, log only ([experiment] scs_pass_probe, default 1): the first step
// of its API-level QVFR (docs/recon-ats.md, local "API-level QVFR design"). Nothing the game draws changes.
//
// What the capture census showed (docs/recon-ats.md, local "Capture census at Scaling 100%"): after the shared work (shadows,
// probes, a deferred reflection renderer at 512x512 / 512x1024), each eye binds a G-buffer of 4 render targets with a
// depth target at the eye's size, then draws its AO at half size, light volumes and the forward pass; the eyes run back
// to back; post-processing for both starts at a quarter-size target (luminance) after the second eye. So:
//   - a G-buffer bind: 4+ render targets, a depth target, at least 1024x1024 (the probes' are 512 wide), sized like
//     the largest such bind seen (the eye size, learned);
//   - an eye pass runs from its G-buffer bind to the next G-buffer bind with other targets, to the first bind of a
//     target under a third of the eye's width (the post's quarter size), or to the frame's end;
//   - the frame's end is xrEndFrame (any thread), taken at the next bind on the game's render thread;
//   - the render thread is the one that binds the first G-buffer (other threads bind too: 2026-11-14 14:31 run, where
//     following the first thread to bind anything found no passes at all).
// Per frame: each segment's wall time on the render thread and draws (shared work, each pass, post), each pass's end
// reason; a detailed line for the first frames with passes, a summary every 900 frames.
#pragma once

#include <cstdint>
#include <string>

struct ID3D11Device;

namespace scs {

// The bookkeeping, without D3D11 (tests drive it with plain sizes and times).
class PassTracker {
public:
    enum class End { none, nextGbuffer, post, frameEnd };
    static const char* end_name(End end);

    struct Bind {
        unsigned targets = 0;          // render targets bound
        bool depth = false;            // a depth target bound
        unsigned width = 0, height = 0;  // render target 0's size (0 if none)
        const void* first = nullptr;   // render target 0's view (identity only)
    };
    struct Segment {
        double ms = 0;
        uint64_t draws = 0, instanced = 0;
    };
    static constexpr int kMaxPasses = 4;
    struct Frame {
        Segment shared, pass[kMaxPasses], post;
        End end[kMaxPasses] = {};
        int passes = 0;
        unsigned eyeWidth = 0, eyeHeight = 0;
    };

    // A G-buffer's shape: 4+ render targets with a depth target, at least 1024x1024 (the probes' are 512 wide). The
    // thread that binds the first one is the render thread the probe follows.
    static bool gbuffer_shape(const Bind& b) { return b.targets >= 4 && b.depth && b.width >= 1024 && b.height >= 1024; }
    // A render-target bind at `seconds` (the render thread's clock).
    void bind(const Bind& b, double seconds);
    void draw(bool instanced);
    // The frame ended (xrEndFrame seen): closes the open segment at `seconds` and returns the frame. The next frame's
    // shared work starts here.
    Frame end_frame(double seconds);
    unsigned eye_width() const { return eyeWidth_; }
    unsigned eye_height() const { return eyeHeight_; }

private:
    enum class Phase { shared, pass, post };
    Segment& current();
    void close_pass(End why, double seconds);

    Frame frame_{};
    Phase phase_ = Phase::shared;
    double segmentStart_ = -1;  // seconds; -1 until the first bind
    const void* passTarget_ = nullptr;
    unsigned eyeWidth_ = 0, eyeHeight_ = 0;
};

// Sums of frames for the periodic summary.
struct PassSummary {
    uint64_t frames = 0, byPasses[PassTracker::kMaxPasses + 1] = {};
    // Frames with exactly two passes (the stereo frames):
    uint64_t stereo = 0;
    double sharedMs = 0, passMs[2] = {}, postMs = 0, maxPassMs[2] = {};
    double sharedDraws = 0, passDraws[2] = {}, postDraws = 0, passInstanced[2] = {};
    uint64_t ends[2][4] = {};  // per pass, by PassTracker::End
    void add(const PassTracker::Frame& f);
    std::string line() const;  // "" with no frames
};

// xrCreateSession with the game's D3D11 device: the immediate context observed (engine/context_hooks.h), from the
// session's first xrEndFrame (the install issues commands on the context: done by the thread that ends the frames).
// False: no device.
bool pass_probe_install(ID3D11Device* device);
// Every xrEndFrame, on the thread that ends the frames: a session start or end asked for is done here first.
void pass_probe_frame_end();
// xrDestroySession (the game's main thread): observing stops at once, the rest at the next xrEndFrame.
void pass_probe_session_end();

}  // namespace scs
