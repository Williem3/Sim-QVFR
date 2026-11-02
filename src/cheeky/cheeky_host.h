// Cheeky Foveated DLSS, hosted by the layer ([cheeky] in ace-quadview.ini). Cheeky's processing
// lives in its resident runtime, CheekyFoveatedDLSSRuntime.dll (GPL v3, built and shipped separately
// from Cheeky's repository), behind a versioned C ABI: Start / Tick / Command / Snapshot / Detach.
// Cheeky's own dxgi.dll loader and desktop overlay are one host of that ABI; this layer is another,
// so Cheeky runs inside ACE without its loader: the layer starts the runtime at
// xrCreateApiLayerInstance (seconds before ACE initialises NGX), hands it ACE's D3D12 device and
// queue from the session's graphics binding and ticks it at every xrEndFrame.
//
// Spike: Cheeky's settings come from CheekyFoveatedDLSS.ini in `config_dir` (Cheeky writes it), and
// the layer logs, every `report_s` seconds, each DLSS view Cheeky sees with its sizes and evaluations
// per frame, and writes the full snapshot to logs/cheeky-snapshot.json.
#pragma once

#include <string>

struct ID3D12Device;
struct ID3D12CommandQueue;

namespace aceqv {

struct CheekyConfig {
    bool enabled = false;
    std::wstring runtime;    // CheekyFoveatedDLSSRuntime.dll, absolute
    std::wstring configDir;  // where Cheeky keeps CheekyFoveatedDLSS.ini and its log, absolute
    unsigned reportSeconds = 10;
};

// [cheeky] in `ini`: enabled, runtime and config_dir (relative paths are against `layerDir`;
// default <layerDir>\CheekyFoveatedDLSS\CheekyFoveatedDLSSRuntime.dll and <layerDir>\CheekyFoveatedDLSS),
// report_s.
CheekyConfig cheeky_config(const wchar_t* ini, const std::wstring& layerDir);

// Loads the runtime and starts it with no graphics yet. False (logged) when disabled, missing or
// refused; Cheeky then stays off for this ACE run.
bool cheeky_start(const CheekyConfig& config);
bool cheeky_running();
// The session's D3D12 device and queue (xrCreateSession), or none (xrDestroySession: processing
// pauses until the next session).
void cheeky_session(ID3D12Device* device, ID3D12CommandQueue* queue);
// Every xrEndFrame: Cheeky's frame callback, and the periodic view report.
void cheeky_frame();
// Applies "Name=value" lines to Cheeky's settings at once (CheekyRuntime_Command "set").
bool cheeky_set(const std::string& lines);
// Cheeky's snapshot JSON (empty on failure).
std::string cheeky_snapshot();

}  // namespace aceqv
