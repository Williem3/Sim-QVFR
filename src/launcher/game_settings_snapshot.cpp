// The game's graphics settings at a launch (game_settings_snapshot.h).
#include "game_settings_snapshot.h"

#include <cstring>
#include <vector>

#include "../text.h"
#include "lmu_game_settings.h"

namespace simqvfr::launcher {

namespace {

namespace lmu = lmu_settings;

// LMU settings the In-game settings page doesn't edit but that change what a frame costs or looks like: the render
// scale and upscaler, the VR FOV scale (QVFR refuses frames at anything but 100%), rain and particle flow, mirror size
// and placement, frame pacing, VR projection and IPD.
constexpr const char* kLmuConfigExtras[] = {"RenderScale", "UpscaleMethod", "UpscaleSharpness", "ShaderLevel",
                                            "VSyncMode",   "VRFOVScaling",  "VRUIScale"};
constexpr const char* kLmuPlayerExtras[] = {
    "Max Framerate",        "Render Once Per VSync",  "Delay Video Swap",     "Synchronize Frame",
    "MIP Mapping",          "Texture Streaming",      "Rain FX Quality",      "Raindrop Flow",
    "Rainspray Flow",       "Smoke Flow",             "Spark Flow",           "Tire Emitter Flow",
    "Engine Emitter Flow",  "Heat FX Fade Speed",     "Moving Rearview",      "Rearview Width",
    "Rearview Height",      "Rearview_Front_Clip",    "Rearview Onboard",     "Rearview Independent",
    "Rearview Swingman",    "Replay Extra Visible Vehicles", "VR Projection Mode", "VR IPD Scale",
    "Dynamic Vertex Buffers", "Full Tire Shader",     "UI autonomous render thread", "Low Detail UI",
    "Showroom Background Rendering", "Real Road System", "Partial Cockpit View", "Steering Wheel",
    "Head Physics"};

// The summary's settings (Def keys or extras), in its order.
constexpr const char* kLmuSummary[] = {"EPostProcessingSettings", "MSAA",           "UseFXAA",
                                       "RenderScale",             "UpscaleMethod",  "VRFOVScaling",
                                       "Track Detail",            "Player Detail",  "Opponent Detail",
                                       "Texture Detail",          "Shadows",        "Environment Reflections",
                                       "Road Reflections",        "Screen Space Ambient Occlusion",
                                       "Max Visible Vehicles",    "Rearview Driving", "Auto Detail Framerate"};

const lmu::Def* lmu_def(const char* key) {
    for (const lmu::Def& d : lmu::defs())
        if (std::strcmp(d.key, key) == 0)
            return &d;
    return nullptr;
}

// A value's name on the page ("High", "On"), English; empty when the page doesn't know it.
std::wstring value_name(const lmu::Def& d, const std::string& value) {
    if (d.kind == lmu::Kind::Toggle) {
        bool known = false;
        const bool on = lmu::toggle_on(value, known);
        return known ? (on ? L"On" : L"Off") : L"";
    }
    const int i = lmu::option_index(d, value);
    return i < 0 ? L"" : d.options[i].name;
}

struct Line {
    const char* key;
    bool present;
    std::string value;
};

std::string section(const char* title, bool ok, lmu::File file, const std::string& text, const std::vector<const char*>& keys,
                    std::vector<Line>& lines) {
    std::string out = std::string("[") + title + "]\n";
    if (!ok)
        return out + "; could not read the file\n";
    for (const char* key : keys) {
        std::string v;
        const bool there = lmu::read_value(file, text, key, v);
        lines.push_back({key, there, v});
        out += std::string(key) + "=" + v;
        if (!there) {
            out += " ; not in the file";
        } else if (const lmu::Def* d = lmu_def(key)) {
            const std::wstring name = value_name(*d, v);
            out += " ; " + narrow(d->label) + ": " + (name.empty() ? "other" : narrow(name));
        }
        out += "\n";
    }
    return out;
}

// A summary setting's short text: "Post effects High", "Render scale 1.000".
std::string summary_item(const Line& l) {
    if (const lmu::Def* d = lmu_def(l.key)) {
        const std::wstring name = value_name(*d, l.value);
        return narrow(d->label) + " " + (name.empty() ? l.value : narrow(name));
    }
    if (std::strcmp(l.key, "RenderScale") == 0)
        return "Render scale " + l.value;
    if (std::strcmp(l.key, "UpscaleMethod") == 0)
        return "Upscaler " + l.value;
    if (std::strcmp(l.key, "VRFOVScaling") == 0)
        return "VR FOV scale " + l.value;
    return std::string(l.key) + " " + l.value;
}

}  // namespace

std::string lmu_settings_text(bool configOk, const std::string& config, bool playerOk, const std::string& player) {
    std::vector<const char*> configKeys, playerKeys;
    for (const lmu::Def& d : lmu::defs())
        (d.file == lmu::File::Config ? configKeys : playerKeys).push_back(d.key);
    configKeys.insert(configKeys.end(), std::begin(kLmuConfigExtras), std::end(kLmuConfigExtras));
    playerKeys.insert(playerKeys.end(), std::begin(kLmuPlayerExtras), std::end(kLmuPlayerExtras));
    std::vector<Line> lines;
    const std::string body = section("Config_DX11_VR.ini", configOk, lmu::File::Config, config, configKeys, lines) +
                             section("Settings.JSON Graphic Options", playerOk, lmu::File::Player, player, playerKeys, lines);
    std::string summary;
    for (const char* key : kLmuSummary)
        for (const Line& l : lines)
            if (l.present && std::strcmp(l.key, key) == 0)
                summary += (summary.empty() ? "" : ", ") + summary_item(l);
    if (!configOk || !playerOk)
        summary += std::string(summary.empty() ? "" : "; ") +
                   (!configOk && !playerOk ? "neither file could be read"
                    : !configOk            ? "Config_DX11_VR.ini could not be read"
                                           : "Settings.JSON could not be read");
    return "; Le Mans Ultimate's graphics settings at this launch, read from its files (Sim-QVFR changed nothing in "
           "them)\n; summary: " +
           summary + "\n" + body;
}

std::string game_settings_text(const char* gameId, const std::wstring& exe) {
    if (!gameId || std::strcmp(gameId, "lmu") != 0)
        return "";
    const std::wstring folder = lmu::user_folder(exe);
    if (folder.empty())
        return "";
    std::string config, player;
    const bool configOk = lmu::read_file(lmu::file_path(folder, lmu::File::Config), config);
    const bool playerOk = lmu::read_file(lmu::file_path(folder, lmu::File::Player), player);
    return lmu_settings_text(configOk, config, playerOk, player);
}

std::wstring game_settings_summary(const std::string& text) {
    constexpr char kPrefix[] = "; summary: ";
    std::size_t at = text.rfind(std::string("\n") + kPrefix);
    at = text.compare(0, sizeof(kPrefix) - 1, kPrefix) == 0 ? 0 : at == std::string::npos ? at : at + 1;
    if (at == std::string::npos)
        return L"";
    const std::size_t from = at + sizeof(kPrefix) - 1, eol = text.find_first_of("\r\n", from);
    return widen(text.substr(from, eol == std::string::npos ? std::string::npos : eol - from));
}

}  // namespace simqvfr::launcher
