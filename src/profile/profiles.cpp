#include "profiles.h"

#include <algorithm>
#include <cstring>
#include <cwctype>
#include <filesystem>

#include <windows.h>

namespace simqvfr {
namespace {

namespace fs = std::filesystem;

constexpr wchar_t kExtension[] = L".ini";

struct Preset {
    const wchar_t* name;
    int outer, density, width, height;
};
// Each keeps periphery_scale + focus_density x focus_percent under 1 (a 4000-pixel-wide headset's views fit
// the runtime's 8192-pixel limit). Eye tracked: a small, dense area that follows the eyes.
constexpr Preset kEyeTrackedPresets[] = {
    {L"Performance", 20, 140, 25, 25}, {L"Balanced", 25, 160, 28, 28}, {L"Quality", 25, 180, 30, 30}};
// Fixed: a larger area, wider than tall (the eyes move off the centre more sideways), a little less dense.
constexpr Preset kFixedPresets[] = {
    {L"Performance", 25, 120, 35, 30}, {L"Balanced", 25, 140, 40, 35}, {L"Quality", 25, 160, 42, 38}};
constexpr const Preset (&kPresets)[3] = kEyeTrackedPresets;  // the names, the same in both sets

std::string utf8(const std::wstring& s) {
    if (s.empty())
        return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n, nullptr, nullptr);
    return out;
}

std::wstring wide(const std::string& s) {
    if (s.empty())
        return {};
    const int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), static_cast<int>(s.size()), nullptr, 0);
    if (n <= 0)
        return {};
    std::wstring out(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n);
    return out;
}

constexpr char kBase64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string base64(const std::string& in) {
    std::string out;
    for (size_t i = 0; i < in.size(); i += 3) {
        const uint32_t b = static_cast<uint8_t>(in[i]) << 16 | (i + 1 < in.size() ? static_cast<uint8_t>(in[i + 1]) << 8 : 0) |
                           (i + 2 < in.size() ? static_cast<uint8_t>(in[i + 2]) : 0);
        out += kBase64[b >> 18 & 63];
        out += kBase64[b >> 12 & 63];
        out += i + 1 < in.size() ? kBase64[b >> 6 & 63] : '=';
        out += i + 2 < in.size() ? kBase64[b & 63] : '=';
    }
    return out;
}

bool unbase64(const std::string& in, std::string& out) {
    out.clear();
    uint32_t bits = 0;
    int count = 0;
    for (const char c : in) {
        if (c == '=')
            break;
        if (c == ' ' || c == '\r' || c == '\n' || c == '\t')  // pasted from a chat, wrapped
            continue;
        const char* at = std::strchr(kBase64, c);
        if (!at || !c)
            return false;
        bits = bits << 6 | static_cast<uint32_t>(at - kBase64);
        if (++count == 4) {
            out += static_cast<char>(bits >> 16 & 0xff);
            out += static_cast<char>(bits >> 8 & 0xff);
            out += static_cast<char>(bits & 0xff);
            bits = 0;
            count = 0;
        }
    }
    if (count == 1)
        return false;
    if (count == 2)
        out += static_cast<char>(bits >> 4 & 0xff);
    if (count == 3) {
        out += static_cast<char>(bits >> 10 & 0xff);
        out += static_cast<char>(bits >> 2 & 0xff);
    }
    return true;
}

// The NUL-separated, double-NUL-ended lists GetPrivateProfileSectionNames and GetPrivateProfileSection give.
std::vector<std::wstring> nul_list(const wchar_t* ini, const wchar_t* section) {
    std::vector<wchar_t> buffer(32 * 1024);
    const DWORD n = section ? GetPrivateProfileSectionW(section, buffer.data(), static_cast<DWORD>(buffer.size()), ini)
                            : GetPrivateProfileSectionNamesW(buffer.data(), static_cast<DWORD>(buffer.size()), ini);
    std::vector<std::wstring> out;
    for (const wchar_t* p = buffer.data(); p < buffer.data() + n && *p; p += wcslen(p) + 1)
        out.emplace_back(p);
    return out;
}

std::wstring read_value(const wchar_t* ini, const std::wstring& section, const std::wstring& key) {
    wchar_t value[1024] = {};
    GetPrivateProfileStringW(section.c_str(), key.c_str(), L"\x01", value, static_cast<DWORD>(std::size(value)), ini);
    return value;
}

std::wstring trim(std::wstring s) {
    while (!s.empty() && std::iswspace(s.back()))
        s.pop_back();
    size_t i = 0;
    while (i < s.size() && std::iswspace(s[i]))
        ++i;
    return s.substr(i);
}

}  // namespace

std::vector<Profile> builtin_profiles(bool eyeTracked) {
    std::vector<Profile> out;
    for (const Preset& p : eyeTracked ? kEyeTrackedPresets : kFixedPresets) {
        Profile profile;
        profile.name = p.name;
        profile.builtIn = true;
        const auto n = [](int v) { return std::to_wstring(v); };
        profile.keys = {{L"experiment", L"periphery_scale", n(p.outer)},   {L"experiment", L"eye_scale", n(p.outer)},
                        {L"experiment", L"focus_density", n(p.density)},   {L"experiment", L"focus_percent", n(p.width)},
                        {L"experiment", L"focus_height_percent", n(p.height)}};
        out.push_back(std::move(profile));
    }
    return out;
}

std::wstring preset_summary(const Profile& profile) {
    if (!profile.builtIn)
        return {};
    std::wstring outer, density, width, height;
    for (const ProfileKey& k : profile.keys) {
        if (k.key == L"periphery_scale")
            outer = k.value;
        else if (k.key == L"focus_density")
            density = k.value;
        else if (k.key == L"focus_percent")
            width = k.value;
        else if (k.key == L"focus_height_percent")
            height = k.value;
    }
    return L"Outer " + outer + L"% · inner " + density + L"% · focus " + width + L" × " + height + L"%";
}

std::wstring profiles_folder(const std::wstring& settingsIni) {
    return (fs::path(settingsIni).parent_path() / L"profiles").wstring();
}

bool is_preset_name(const std::wstring& name) {
    for (const Preset& p : kPresets)
        if (_wcsicmp(p.name, name.c_str()) == 0)
            return true;
    return false;
}

bool valid_profile_name(const std::wstring& name) {
    if (name.empty() || name.size() > 40 || trim(name) != name || name.back() == L'.' || is_preset_name(name))
        return false;
    for (const wchar_t c : name)
        if (c < 32 || std::wcschr(L"\\/:*?\"<>|", c))
            return false;
    return true;
}

std::vector<std::wstring> profile_names(const std::wstring& folder) {
    std::vector<std::wstring> names;
    for (const Preset& p : kPresets)
        names.emplace_back(p.name);
    std::vector<std::wstring> saved;
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(folder, ec))
        if (entry.is_regular_file(ec) && _wcsicmp(entry.path().extension().c_str(), kExtension) == 0 &&
            valid_profile_name(entry.path().stem().wstring()))
            saved.push_back(entry.path().stem().wstring());
    std::sort(saved.begin(), saved.end(), [](const std::wstring& a, const std::wstring& b) { return _wcsicmp(a.c_str(), b.c_str()) < 0; });
    names.insert(names.end(), saved.begin(), saved.end());
    return names;
}

bool load_profile(const std::wstring& folder, const std::wstring& name, Profile& profile, bool eyeTracked) {
    for (const Profile& p : builtin_profiles(eyeTracked))
        if (_wcsicmp(p.name.c_str(), name.c_str()) == 0) {
            profile = p;
            return true;
        }
    if (!valid_profile_name(name))
        return false;
    const std::wstring file = (fs::path(folder) / (name + kExtension)).wstring();
    if (GetFileAttributesW(file.c_str()) == INVALID_FILE_ATTRIBUTES)
        return false;
    Profile p;
    p.name = name;
    for (const std::wstring& section : nul_list(file.c_str(), nullptr))
        for (const std::wstring& line : nul_list(file.c_str(), section.c_str())) {
            const size_t eq = line.find(L'=');
            if (eq != std::wstring::npos && eq > 0 && line[0] != L';')
                p.keys.push_back({section, trim(line.substr(0, eq)), trim(line.substr(eq + 1))});
        }
    if (p.keys.empty())
        return false;
    profile = std::move(p);
    return true;
}

bool save_profile(const std::wstring& folder, const Profile& profile) {
    if (!valid_profile_name(profile.name) || profile.keys.empty())
        return false;
    std::error_code ec;
    fs::create_directories(folder, ec);
    const std::wstring file = (fs::path(folder) / (profile.name + kExtension)).wstring();
    DeleteFileW(file.c_str());  // afresh: no keys left over from an older one
    bool ok = true;
    for (const ProfileKey& k : profile.keys)
        ok &= WritePrivateProfileStringW(k.section.c_str(), k.key.c_str(), k.value.c_str(), file.c_str()) != 0;
    return ok;
}

bool delete_profile(const std::wstring& folder, const std::wstring& name) {
    return valid_profile_name(name) && DeleteFileW((fs::path(folder) / (name + kExtension)).c_str()) != 0;
}

bool apply_profile(const wchar_t* ini, const Profile& profile) {
    bool ok = true;
    for (const ProfileKey& k : profile.keys)
        ok &= WritePrivateProfileStringW(k.section.c_str(), k.key.c_str(), k.value.c_str(), ini) != 0;
    return ok;
}

bool profile_matches(const wchar_t* ini, const Profile& profile) {
    // eye_scale is Two-loop's resolution and QVFR's fallback only; the headset menu leaves it as it was in QVFR.
    const bool qvfr = _wcsicmp(read_value(ini, L"experiment", L"mode").c_str(), L"views4") == 0;
    for (const ProfileKey& k : profile.keys)
        if (!(qvfr && k.section == L"experiment" && k.key == L"eye_scale") && read_value(ini, k.section, k.key) != k.value)
            return false;
    return !profile.keys.empty();
}

std::wstring current_profile(const wchar_t* ini) {
    const std::wstring name = read_value(ini, L"profiles", L"current");
    return name == L"\x01" ? std::wstring() : name;
}

bool set_current_profile(const wchar_t* ini, const std::wstring& name) {
    return WritePrivateProfileStringW(L"profiles", L"current", name.c_str(), ini) != 0;
}

std::string export_profile(const Profile& profile) {
    std::wstring text = profile.name + L"\n";
    std::wstring section;
    for (const ProfileKey& k : profile.keys) {
        if (k.section != section) {
            section = k.section;
            text += L"[" + section + L"]\n";
        }
        text += k.key + L"=" + k.value + L"\n";
    }
    return "QVFR1:" + base64(utf8(text));
}

bool import_profile(const std::string& in, Profile& profile) {
    std::string text = in;
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front())))
        text.erase(text.begin());
    if (text.rfind("QVFR1:", 0) != 0)
        return false;
    std::string bytes;
    if (!unbase64(text.substr(6), bytes))
        return false;
    const std::wstring lines = wide(bytes);
    Profile p;
    std::wstring section;
    size_t at = 0;
    bool first = true;
    while (at < lines.size()) {
        const size_t end = std::min(lines.find(L'\n', at), lines.size());
        const std::wstring line = trim(lines.substr(at, end - at));
        at = end + 1;
        if (first) {
            p.name = line;
            first = false;
        } else if (line.size() > 2 && line.front() == L'[' && line.back() == L']') {
            section = line.substr(1, line.size() - 2);
        } else if (const size_t eq = line.find(L'='); eq != std::wstring::npos && eq > 0 && !section.empty()) {
            p.keys.push_back({section, trim(line.substr(0, eq)), trim(line.substr(eq + 1))});
        } else if (!line.empty()) {
            return false;
        }
    }
    if (!valid_profile_name(p.name) || p.keys.empty())
        return false;
    profile = std::move(p);
    return true;
}

}  // namespace simqvfr
