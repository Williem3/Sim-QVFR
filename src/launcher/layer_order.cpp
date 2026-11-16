#include "layer_order.h"

#include <algorithm>
#include <cstring>
#include <iterator>
#include <sstream>

#include <bcrypt.h>

#include "../ini.h"

namespace simqvfr::launcher {
namespace {

std::wstring join(const std::vector<std::string>& names) {
    std::string out;
    for (const std::string& n : names)
        out += (out.empty() ? "" : ",") + n;
    return widen(out);
}

void remove(std::vector<std::string>& names, const std::string& name) {
    names.erase(std::remove(names.begin(), names.end(), name), names.end());
}

// One registry value as it is stored.
struct RawValue {
    std::wstring name;
    DWORD type = REG_DWORD;
    std::vector<BYTE> data;
};

std::vector<RawValue> read_values(HKEY key) {
    std::vector<RawValue> out;
    for (DWORD i = 0;; ++i) {
        wchar_t name[2048];
        DWORD nameLength = static_cast<DWORD>(std::size(name)), type = 0, size = 0;
        LSTATUS s = RegEnumValueW(key, i, name, &nameLength, nullptr, &type, nullptr, &size);
        if (s == ERROR_NO_MORE_ITEMS)
            break;
        if (s != ERROR_SUCCESS)
            continue;
        RawValue v{std::wstring(name, nameLength), type, std::vector<BYTE>(size)};
        nameLength = static_cast<DWORD>(std::size(name));
        s = RegEnumValueW(key, i, name, &nameLength, nullptr, &type, v.data.empty() ? nullptr : v.data.data(), &size);
        if (s == ERROR_SUCCESS)
            out.push_back(std::move(v));
    }
    return out;
}

bool is_on(const RawValue& v) {
    DWORD d = 1;
    if (v.type != REG_DWORD || v.data.size() < sizeof(d))
        return false;
    std::memcpy(&d, v.data.data(), sizeof(d));
    return d == 0;
}

std::string layer_name(const std::wstring& manifest) {
    std::string text;
    json::Value v;
    if (!read_file(manifest, text) || !json::parse(text, v))
        return {};
    const json::Value* layer = v.find("api_layer");
    return layer ? layer->string_at("name") : std::string();
}

// A .reg file of the key's values (UTF-16 with a byte-order mark, as regedit writes them).
bool write_backup(const std::wstring& path, bool machine, const wchar_t* subkey, const std::vector<RawValue>& values) {
    std::wstring text = L"Windows Registry Editor Version 5.00\r\n\r\n[";
    text += std::wstring(machine ? L"HKEY_LOCAL_MACHINE\\" : L"HKEY_CURRENT_USER\\") + subkey + L"]\r\n";
    for (const RawValue& v : values) {
        std::wstring name;
        for (const wchar_t c : v.name) {
            if (c == L'\\' || c == L'"')
                name += L'\\';
            name += c;
        }
        text += L"\"" + name + L"\"=";
        wchar_t hex[16];
        if (v.type == REG_DWORD && v.data.size() == 4) {
            DWORD d = 0;
            std::memcpy(&d, v.data.data(), 4);
            swprintf_s(hex, L"dword:%08x", d);
            text += hex;
        } else {
            swprintf_s(hex, L"hex(%x):", v.type);
            text += hex;
            for (size_t i = 0; i < v.data.size(); ++i) {
                swprintf_s(hex, L"%s%02x", i ? L"," : L"", v.data[i]);
                text += hex;
            }
        }
        text += L"\r\n";
    }
    std::string bytes = "\xff\xfe";
    bytes.append(reinterpret_cast<const char*>(text.data()), text.size() * sizeof(wchar_t));
    return write_file(path, bytes);
}

bool set_value(HKEY key, const RawValue& v) {
    return RegSetValueExW(key, v.name.c_str(), 0, v.type, v.data.empty() ? nullptr : v.data.data(),
                          static_cast<DWORD>(v.data.size())) == ERROR_SUCCESS;
}

}  // namespace

// ---- With the launcher --------------------------------------------------------------------------

std::vector<std::string> split_layer_names(const std::wstring& list) {
    std::vector<std::string> out;
    std::string current;
    for (const char c : narrow(list) + ",") {
        if (c == ',' || c == ';') {
            if (!current.empty() && std::find(out.begin(), out.end(), current) == out.end())
                out.push_back(current);
            current.clear();
        } else if (c != ' ' && c != '\t') {
            current += c;
        }
    }
    return out;
}

LauncherLayers read_launcher_layers(const std::wstring& ini) {
    return {split_layer_names(ini_get(ini.c_str(), L"launcher", L"layer_order")),
            split_layer_names(ini_get(ini.c_str(), L"launcher", L"off_layers")),
            split_layer_names(ini_get(ini.c_str(), L"launcher", L"on_layers"))};
}

bool write_launcher_layers(const std::wstring& ini, const LauncherLayers& l) {
    bool ok = WritePrivateProfileStringW(L"launcher", L"layer_order", join(l.order).c_str(), ini.c_str()) != 0;
    ok &= WritePrivateProfileStringW(L"launcher", L"off_layers", join(l.off).c_str(), ini.c_str()) != 0;
    ok &= WritePrivateProfileStringW(L"launcher", L"on_layers", join(l.on).c_str(), ini.c_str()) != 0;
    return ok;
}

Options launch_options(const LauncherLayers& layers, bool twoLoop, bool cheekyLoad, bool cheekyFg,
                       std::vector<std::wstring> setVariables, bool antiCheat) {
    if (antiCheat)
        cheekyLoad = cheekyFg = false;
    Options o;
    o.order = layers.order;
    o.offLayers = layers.off;
    o.onLayers = layers.on;
    o.setVariables = std::move(setVariables);
    if (twoLoop)
        o.forced.push_back({kFrameGenerationLayer, false, "Two-loop renders two game frames per headset frame: frame generation breaks it"});
    else if (cheekyLoad && cheekyFg)
        o.forced.push_back({kFrameGenerationLayer, false, "Cheeky's frame generation is on: one frame generation at a time"});
    if (cheekyLoad)
        o.forced.push_back({kCheekyLayer, true, "Cheeky Foveated DLSS is on: its runtime needs its layer"});
    if (antiCheat)
        o.forced.push_back({kCheekyLayer, false, "Run with anti-cheat is on: Cheeky stays off"});
    return o;
}

bool move_launcher_layer(LauncherLayers& layers, const LayerPlan& plan, const std::string& name, int step) {
    std::vector<std::string> names;
    for (const PlannedLayer& p : plan.layers)
        names.push_back(p.name);
    const auto it = std::find(names.begin(), names.end(), name);
    if (it == names.end())
        return false;
    const auto at = it - names.begin(), to = at + step;
    if (to < 0 || to >= static_cast<std::ptrdiff_t>(names.size()))
        return false;
    std::swap(names[at], names[to]);
    layers.order = names;
    return true;
}

void set_launcher_layer(LauncherLayers& layers, const std::string& name, bool on, bool systemOn) {
    remove(layers.on, name);
    remove(layers.off, name);
    if (on != systemOn)
        (on ? layers.on : layers.off).push_back(name);
}

// ---- Without the launcher (Windows) -------------------------------------------------------------

std::vector<SystemLayer> read_system_layers(HKEY machine, HKEY user, const wchar_t* subkey, bool* lockMoved) {
    std::vector<SystemLayer> out;
    for (const HKEY root : {machine, user}) {
        HKEY key = nullptr;
        if (RegOpenKeyExW(root, subkey, 0, KEY_READ | KEY_WOW64_64KEY, &key) != ERROR_SUCCESS)
            continue;
        for (const RawValue& v : read_values(key))
            out.push_back({v.name, root == machine, is_on(v), layer_name(v.name), {}});
        RegCloseKey(key);
    }
    const bool moved = lock_system_layers(out);
    if (lockMoved)
        *lockMoved = moved;
    return out;
}

bool lock_system_layers(std::vector<SystemLayer>& layers) {
    bool moved = false;
    for (const bool machine : {true, false}) {
        const auto first = std::find_if(layers.begin(), layers.end(), [&](const SystemLayer& l) { return l.machine == machine; });
        const auto ours = std::find_if(layers.begin(), layers.end(),
                                       [&](const SystemLayer& l) { return l.machine == machine && l.name == kLayerName; });
        if (ours == layers.end())
            continue;
        ours->lock = "the Sim-QVFR layer stays nearest the game";
        moved |= ours != first;
        std::rotate(first, ours, ours + 1);
    }
    return moved;
}

bool move_system_layer(std::vector<SystemLayer>& layers, size_t index, int step) {
    const std::ptrdiff_t to = static_cast<std::ptrdiff_t>(index) + step;
    if (index >= layers.size() || to < 0 || to >= static_cast<std::ptrdiff_t>(layers.size()))
        return false;
    SystemLayer &a = layers[index], &b = layers[static_cast<size_t>(to)];
    if (a.machine != b.machine || !a.lock.empty() || !b.lock.empty())
        return false;
    std::swap(a, b);
    return true;
}

bool system_needs_admin(const std::vector<SystemLayer>& current, const std::vector<SystemLayer>& want) {
    const auto hive = [](const std::vector<SystemLayer>& l) {
        std::vector<SystemLayer> out;
        std::copy_if(l.begin(), l.end(), std::back_inserter(out), [](const SystemLayer& s) { return s.machine; });
        return out;
    };
    return hive(current) != hive(want);
}

bool backup_key(HKEY root, const wchar_t* subkey, bool machine, const std::wstring& path) {
    HKEY key = nullptr;
    if (RegOpenKeyExW(root, subkey, 0, KEY_READ | KEY_WOW64_64KEY, &key) != ERROR_SUCCESS)
        return false;
    const std::vector<RawValue> values = read_values(key);
    RegCloseKey(key);
    return write_backup(path, machine, subkey, values);
}

bool write_system_hive(HKEY root, const wchar_t* subkey, bool machine, const std::vector<SystemLayer>& all,
                       const std::wstring& backup, std::wstring& error) {
    std::vector<SystemLayer> want;
    std::copy_if(all.begin(), all.end(), std::back_inserter(want), [&](const SystemLayer& s) { return s.machine == machine; });
    if (want.empty())
        return true;
    HKEY key = nullptr;
    const LSTATUS opened = RegOpenKeyExW(root, subkey, 0, KEY_READ | KEY_SET_VALUE | KEY_WOW64_64KEY, &key);
    if (opened != ERROR_SUCCESS) {
        error = opened == ERROR_ACCESS_DENIED ? L"administrator rights are needed to change the machine-wide layers"
                                              : L"the OpenXR layer key cannot be opened (error " + std::to_wstring(opened) + L")";
        return false;
    }
    const std::vector<RawValue> existing = read_values(key);
    bool sameSet = existing.size() == want.size();
    for (const SystemLayer& w : want)
        sameSet &= std::any_of(existing.begin(), existing.end(), [&](const RawValue& v) { return iequals(v.name, w.manifest); });
    if (!sameSet) {
        RegCloseKey(key);
        error = L"the installed layers changed since they were read; nothing was written";
        return false;
    }
    // The order can't change: the loader reads values in their stored order, and re-adding a value keeps its
    // slot, so only deleting the key (every vendor's layers) or rewriting other vendors' values would reorder.
    // Neither is ours to do. The list as read_system_layers shows it (the Sim-QVFR layer locked first) is the
    // key's order.
    std::vector<SystemLayer> shown;
    for (const RawValue& v : existing)
        shown.push_back({v.name, machine, is_on(v), layer_name(v.name), {}});
    lock_system_layers(shown);
    for (size_t i = 0; i < want.size(); ++i)
        if (!iequals(shown[i].manifest, want[i].manifest)) {
            RegCloseKey(key);
            error = L"the order of Windows' OpenXR layers can't be changed safely without the launcher; nothing was "
                    L"written (on/off switches still apply)";
            return false;
        }
    if (!backup.empty() && !write_backup(backup, machine, subkey, existing)) {
        RegCloseKey(key);
        error = L"cannot write the backup " + backup + L"; nothing was written";
        return false;
    }
    // Only a switch that changes is written, in place, as the loader's DWORD (0 on, 1 off): the user's own choice.
    bool ok = true;
    for (const SystemLayer& w : want) {
        const RawValue& was = *std::find_if(existing.begin(), existing.end(), [&](const RawValue& v) { return iequals(v.name, w.manifest); });
        if (is_on(was) == w.on)
            continue;
        RawValue v = was;
        const DWORD d = w.on ? 0 : 1;
        v.type = REG_DWORD;
        v.data.assign(reinterpret_cast<const BYTE*>(&d), reinterpret_cast<const BYTE*>(&d) + sizeof(d));
        ok &= set_value(key, v);
    }
    RegCloseKey(key);
    if (!ok)
        error = L"writing an OpenXR layer switch failed (backup of the key as it was: " + backup + L")";
    return ok;
}

std::string system_request(const std::vector<SystemLayer>& want) {
    std::string out;
    for (const SystemLayer& l : want)
        out += std::string(l.machine ? "M" : "U") + "\t" + (l.on ? "1" : "0") + "\t" + narrow(l.manifest) + "\n";
    return out;
}

bool parse_system_request(const std::string& text, std::vector<SystemLayer>& want) {
    want.clear();
    std::istringstream lines(text);
    std::string line;
    while (std::getline(lines, line)) {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (line.empty())
            continue;
        if (line.size() < 5 || (line[0] != 'M' && line[0] != 'U') || line[1] != '\t' || (line[2] != '0' && line[2] != '1') ||
            line[3] != '\t')
            return false;
        // A registry value name: no control characters, at most a long path's length.
        const std::string manifest = line.substr(4);
        if (manifest.size() > 32767 ||
            std::any_of(manifest.begin(), manifest.end(), [](char c) { return static_cast<unsigned char>(c) < 0x20 || c == 0x7f; }))
            return false;
        want.push_back({widen(manifest), line[0] == 'M', line[2] == '1', {}, {}});
        if (want.back().manifest.empty() || narrow(want.back().manifest) != manifest)  // not valid UTF-8
            return false;
    }
    return !want.empty();
}

std::string system_request_hash(const std::string& request) {
    std::string hex;
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    UCHAR digest[32];
    if (BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0)) &&
        BCRYPT_SUCCESS(BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0)) &&
        BCRYPT_SUCCESS(BCryptHashData(hash, reinterpret_cast<PUCHAR>(const_cast<char*>(request.data())),
                                      static_cast<ULONG>(request.size()), 0)) &&
        BCRYPT_SUCCESS(BCryptFinishHash(hash, digest, sizeof(digest), 0))) {
        static const char digits[] = "0123456789abcdef";
        for (const UCHAR b : digest) {
            hex += digits[b >> 4];
            hex += digits[b & 15];
        }
    }
    if (hash)
        BCryptDestroyHash(hash);
    if (alg)
        BCryptCloseAlgorithmProvider(alg, 0);
    return hex;
}

}  // namespace simqvfr::launcher
