// qvfr_frame.h's internals, shared by its files (detail: not for other code). The state is the render thread's unless
// its comment says otherwise (liveMutex), defined once in the file that owns it. Nothing here runs per draw: the passes'
// bounds (pass_begin, pass_end) and the menus' VR UI panel draws (panel_draw) come in through mvr_pass.h's function
// pointers, the rest per pass or per frame.
//   qvfr_frame.cpp    the frame: eye images, focus matrices, the atlas, live resolution, eye off, multi-view's install
//                     and switching, the passes' begin and end, the frame capture, the frame's end, the public calls
//   pass_costs.cpp    what the passes cost: CPU clocks, GPU spans, the A/B runs' sides
//   native_menus.cpp  the menus at native resolution (frames without the eye loop, the VR UI panel drawn again)
#pragma once

#include "qvfr_frame.h"

#include <windows.h>
#include <d3d11.h>

#include <atomic>
#include <cstdint>
#include <mutex>

#include "../../log.h"

namespace lmu::qvfr_frame_detail {

// ---- qvfr_frame.cpp ----
extern QvfrFrameRun cfg;
extern bool qvfrMode;
extern std::mutex liveMutex;
extern double focusFraction;
extern double focusFractionY;
extern float gazeTan[2][2];
extern bool gazeValid[2];
extern int passesThisFrame;
extern unsigned long long frames;
extern simqvfr::Views4Frame latestRecord;
extern bool haveRecord;
ID3D11Texture2D* game_eye_image(int e);
float* slot_matrix(int eye, int kind);
DXGI_FORMAT typeless_of(DXGI_FORMAT f);
void real_recommended(std::uint32_t (&out)[2]);

// ---- pass_costs.cpp ----
double now_ms();
unsigned long long thread_cycles();
extern double tscPerMs;

struct CostWindow {
    double passWallMs[4] = {}, passCpuCycles[4] = {}, hookCycles = 0;
    double gpuMs[4] = {};  // spans: eyes set aside, focus 0 copied, focus 1 copied + eyes back, atlas filled
    unsigned frames = 0, gpuFrames = 0;
};
extern CostWindow costWindow;

// GPU spans of one frame: a disjoint query around timestamp pairs, a ring of frames read back when done.
struct GpuSpans {
    static constexpr int kSlots = 4, kSpans = 4;
    ID3D11DeviceContext* context = nullptr;
    ID3D11Query* disjoint[kSlots] = {};
    ID3D11Query* stamp[kSlots][kSpans][2] = {};
    bool used[kSlots][kSpans] = {}, pending[kSlots] = {};
    int slot = 0;
    bool open = false, failed = false;

    bool ready(ID3D11Texture2D* any) {
        if (context)
            return true;
        if (failed || !any)
            return false;
        ID3D11Device* device = nullptr;
        any->GetDevice(&device);
        D3D11_QUERY_DESC dj{D3D11_QUERY_TIMESTAMP_DISJOINT, 0}, ts{D3D11_QUERY_TIMESTAMP, 0};
        bool ok = true;
        for (int s = 0; s < kSlots; ++s) {
            ok = ok && SUCCEEDED(device->CreateQuery(&dj, &disjoint[s]));
            for (int k = 0; k < kSpans; ++k)
                for (int w = 0; w < 2; ++w)
                    ok = ok && SUCCEEDED(device->CreateQuery(&ts, &stamp[s][k][w]));
        }
        if (ok)
            device->GetImmediateContext(&context);
        device->Release();
        failed = !ok;
        if (!ok)
            simqvfr::log_line("lmu qvfr cost: GPU timestamp queries unavailable: copies not timed");
        return ok;
    }
    void collect(int s) {
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj{};
        if (context->GetData(disjoint[s], &dj, sizeof(dj), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK)
            return;  // not done yet (or lost when the slot is reused)
        pending[s] = false;
        if (dj.Disjoint || dj.Frequency == 0)
            return;
        double ms[kSpans] = {};
        for (int k = 0; k < kSpans; ++k) {
            UINT64 b = 0, e = 0;
            if (!used[s][k])
                continue;
            if (context->GetData(stamp[s][k][0], &b, sizeof(b), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK ||
                context->GetData(stamp[s][k][1], &e, sizeof(e), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK || e < b)
                return;
            ms[k] = static_cast<double>(e - b) * 1000.0 / static_cast<double>(dj.Frequency);
        }
        for (int k = 0; k < kSpans; ++k)
            costWindow.gpuMs[k] += ms[k];
        ++costWindow.gpuFrames;
    }
    void begin_frame() {
        if (open)
            end_frame();
        for (int s = 0; s < kSlots; ++s)
            if (pending[s])
                collect(s);
        slot = (slot + 1) % kSlots;
        pending[slot] = false;
        for (bool& u : used[slot])
            u = false;
        context->Begin(disjoint[slot]);
        open = true;
    }
    void mark(int span, int which) {
        if (!open)
            return;
        context->End(stamp[slot][span][which]);
        used[slot][span] = true;
    }
    void end_frame() {
        if (!open)
            return;
        context->End(disjoint[slot]);
        open = false;
        pending[slot] = true;
    }
};
extern GpuSpans gpuSpans;

extern int backendSide;
extern bool backendAbOk, backendAbLogged;
extern int carSide;
extern bool carAbLogged, carsOn;
extern int maskSide;
extern bool maskAbLogged;
bool car_ab_on();
extern std::atomic<bool> carsWanted;
extern bool carsPaused;
int ab_side();
bool ab_run();

// An A/B run (QvfrFrameRun::mvrAb): each side's CPU per pass since the last cost line, in thread cycles.
struct AbWindow {
    double cpu[2][4] = {};  // [0] with MVR, [1] without
    unsigned frames[2] = {};
};
extern AbWindow ab;

// ---- native_menus.cpp ----
extern bool menuNative;
extern double layerShare[2];
extern ID3D11Texture2D* menuAtlas;
extern unsigned menuW, menuH;
extern bool menuReady;
extern float menuUv[4][4];
extern unsigned long long menuFrames;
void panel_draw(ID3D11DeviceContext* context, bool after, unsigned count, unsigned start);
void menu_finish();

}  // namespace lmu::qvfr_frame_detail
