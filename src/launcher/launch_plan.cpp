#include "launch_plan.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>

#include <windows.h>

namespace aceqv::launcher {
namespace {

namespace fs = std::filesystem;

bool same_name(const std::wstring& a, const std::wstring& b) { return _wcsicmp(a.c_str(), b.c_str()) == 0; }

bool is_variable_name(const std::string& s) {
    if (s.empty() || !(isalpha(static_cast<unsigned char>(s[0])) || s[0] == '_'))
        return false;
    return std::all_of(s.begin(), s.end(), [](char c) { return isalnum(static_cast<unsigned char>(c)) || c == '_'; });
}

// The layer object of a single-layer manifest, or null.
const json::Value* layer_of(const json::Value& manifest) {
    const json::Value* layer = manifest.find("api_layer");
    return layer && layer->type == json::Value::Type::Object ? layer : nullptr;
}

}  // namespace

std::wstring widen(const std::string& utf8) {
    if (utf8.empty())
        return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    std::wstring out(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), out.data(), n);
    return out;
}

std::string narrow(const std::wstring& wide) {
    if (wide.empty())
        return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), out.data(), n, nullptr, nullptr);
    return out;
}

bool read_file(const std::wstring& path, std::string& out) {
    std::ifstream in(fs::path(path), std::ios::binary);
    if (!in)
        return false;
    out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return true;
}

bool write_file(const std::wstring& path, const std::string& data) {
    std::ofstream out(fs::path(path), std::ios::binary | std::ios::trunc);
    out.write(data.data(), static_cast<std::streamsize>(data.size()));
    return static_cast<bool>(out);
}

bool explicit_copy(const json::Value& manifest, const std::wstring& manifestPath, json::Value& copy,
                   std::wstring& library) {
    const json::Value* layer = layer_of(manifest);
    if (!layer)
        return false;
    const std::string path = layer->string_at("library_path");
    if (path.empty())
        return false;
    fs::path lib(widen(path));
    if (lib.is_relative())
        lib = fs::path(manifestPath).parent_path() / lib;
    std::error_code ec;
    const fs::path absolute = fs::weakly_canonical(lib, ec);
    library = (ec ? lib.lexically_normal() : absolute).wstring();
    copy = manifest;
    json::Value* copied = copy.find("api_layer");
    copied->set_string("library_path", narrow(library));
    copied->erase("disable_environment");
    copied->erase("enable_environment");
    return true;
}

LayerPlan plan_layers(const std::vector<Registration>& registrations, const Options& options) {
    LayerPlan plan;
    std::vector<std::string> seen;
    const auto is_set = [&](const std::string& variable) {
        const std::wstring w = widen(variable);
        return std::any_of(options.setVariables.begin(), options.setVariables.end(),
                           [&](const std::wstring& v) { return same_name(v, w); });
    };
    for (const Registration& r : registrations) {
        const std::string where = narrow(r.manifest);
        if (!r.enabled)
            continue;  // registered but switched off: the loader ignores it too
        std::string text;
        json::Value manifest;
        if (!read_file(r.manifest, text) || !json::parse(text, manifest) || !layer_of(manifest)) {
            plan.notes.push_back(where + ": unreadable manifest, left alone");
            continue;
        }
        const json::Value& layer = *layer_of(manifest);
        const std::string name = layer.string_at("name");
        if (name.empty()) {
            plan.notes.push_back(where + ": manifest without a layer name, left alone");
            continue;
        }
        if (name == kLayerName)
            continue;  // this layer's installed copy: silenced below, loaded explicitly instead
        if (std::find(seen.begin(), seen.end(), name) != seen.end())
            continue;  // a second registration of a layer already handled (same disable variable)
        const std::string enable = layer.string_at("enable_environment");
        if (!enable.empty() && !is_set(enable))
            continue;  // inactive unless its enable variable is set
        json::Value copy;
        std::wstring library;
        if (!explicit_copy(manifest, r.manifest, copy, library)) {
            plan.notes.push_back(name + ": no library_path, left alone");
            continue;
        }
        std::error_code ec;
        if (!fs::exists(library, ec)) {
            plan.notes.push_back(name + ": library " + narrow(library) + " missing (the loader skips it too)");
            continue;
        }
        seen.push_back(name);
        const std::string disable = layer.string_at("disable_environment");
        if (!is_variable_name(disable)) {
            plan.notes.push_back(name + ": has no disable_environment, so it stays ABOVE the ACE layer (" + where + ")");
            continue;
        }
        const bool off = std::find(options.offLayers.begin(), options.offLayers.end(), name) != options.offLayers.end() ||
                         (options.frameGenerationOff && name == kFrameGenerationLayer);
        if (off) {
            plan.off.emplace_back(name, widen(disable));
            continue;
        }
        plan.below.push_back(MovedLayer{name, library, widen(disable), std::move(copy)});
    }
    return plan;
}

bool write_run_folder(const std::wstring& folder, const std::wstring& layerManifest, const std::wstring& layerLibrary,
                      const LayerPlan& plan, std::wstring& error) {
    std::error_code ec;
    fs::create_directories(folder, ec);
    if (ec) {
        error = L"cannot create " + folder;
        return false;
    }
    std::string text;
    json::Value ours;
    if (!read_file(layerManifest, text) || !json::parse(text, ours) || !layer_of(ours) ||
        layer_of(ours)->string_at("name") != kLayerName) {
        error = L"the layer manifest " + layerManifest + L" is missing or not the ACE layer's";
        return false;
    }
    json::Value* layer = ours.find("api_layer");
    layer->set_string("library_path", narrow(layerLibrary));
    layer->erase("disable_environment");  // that variable silences an installed copy for this launch
    layer->erase("enable_environment");
    if (!write_file((fs::path(folder) / (std::string(kLayerName) + ".json")).wstring(), json::dump(ours))) {
        error = L"cannot write the layer manifest in " + folder;
        return false;
    }
    for (const MovedLayer& m : plan.below)
        if (!write_file((fs::path(folder) / (m.name + ".json")).wstring(), json::dump(m.manifest))) {
            error = L"cannot write " + widen(m.name) + L".json in " + folder;
            return false;
        }
    return true;
}

std::vector<std::pair<std::wstring, std::wstring>> launch_variables(const LayerPlan& plan, const std::wstring& runFolder,
                                                                    const std::wstring& ini, const std::wstring& log,
                                                                    const std::wstring& runtimeInfo) {
    std::wstring enabled = widen(kLayerName);  // listed nearest the game first
    for (const MovedLayer& m : plan.below)
        enabled += L";" + widen(m.name);
    std::vector<std::pair<std::wstring, std::wstring>> v = {
        {L"XR_API_LAYER_PATH", runFolder},
        {L"XR_ENABLE_API_LAYERS", enabled},
        {L"DISABLE_" + widen(kLayerName), L"1"},  // an installed copy of this layer
        {L"ACE_QV_INI", ini},
        {L"ACE_QV_LOG", log},
        {L"ACE_QV_RUNTIME_INFO", runtimeInfo},
        {L"SteamAppId", kAceAppId},
        {L"SteamGameId", kAceAppId},
    };
    for (const MovedLayer& m : plan.below)
        v.emplace_back(m.disableVar, L"1");
    for (const auto& [name, variable] : plan.off)
        v.emplace_back(variable, L"1");
    return v;
}

std::wstring environment_block(const std::vector<std::wstring>& current,
                               const std::vector<std::pair<std::wstring, std::wstring>>& set) {
    std::vector<std::pair<std::wstring, std::wstring>> vars;
    for (const std::wstring& entry : current) {
        const size_t eq = entry.find(L'=', 1);  // entries like "=C:=C:\x" start with '='
        if (eq == std::wstring::npos)
            continue;
        std::wstring name = entry.substr(0, eq);
        if (_wcsnicmp(name.c_str(), L"ACE_QV_", 7) == 0)
            continue;
        vars.emplace_back(std::move(name), entry.substr(eq + 1));
    }
    for (const auto& [name, value] : set) {
        const auto it = std::find_if(vars.begin(), vars.end(), [&](const auto& p) { return same_name(p.first, name); });
        if (it != vars.end())
            it->second = value;
        else
            vars.emplace_back(name, value);
    }
    std::sort(vars.begin(), vars.end(), [](const auto& a, const auto& b) { return _wcsicmp(a.first.c_str(), b.first.c_str()) < 0; });
    std::wstring block;
    for (const auto& [name, value] : vars) {
        block += name + L"=" + value;
        block += L'\0';
    }
    block += L'\0';
    return block;
}

std::vector<std::wstring> steam_libraries(const std::string& vdf) {
    std::vector<std::wstring> out;
    std::istringstream lines(vdf);
    std::string line;
    while (std::getline(lines, line)) {
        // "path"		"D:\\SteamLibrary"
        std::vector<std::string> quoted;
        for (size_t i = 0; i < line.size();) {
            const size_t open = line.find('"', i);
            if (open == std::string::npos)
                break;
            std::string token;
            size_t j = open + 1;
            for (; j < line.size() && line[j] != '"'; ++j) {
                if (line[j] == '\\' && j + 1 < line.size())
                    ++j;
                token += line[j];
            }
            quoted.push_back(token);
            i = j + 1;
        }
        if (quoted.size() == 2 && _stricmp(quoted[0].c_str(), "path") == 0 && !quoted[1].empty())
            out.push_back(widen(quoted[1]));
    }
    return out;
}

}  // namespace aceqv::launcher
