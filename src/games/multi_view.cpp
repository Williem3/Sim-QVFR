// NVIDIA multi-view rendering for this run (multi_view.h).
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
    if (!ini || GetPrivateProfileIntW(L"experiment", L"multi_view", 0, ini) == 0)
        return say("off (multi_view=0)", false);
    if (!game || !game->features.multiView)
        return say("off: not for this game", false);
    if (const nvapi::Gpu* gpu = nvapi::two_views_gpu())
        return say(gpu->name.empty() ? std::string("an NVIDIA GPU") : gpu->name, true);
    const std::vector<nvapi::Gpu>& found = nvapi::gpus();
    if (found.empty())
        return say("off: no NVIDIA GPU or driver", false);
    std::string names;
    for (const nvapi::Gpu& gpu : found)
        names += (names.empty() ? "" : ", ") + (gpu.name.empty() ? std::string("unnamed") : gpu.name);
    return say("off: it needs an NVIDIA RTX 20 series or newer (found " + names + ")", false);
}

}  // namespace simqvfr::games
