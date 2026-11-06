// The layers below Sim-QVFR, loaded and linked by the layer itself instead of the OpenXR loader.
//
// Why: the launcher normally names every layer in XR_ENABLE_API_LAYERS and lets the game's loader chain them.
// The loader built into OpenComposite (ACC) has an old bug splitting that list: it takes each name's end
// position as its length, so only the first and last names survive, the middle ones are "not found" and
// xrCreateInstance fails (the game starts flat). For such a game the launcher names only this layer and
// lists the rest in SIM_QVFR_CHAIN (';'-separated, nearest the game first); their manifests are in the run
// folder (XR_API_LAYER_PATH) as <name>.json. The layer loads each one as the loader would (LoadLibrary, the
// negotiate entry point) and links them between itself and what the loader put below it.
//
// What the loader no longer does for them: list their instance extensions to the game (and accept the game
// asking for them). ACC asks for none of theirs.
#pragma once

#include <string>
#include <vector>

#include "xr.h"

namespace simqvfr::chain {

inline constexpr wchar_t kChainVariable[] = L"SIM_QVFR_CHAIN";

// One layer as the loader would hold it after negotiation.
struct Layer {
    std::string name;
    PFN_xrGetInstanceProcAddr getInstanceProcAddr = nullptr;
    PFN_xrCreateApiLayerInstance createApiLayerInstance = nullptr;
};

// The names in a SIM_QVFR_CHAIN value, in order, empty entries skipped.
std::vector<std::string> split_names(const std::string& value);

// Loads the layer `name` from the manifest <folder>\<name>.json: its library (absolute, or relative to the
// manifest), its negotiate entry point (the manifest's "functions" may rename it), negotiation as the loader
// does it. False with `why` set if any step fails; the caller skips that layer, as the loader would.
bool load(const std::wstring& folder, const std::string& name, Layer& out, std::string& why);

// The nodes that put `layers` between this layer and `below` (the next-info the loader gave this layer:
// its own node, whose functions are the next layer's). Built in `storage`, which must outlive the
// xrCreateApiLayerInstance call down the chain. Returns this layer's new node: call its
// nextCreateApiLayerInstance with nextInfo = its next, and keep its nextGetInstanceProcAddr for dispatch.
// With no layers it returns `self` unchanged.
const XrApiLayerNextInfo* link(const std::vector<Layer>& layers, const XrApiLayerNextInfo* self,
                               std::vector<XrApiLayerNextInfo>& storage);

}  // namespace simqvfr::chain
