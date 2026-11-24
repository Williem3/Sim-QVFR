// Every global of the layer, in the order xr_layer.cpp defined them: construction and destruction follow
// this order (gazeThread and padWatch join threads in their destructors that read other globals).
#include <atomic>
#include <map>
#include <mutex>
#include <string>
#include <utility>

#include <dxgi1_4.h>

#include "games/adapter.h"
#include "input/hotkeys.h"
#include "quad/helmet.h"
#include "quad/session_graphics.h"
#include "xr.h"
#include "layer/layer.h"

namespace simqvfr::layer {

std::atomic<bool> processTerminating{false};  // constant-initialised: set from DllMain (early_load.cpp)

Dispatch next;
NativeQuad nativeQuad;

std::mutex chainMutex;

std::map<XrSwapchain, std::pair<uint32_t, uint32_t>> chainSizes;

Experiment experiment;

// The game this process is (games/adapter.h): its QVFR and its own experiments. Elsewhere one that does nothing.
games::GameAdapter* game = &games::adapter_for(nullptr);

bool focusLogged = false;

// The helmet overlay ([helmet], views4 composing; helmet.h): set at launch, then live under quad.mutex.
HelmetSettings helmetSettings;

Live live;


Gaze gaze;

HeadSpaces headSpaces;

GazeThread gazeThread;

GazeHold gazeHold;

Quad quad;

// The session's graphics API and devices (session_graphics.h), under quad.mutex: the panels and compositors draw with it.
SessionGraphics sessionGraphics;

V4D3D11 v4d3d11;

Views4Outline v4outline;

uint64_t endFrames = 0;

std::atomic<XrSwapchain> measuredEyeChain{XR_NULL_HANDLE};

uint64_t locates = 0;

CpuTimes cpu;

Pacing pacing;

SessionSummary summary;

std::atomic<uint32_t> stintGeneration{0};

std::string layersBelow;  // the layers between this one and the runtime (log_layer_order)

IDXGIAdapter3* vramAdapter = nullptr;

bool visibilityMaskLogged = false;

bool views4RectsLogged = false;

bool v4EyesOnlyLogged = false;  // views4_submit: an eyes-only frame (Views4Frame::eyesOnly) was reported

IniWorker iniWorker;

Optimize optimize;

Hotkeys hotkeys = default_hotkeys();  // the frame thread's

PadWatch padWatch;

Menu menu;

Osd osd;

Clock clockCard;

Notice notice;

bool visorHotkeyHeld = false;

bool cpuProfileHotkeyHeld = false;

bool frameCaptureHotkeyHeld = false;
int frameCensusCapture = 0;
int frameCensusBurst = 1;

ProfileNotice profileNotice;

std::atomic<int> loaderNegotiations{0};

std::atomic<bool> instanceReachedLayer{false};

}  // namespace simqvfr::layer
