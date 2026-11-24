#include "mod_catalog.h"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>

#include <windows.h>

#include "../json.h"

namespace simqvfr::launcher {

namespace fs = std::filesystem;

namespace {

std::wstring widen(const std::string& s) {
    if (s.empty())
        return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(std::max(n, 0)), L'\0');
    if (n > 0)
        MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

std::wstring ascii_wide(const std::string& s) { return std::wstring(s.begin(), s.end()); }

// A shown text: at most `max` characters, no control characters.
std::wstring shown(const std::string& s, size_t max) {
    std::wstring w = widen(s);
    w.erase(std::remove_if(w.begin(), w.end(), [](wchar_t c) { return c < 0x20; }), w.end());
    if (w.size() > max)
        w.resize(max);
    return w;
}

bool read_file(const fs::path& path, std::string& text) {
    std::error_code ec;
    if (!fs::is_regular_file(path, ec) || fs::file_size(path, ec) > 256 * 1024)
        return false;
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return false;
    std::ostringstream s;
    s << in.rdbuf();
    text = s.str();
    return true;
}

std::wstring section_of(const std::wstring& id) { return L"mod_" + id; }

}  // namespace

bool valid_mod_name(const std::string& name, bool dash) {
    if (name.empty() || name.size() > 32)
        return false;
    for (char c : name)
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || (dash && c == '-')))
            return false;
    return true;
}

bool valid_source_link(const std::string& url) {
    if (url.size() < 12 || url.size() > 200 || url.compare(0, 8, "https://") != 0)
        return false;
    for (char c : url)
        if (static_cast<unsigned char>(c) <= 0x20 || c == '"' || c == '<' || c == '>' || c == '\\' || c < 0)
            return false;
    const size_t host = url.find('/', 8);
    return host != std::string::npos && host > 8 && host + 1 < url.size();  // a host and a path: a project, not a site
}

bool parse_mod_manifest(const std::string& text, ModEntry& mod, std::wstring& error) {
    json::Value root;
    if (!json::parse(text, root) || root.type != json::Value::Type::Object) {
        error = L"mod.json isn't valid JSON";
        return false;
    }
    const std::string id = root.string_at("id");
    if (!valid_mod_name(id, true)) {
        error = L"mod.json's id isn't a name a mod can have (a-z, 0-9, _ and -, up to 32)";
        return false;
    }
    if (!mod.id.empty() && _wcsicmp(mod.id.c_str(), ascii_wide(id).c_str()) != 0) {
        error = L"mod.json's id isn't its folder's name";
        return false;
    }
    mod.id = ascii_wide(id);
    const std::string dll = root.string_at("dll");
    const bool plainName = !dll.empty() && dll.size() <= 64 && dll.find_first_of("\\/:") == std::string::npos &&
                           dll.find("..") == std::string::npos && dll.size() > 4 &&
                           _stricmp(dll.c_str() + dll.size() - 4, ".dll") == 0;
    if (!plainName) {
        error = L"mod.json's dll isn't a file name ending .dll";
        return false;
    }
    mod.dll = widen(dll);
    mod.name = shown(root.string_at("name"), 60);
    if (mod.name.empty())
        mod.name = mod.id;
    mod.author = shown(root.string_at("author"), 60);
    mod.version = shown(root.string_at("version"), 24);
    // Every community mod is open source: its code public, linked here (and, later, its updates checked there).
    const std::string source = root.string_at("source");
    mod.source = valid_source_link(source) ? ascii_wide(source) : std::wstring();
    mod.license = shown(root.string_at("license"), 40);
    if (!mod_api::first_party_mod(mod.id) && (mod.source.empty() || mod.license.empty())) {
        error = mod.source.empty() ? L"mod.json has no \"source\" link (https) to the mod's source code: Sim-QVFR mods must be open source"
                                   : L"mod.json has no \"license\" for its source code";
        return false;
    }
    mod.games.clear();
    if (const json::Value* games = root.find("games"); games && games->type == json::Value::Type::Array)
        for (const json::Value& g : games->items)
            if (g.type == json::Value::Type::Object && valid_mod_name(g.string_at("id"), true))
                mod.games.push_back(ascii_wide(g.string_at("id")));
    return true;
}

bool parse_mod_settings(const std::string& text, std::vector<ModSetting>& settings, std::wstring& error) {
    settings.clear();
    json::Value root;
    if (!json::parse(text, root) || root.type != json::Value::Type::Object) {
        error = L"settings.json isn't valid JSON";
        return false;
    }
    const json::Value* list = root.find("settings");
    if (!list || list->type != json::Value::Type::Array) {
        error = L"settings.json has no \"settings\" list";
        return false;
    }
    if (list->items.size() > static_cast<size_t>(kModSettingsMax)) {
        error = L"settings.json has more than 16 settings";
        return false;
    }
    for (const json::Value& v : list->items) {
        ModSetting s;
        s.key = v.type == json::Value::Type::Object ? v.string_at("key") : std::string();
        if (!valid_mod_name(s.key, false)) {
            error = L"a setting's key isn't a name (a-z, 0-9 and _, up to 32)";
            settings.clear();
            return false;
        }
        for (const ModSetting& other : settings)
            if (other.key == s.key) {
                error = L"two settings have the key " + ascii_wide(s.key);
                settings.clear();
                return false;
            }
        s.label = shown(v.string_at("label"), 60);
        if (s.label.empty())
            s.label = ascii_wide(s.key);
        s.help = shown(v.string_at("help"), 300);
        const std::string type = v.string_at("type");
        const json::Value* def = v.find("default");
        if (type == "toggle") {
            s.kind = ModSetting::Kind::Toggle;
            s.def = def && def->type == json::Value::Type::Bool && def->boolean ? "1" : "0";
        } else if (type == "choice") {
            s.kind = ModSetting::Kind::Choice;
            const json::Value* options = v.find("options");
            if (options && options->type == json::Value::Type::Array)
                for (const json::Value& o : options->items) {
                    const std::string value = o.type == json::Value::Type::Object ? o.string_at("value") : std::string();
                    if (!valid_mod_name(value, true))
                        continue;
                    std::wstring label = shown(o.string_at("label"), 24);
                    s.options.push_back({value, label.empty() ? ascii_wide(value) : label});
                }
            if (s.options.size() < 2 || s.options.size() > 4) {
                error = L"the setting " + ascii_wide(s.key) + L" needs 2 to 4 options";
                settings.clear();
                return false;
            }
            s.def = s.options.front().value;
            if (def && def->type == json::Value::Type::String)
                for (const ModOption& o : s.options)
                    if (o.value == def->text)
                        s.def = o.value;
        } else {
            error = L"the setting " + ascii_wide(s.key) + L" has a type other than toggle or choice";
            settings.clear();
            return false;
        }
        settings.push_back(std::move(s));
    }
    return true;
}

std::vector<ModEntry> scan_mods(const std::wstring& modsFolder) {
    std::vector<ModEntry> mods;
    std::error_code ec;
    if (modsFolder.empty() || !fs::is_directory(fs::symlink_status(modsFolder, ec)))
        return mods;
    for (fs::directory_iterator it(modsFolder, ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code e;
        if (!it->is_directory(e) || it->is_symlink(e) ||
            (GetFileAttributesW(it->path().c_str()) & FILE_ATTRIBUTE_REPARSE_POINT))
            continue;
        const fs::path manifest = it->path() / L"mod.json";
        std::string text;
        if (!read_file(manifest, text))
            continue;
        ModEntry mod;
        mod.folder = it->path().wstring();
        mod.id = it->path().filename().wstring();
        if (!parse_mod_manifest(text, mod, mod.error)) {
            if (mod.name.empty())
                mod.name = mod.id;
        } else {
            mod.dllPath = (it->path() / mod.dll).wstring();
            if (read_file(it->path() / L"settings.json", text))
                parse_mod_settings(text, mod.settings, mod.settingsError);
        }
        mods.push_back(std::move(mod));
    }
    std::sort(mods.begin(), mods.end(), [](const ModEntry& a, const ModEntry& b) { return a.id < b.id; });
    if (mods.size() > static_cast<size_t>(kModsMax))
        mods.resize(static_cast<size_t>(kModsMax));
    return mods;
}

ModStatus mod_status(const ModEntry& mod, const std::wstring& ini, bool coreOfficial) {
    ModStatus st;
    if (!mod.error.empty() || mod.dllPath.empty())
        return st;
    st.facts = mod_api::read_mod_facts(mod.dllPath);
    st.approval = mod_api::read_mod_approval(ini, mod.id);
    mod_api::TrustInput in;
    in.mod = st.facts;
    in.approval = st.approval;
    in.coreOfficial = coreOfficial;
    in.firstPartyPath = mod_api::first_party_mod(mod.id);
    st.offline = mod_api::decide_mod_trust(in);
    in.onlineLaunch = true;
    st.online = mod_api::decide_mod_trust(in);
    // Third-party: neither official nor this build's own (the approval is what decides).
    mod_api::TrustInput bare = in;
    bare.approval = {};
    bare.onlineLaunch = false;
    const mod_api::ModTrust without = mod_api::decide_mod_trust(bare).trust;
    st.approvable = !st.facts.sha256.empty() && without == mod_api::ModTrust::Refused;
    return st;
}

std::string mod_setting_value(const std::wstring& ini, const std::wstring& id, const ModSetting& setting) {
    wchar_t buffer[64] = {};
    GetPrivateProfileStringW(section_of(id).c_str(), ascii_wide(setting.key).c_str(), L"", buffer,
                             static_cast<DWORD>(std::size(buffer)), ini.c_str());
    const std::wstring w = buffer;
    std::string v;
    for (wchar_t c : w)
        v += c < 0x80 ? static_cast<char>(c) : '?';
    if (setting.kind == ModSetting::Kind::Toggle)
        return v == "1" || v == "0" ? v : setting.def;
    for (const ModOption& o : setting.options)
        if (o.value == v)
            return v;
    return setting.def;
}

bool write_mod_setting(const std::wstring& ini, const std::wstring& id, const ModSetting& setting, const std::string& value) {
    bool known = setting.kind == ModSetting::Kind::Toggle ? (value == "1" || value == "0") : false;
    for (const ModOption& o : setting.options)
        known |= o.value == value;
    if (!known || ini.empty())
        return false;
    return WritePrivateProfileStringW(section_of(id).c_str(), ascii_wide(setting.key).c_str(), ascii_wide(value).c_str(),
                                      ini.c_str()) != 0;
}

}  // namespace simqvfr::launcher
