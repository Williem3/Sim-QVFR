// Multi-view rendering for this run (multi_view.h).
#include "multi_view.h"

#include <windows.h>

#include "../engine/nvapi_gpus.h"

namespace simqvfr::games {

bool multi_view_on(const GameInfo* game, const wchar_t* ini, std::string* why) {
    const auto say = [&](const std::string& text, bool on) {
        if (why)
            *why = text;
        return on;
    };
    if (!ini || GetPrivateProfileIntW(L"experiment", L"multi_view", 1, ini) == 0)  // on unless set to 0
        return say("off (multi_view=0)", false);
    if (!game || !game->features.multiView)
        return say("off: not for this game", false);
    const bool software = GetPrivateProfileIntW(L"experiment", L"multi_view_software", 0, ini) != 0;
    if (const nvapi::Gpu* gpu = nvapi::two_views_gpu())
        return say((gpu->name.empty() ? std::string("an NVIDIA GPU") : gpu->name) +
                       (software ? ": Sim-QVFR's own (instancing; multi_view_software=1)" : ": NVIDIA's single-pass stereo"),
                   true);
    std::string names;
    for (const nvapi::Gpu& gpu : nvapi::gpus())
        names += (names.empty() ? "" : ", ") + (gpu.name.empty() ? std::string("unnamed") : gpu.name);
    return say(std::string("Sim-QVFR's own (instancing)") +
                   (names.empty() ? "" : "; no NVIDIA single-pass stereo with full positions on " + names),
               true);
}

unsigned lmu_mvr_gate(const wchar_t* ini) {
    const unsigned gate = ini ? GetPrivateProfileIntW(L"experiment", L"lmu_mvr", 0, ini) : 0;
    return gate && nvapi::two_views_gpu() ? gate : 0;
}

}  // namespace simqvfr::games
