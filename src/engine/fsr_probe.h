// Which AMD FSR this PC gets, asked outside the game (the launcher): AMD's own provider query (FSR API ffxQuery, its
// version list for the upscaler) on the high-performance graphics card, with AMD's runtime from Sim-QVFR's folder.
// FSR 4 where the runtime offers it on that card (AMD RDNA 4, and RDNA 3/2 with newer runtimes), else FSR 3.1. The
// game itself asks again on its own device (engine/fsr_upscale.h): this only names it for the player.
#pragma once

#include <string>

namespace simqvfr::fsr {

struct Offer {
    bool checked = false;   // the query ran (false: no FSR in this build, no card, or AMD's DLLs missing; `why` says)
    int major = 0;          // the newest provider's version, as AMD's runtime names it: 4 FSR 4, 3 FSR 3.1
    int minor = 0;
    std::wstring names;     // every provider offered, newest first as AMD lists them ("4.1.1, 3.1.5")
    std::wstring gpu;       // the card asked about
    unsigned vendor = 0;    // its PCI vendor id
    std::wstring why;       // why not checked
};

// Slow (a D3D12 device and AMD's 28 MB DLL): call off the UI thread. `folder`: where AMD's two DLLs are (the layer's).
Offer probe_offer(const std::wstring& folder);

// "FSR 4", "FSR 3.1": the newest the card gets; empty while not known.
inline std::wstring offer_name(const Offer& o) {
    if (!o.checked || o.major <= 0)
        return {};
    return o.major >= 4 ? L"FSR " + std::to_wstring(o.major) : L"FSR " + std::to_wstring(o.major) + L"." + std::to_wstring(o.minor);
}

}  // namespace simqvfr::fsr
