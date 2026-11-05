// Sim-QVFR launcher: what one launch of Assetto Corsa EVO needs, kept free of process and
// registry calls so it can be tested offline.
//
// The layer is loaded per launch as an explicit OpenXR layer (XR_API_LAYER_PATH points at a run
// folder holding its manifest, XR_ENABLE_API_LAYERS names it). The loader puts implicit layers
// above explicit ones, so an installed layer that changes what ACE sees (OpenXR Toolkit's FOV,
// upscaling or foveation) or one that must see the composed frame (VectorXR frame generation)
// would sit between ACE and this layer. Every enabled implicit layer that the loader lets us
// silence (its manifest names a disable_environment) is therefore loaded as an explicit copy
// below this layer, and its implicit copy is silenced for this launch. Their order and which are on
// are the launcher's layer profile (layer_order.h; default: registry order, as Windows has them on);
// a layer switched off in Windows can be loaded for ACE alone the same way.
#pragma once

#include <string>
#include <utility>
#include <vector>

#include "json.h"

namespace simqvfr::launcher {

inline constexpr char kLayerName[] = "XR_APILAYER_SIM_qvfr";
inline constexpr char kFrameGenerationLayer[] = "XR_APILAYER_DIENERTECH_VECTORXR";
inline constexpr char kCheekyLayer[] = "XR_APILAYER_CHEEKY_foveated_dlss";

// One implicit layer registration (HKLM then HKCU, the loader's order).
struct Registration {
    std::wstring manifest;
    bool enabled = true;  // registry DWORD 0
};

// A layer this launch needs on or off whatever the profile says, and why (layer_order.h's launch_options).
struct ForcedLayer {
    std::string name;
    bool on = false;
    std::string why;
};

struct Options {
    std::vector<std::string> offLayers;  // layer names silenced for this launch (ini: off_layers)
    std::vector<std::string> onLayers;   // switched off in Windows, loaded for ACE all the same (ini: on_layers)
    std::vector<std::string> order;      // the order below the Sim-QVFR layer (ini: layer_order); unlisted layers
                                         // follow the listed ones, in registry order
    bool frameGenerationOff = false;     // mode=quad (two ACE frames per headset frame): VectorXR off
    std::vector<ForcedLayer> forced;     // beats offLayers and onLayers
    std::vector<std::wstring> setVariables;  // environment variables set in the launcher's environment
};

struct MovedLayer {
    std::string name;
    std::wstring library;     // absolute
    std::wstring disableVar;  // silences the implicit copy (empty: switched off in Windows, nothing to silence)
    json::Value manifest;     // the explicit copy: absolute library_path, no enable/disable switches
};

// Every layer the launcher can place below the Sim-QVFR layer, on or off for this launch (the layers pane's rows).
struct PlannedLayer {
    std::string name;
    std::wstring manifest;
    bool on = false;
    bool systemOn = true;  // switched on in Windows (the profile's default)
    std::string lock;      // why this launch fixes its on/off (empty: the profile decides)
};

struct LayerPlan {
    std::vector<MovedLayer> below;                          // loaded below the Sim-QVFR layer, in this order
    std::vector<std::pair<std::string, std::wstring>> off;  // silenced: name, its disable variable
    std::vector<std::string> notes;                         // one line per layer left alone, and why
    std::vector<PlannedLayer> layers;                       // below's and every movable layer off, in profile order
    std::vector<std::string> above;  // on in Windows without a disable_environment: they stay above the Sim-QVFR layer
};

// Reads each registered manifest and decides where its layer goes.
LayerPlan plan_layers(const std::vector<Registration>& registrations, const Options& options);

// Why the OpenXR loader would refuse `manifest` as an explicit layer (empty: it accepts it). Such a layer is
// left out of a launch: a name in XR_ENABLE_API_LAYERS without a valid manifest fails xrCreateInstance.
std::string loader_rejects(const json::Value& manifest);

// The explicit copy of a manifest: library_path made absolute (relative paths resolve against the
// manifest's folder), enable/disable switches removed. False if it is not a single-layer manifest.
bool explicit_copy(const json::Value& manifest, const std::wstring& manifestPath, json::Value& copy,
                   std::wstring& library);

// Writes the run folder: this layer's explicit manifest (pointing at `layerLibrary`) and each moved
// layer's copy, as <name>.json. False with `error` set on failure.
bool write_run_folder(const std::wstring& folder, const std::wstring& layerManifest, const std::wstring& layerLibrary,
                      const LayerPlan& plan, std::wstring& error);

// The variables this launch sets (name, value). `runtimeInfo`: where the layer reports the headset's
// recommended size and eye tracking for the launcher window (SIM_QVFR_RUNTIME_INFO). `settings`: the
// launcher's own ini, where the in-headset menu saves its changes (SIM_QVFR_SETTINGS; empty: not set).
std::vector<std::pair<std::wstring, std::wstring>> launch_variables(const LayerPlan& plan, const std::wstring& runFolder,
                                                                    const std::wstring& ini, const std::wstring& log,
                                                                    const std::wstring& runtimeInfo,
                                                                    const std::wstring& settings = {});

// A CreateProcess environment block: `current` ("NAME=value" strings) without inherited SIM_QVFR_*
// variables, with `set` applied (case-insensitive names), sorted, double-NUL terminated.
std::wstring environment_block(const std::vector<std::wstring>& current,
                               const std::vector<std::pair<std::wstring, std::wstring>>& set);

// Steam library folders from libraryfolders.vdf ("path" entries, unescaped).
std::vector<std::wstring> steam_libraries(const std::string& vdf);

std::wstring widen(const std::string& utf8);
std::string narrow(const std::wstring& wide);
bool read_file(const std::wstring& path, std::string& out);
bool write_file(const std::wstring& path, const std::string& data);

}  // namespace simqvfr::launcher
