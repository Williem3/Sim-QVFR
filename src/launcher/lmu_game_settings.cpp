// LMU's own graphics settings in its files (lmu_game_settings.h): the table, a small reader and in-place writer for its
// ini and its JSON, and the files' safe replacement.
#include "lmu_game_settings.h"

#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iterator>

#include <windows.h>

#include "../i18n.h"

namespace simqvfr::launcher::lmu_settings {

namespace {

// The values as LMU's own comments in Settings.JSON name them ("0=Low 1=Medium 2=High 3=Full"); the ini's post effects
// and MSAA as the game's menu writes them (post effects has no Off: Low is 0; MSAA the sample count).
constexpr Option kPost[] = {{"0", I18N(L"Low")}, {"1", I18N(L"Medium")}, {"2", I18N(L"High")}, {"3", I18N(L"Ultra")}};
constexpr Option kMsaa[] = {{"0", I18N(L"Off")}, {"2", L"2x"}, {"4", L"4x"}, {"8", L"8x"}};
constexpr Option kDetail[] = {{"0", I18N(L"Low")}, {"1", I18N(L"Medium")}, {"2", I18N(L"High")}, {"3", I18N(L"Full")}};
constexpr Option kTexture[] = {{"0", I18N(L"Low")}, {"1", I18N(L"Medium")}, {"2", I18N(L"High")}, {"3", I18N(L"Max")}};
constexpr Option kFilter[] = {{"0", I18N(L"Bilinear")}, {"1", I18N(L"Trilinear")}, {"2", L"2x AF"},
                              {"3", L"4x AF"},          {"4", L"8x AF"},            {"5", L"16x AF"}};
constexpr Option kOffToUltra[] = {{"0", I18N(L"Off")},  {"1", I18N(L"Low")},  {"2", I18N(L"Medium")},
                                  {"3", I18N(L"High")}, {"4", I18N(L"Ultra")}};
constexpr Option kShadowBlur[] = {{"0", I18N(L"Off")}, {"1", I18N(L"Fast")}, {"2", I18N(L"Optimal")}, {"3", I18N(L"Quality")}};
constexpr Option kSoftParticles[] = {{"0", I18N(L"Off")}, {"1", I18N(L"Cheap soft edges")}, {"2", I18N(L"Depth-buffered")}};
constexpr Option kMotionBlur[] = {{"0", I18N(L"Off")}, {"1", I18N(L"Low")}, {"2", I18N(L"Medium")}, {"3", I18N(L"High")}};
constexpr Option kSharpening[] = {{"0", I18N(L"Off")},          {"1", I18N(L"Very blurry")}, {"2", I18N(L"Blurry")},
                                  {"5", I18N(L"Auto")},         {"3", I18N(L"Sharp")},       {"4", I18N(L"Very sharp")}};
constexpr Option kLiveTv[] = {{"0", I18N(L"Off")}, {"1", I18N(L"Trackside cameras only")}, {"2", I18N(L"On")}};
constexpr Option kPitcrew[] = {{"0", I18N(L"Off")}, {"1", I18N(L"Player only")}, {"2", I18N(L"Everybody")}};
constexpr Option kVehicles[] = {{"5", L"5"},   {"10", L"10"}, {"16", L"16"}, {"20", L"20"},
                                {"30", L"30"}, {"40", L"40"}, {"50", L"50"}, {"60", L"60"}};
constexpr Option kHeadlights[] = {{"0", L"0"}, {"5", L"5"}, {"10", L"10"}, {"15", L"15"}, {"20", L"20"}};
// Rearview_Back_Clip, metres, written as LMU writes it ("150.0"); 0.0 = the scene's own default.
constexpr Option kMirrorDistance[] = {{"0.0", I18N(L"Circuit’s default")}, {"50.0", L"50 m"},   {"75.0", L"75 m"},
                                      {"100.0", L"100 m"},                 {"150.0", L"150 m"}, {"200.0", L"200 m"},
                                      {"300.0", L"300 m"},                 {"500.0", L"500 m"}};
// Auto Detail Framerate: the fps under which LMU lowers detail and visible cars (by up to half); 0 = off.
constexpr Option kAutoDetail[] = {{"0", I18N(L"Off")}, {"45", L"45 fps"},  {"60", L"60 fps"},  {"72", L"72 fps"},
                                  {"80", L"80 fps"},   {"90", L"90 fps"},  {"120", L"120 fps"}};
// UI Refresh Rate: the most times a second LMU redraws its interface in a race.
constexpr Option kUiRate[] = {{"15", L"15 Hz"}, {"20", L"20 Hz"}, {"30", L"30 Hz"}, {"45", L"45 Hz"}, {"60", L"60 Hz"}};

template <std::size_t N>
Def choice(File f, const char* key, const wchar_t* label, const wchar_t* help, int group, const Option (&options)[N]) {
    return {f, key, Kind::Choice, label, help, group, options, N};
}

Def toggle(File f, const char* key, const wchar_t* label, const wchar_t* help, int group) {
    return {f, key, Kind::Toggle, label, help, group};
}

bool is_space(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

// ---- JSON: just enough to find a member's value and replace its text ----

std::size_t skip_ws(const std::string& t, std::size_t i) {
    while (i < t.size() && is_space(t[i]))
        ++i;
    return i;
}

// From an opening quote to after its closing one.
std::size_t skip_string(const std::string& t, std::size_t i) {
    for (++i; i < t.size(); ++i) {
        if (t[i] == '\\')
            ++i;
        else if (t[i] == '"')
            return i + 1;
    }
    return t.size();
}

// A value's end: a string, an object or array (to its matching close), or a literal or number.
std::size_t skip_value(const std::string& t, std::size_t i) {
    if (i >= t.size())
        return i;
    if (t[i] == '"')
        return skip_string(t, i);
    if (t[i] == '{' || t[i] == '[') {
        int depth = 0;
        while (i < t.size()) {
            const char c = t[i];
            if (c == '"') {
                i = skip_string(t, i);
                continue;
            }
            if (c == '{' || c == '[')
                ++depth;
            else if ((c == '}' || c == ']') && --depth == 0)
                return i + 1;
            ++i;
        }
        return t.size();
    }
    while (i < t.size() && !is_space(t[i]) && t[i] != ',' && t[i] != '}' && t[i] != ']')
        ++i;
    return i;
}

// The member `key` of the object opening at `open`: its value's span. Members at that object's own level only.
bool member(const std::string& t, std::size_t open, const char* key, std::size_t& begin, std::size_t& end) {
    if (open >= t.size() || t[open] != '{')
        return false;
    const std::size_t keyLen = std::strlen(key);
    std::size_t i = open + 1;
    for (;;) {
        i = skip_ws(t, i);
        if (i >= t.size() || t[i] != '"')
            return false;  // '}' (not there) or not JSON we know
        const std::size_t ks = i, ke = skip_string(t, i);
        i = skip_ws(t, ke);
        if (i >= t.size() || t[i] != ':')
            return false;
        i = skip_ws(t, i + 1);
        const std::size_t vb = i, ve = skip_value(t, i);
        if (ke - ks == keyLen + 2 && t.compare(ks + 1, keyLen, key) == 0) {
            begin = vb;
            end = ve;
            return ve > vb;
        }
        i = skip_ws(t, ve);
        if (i >= t.size() || t[i] != ',')
            return false;
        ++i;
    }
}

// The "Graphic Options" member `key`'s value span in Settings.JSON's text.
bool json_span(const std::string& t, const char* key, std::size_t& begin, std::size_t& end) {
    std::size_t root = 0;
    if (t.compare(0, 3, "\xEF\xBB\xBF") == 0)
        root = 3;
    root = skip_ws(t, root);
    std::size_t gb = 0, ge = 0;
    if (!member(t, root, "Graphic Options", gb, ge))
        return false;
    return member(t, gb, key, begin, end);
}

// ---- The ini: [COMPONENTS] Key=value lines ----

bool same_text(const std::string& a, std::size_t from, std::size_t to, const char* b) {
    const std::size_t n = std::strlen(b);
    if (to - from != n)
        return false;
    for (std::size_t i = 0; i < n; ++i)
        if (std::tolower(static_cast<unsigned char>(a[from + i])) != std::tolower(static_cast<unsigned char>(b[i])))
            return false;
    return true;
}

// The value span of `key` in [COMPONENTS]: after '=', up to a "//" comment or the line's end, spaces at its end left out.
bool ini_span(const std::string& t, const char* key, std::size_t& begin, std::size_t& end) {
    bool inSection = false;
    std::size_t line = 0;
    while (line < t.size()) {
        std::size_t eol = t.find_first_of("\r\n", line);
        if (eol == std::string::npos)
            eol = t.size();
        std::size_t a = line;
        while (a < eol && (t[a] == ' ' || t[a] == '\t'))
            ++a;
        if (a < eol && t[a] == '[') {
            const std::size_t close = t.find(']', a);
            inSection = close != std::string::npos && close < eol && same_text(t, a + 1, close, "COMPONENTS");
        } else if (inSection) {
            const std::size_t eq = t.find('=', a);
            if (eq != std::string::npos && eq < eol) {
                std::size_t ke = eq;
                while (ke > a && (t[ke - 1] == ' ' || t[ke - 1] == '\t'))
                    --ke;
                if (same_text(t, a, ke, key)) {
                    std::size_t vb = eq + 1;
                    while (vb < eol && (t[vb] == ' ' || t[vb] == '\t'))
                        ++vb;
                    std::size_t ve = t.find("//", vb);
                    if (ve == std::string::npos || ve > eol)
                        ve = eol;
                    while (ve > vb && (t[ve - 1] == ' ' || t[ve - 1] == '\t'))
                        --ve;
                    begin = vb;
                    end = ve;
                    return true;
                }
            }
        }
        line = eol;
        while (line < t.size() && (t[line] == '\r' || t[line] == '\n'))
            ++line;
    }
    return false;
}

bool span(File file, const std::string& t, const char* key, std::size_t& begin, std::size_t& end) {
    return file == File::Config ? ini_span(t, key, begin, end) : json_span(t, key, begin, end);
}

bool number(const std::string& s, double& v) {
    if (s.empty())
        return false;
    char* stop = nullptr;
    v = std::strtod(s.c_str(), &stop);
    return stop && *stop == 0 && std::isfinite(v);
}

}  // namespace

const std::vector<Def>& defs() {
    static const std::vector<Def> list = {
        // Image quality (Config_DX11_VR.ini)
        choice(File::Config, "EPostProcessingSettings", I18N(L"Post effects"),
               I18N(L"Bloom, glare and the rest of LMU’s post-processing. Low has a known zoomed-periphery problem with "
                    L"MSAA off: use Medium or higher with Sim-QVFR."),
               0, kPost),
        choice(File::Config, "MSAA", L"MSAA",
               I18N(L"LMU’s own anti-aliasing. Turn it off when Sim-QVFR’s DLSS is on: DLSS then does all the "
                    L"anti-aliasing."),
               0, kMsaa),
        toggle(File::Config, "UseFXAA", L"FXAA", I18N(L"A cheap screen-space edge blur on top of the picture."), 0),
        toggle(File::Config, "VrMirror", I18N(L"Mirror on the monitor"),
               I18N(L"Shows the headset’s picture in LMU’s window on the monitor too. Off saves a little GPU time."), 0),
        // Detail (Settings.JSON)
        choice(File::Player, "Track Detail", I18N(L"Circuit detail"), I18N(L"How much of the circuit’s scenery is drawn."),
               1, kDetail),
        choice(File::Player, "Player Detail", I18N(L"Car detail"), I18N(L"The detail of your own car."), 1, kDetail),
        choice(File::Player, "Opponent Detail", I18N(L"Opponent detail"), I18N(L"The detail of the other cars."), 1,
               kDetail),
        choice(File::Player, "Texture Detail", I18N(L"Texture detail"), I18N(L"The textures’ resolution: costs video memory."),
               1, kTexture),
        choice(File::Player, "Texture Filter", I18N(L"Texture filtering"),
               I18N(L"Anisotropic filtering keeps the road and kerbs sharp at an angle."), 1, kFilter),
        choice(File::Player, "Texture Sharpening", I18N(L"LMU texture sharpening"),
               I18N(L"LMU’s own mip bias. With Sim-QVFR’s DLSS on, leave it on Auto and use DLSS’s Texture sharpening."),
               1, kSharpening),
        choice(File::Player, "Max Visible Vehicles", I18N(L"Visible cars"),
               I18N(L"The most cars LMU draws at once: fewer saves CPU time in a full field."), 1, kVehicles),
        choice(File::Player, "Pitcrew Detail Level", I18N(L"Pit crews"), I18N(L"Whose pit crews are drawn."), 1, kPitcrew),
        choice(File::Player, "Live TV Displays", I18N(L"Live TV screens"),
               I18N(L"The circuit’s big screens showing the race: each one is another view to draw."), 1, kLiveTv),
        // Lighting and effects
        choice(File::Player, "Shadows", I18N(L"Shadows"), I18N(L"Shadow detail."), 2, kOffToUltra),
        choice(File::Player, "Shadow Blur", I18N(L"Shadow blur"), I18N(L"How soft the shadows’ edges are."), 2,
               kShadowBlur),
        choice(File::Player, "Environment Reflections", I18N(L"Environment reflections"),
               I18N(L"Live reflections of the surroundings on the cars."), 2, kOffToUltra),
        choice(File::Player, "Road Reflections", I18N(L"Road reflections"),
               I18N(L"Reflections on a wet road and the heat haze."), 2, kOffToUltra),
        toggle(File::Player, "Screen Space Ambient Occlusion", I18N(L"Ambient occlusion (SSAO)"),
               I18N(L"Soft contact shadows in corners and under objects."), 2),
        choice(File::Player, "Special FX", I18N(L"Special effects"), I18N(L"Smoke, sparks, spray and dust."), 2,
               kOffToUltra),
        choice(File::Player, "Soft Particles", I18N(L"Soft particles"),
               I18N(L"Softens where smoke and spray meet the scenery."), 2, kSoftParticles),
        choice(File::Player, "Motion blur", I18N(L"Motion blur"), I18N(L"Usually off in VR."), 2, kMotionBlur),
        toggle(File::Player, "Headlights On Cars", I18N(L"Headlights light other cars"),
               I18N(L"Other cars lit by headlights at night."), 2),
        choice(File::Player, "Max Headlights", I18N(L"Headlights"),
               I18N(L"The most cars whose headlights light the scene (up to 20)."), 2, kHeadlights),
        toggle(File::Player, "Sun Occlusion", I18N(L"Sun occlusion"), I18N(L"Sunlight dimmed by the clouds."), 2),
        toggle(File::Player, "Wind and Crowd Motion", I18N(L"Wind and crowd motion"),
               I18N(L"Trees in the wind and moving crowds."), 2),
        toggle(File::Player, "Transparency AA", I18N(L"Transparency anti-aliasing"),
               I18N(L"Smooths the edges of fences, trees and grass."), 2),
        // Rear-view mirrors
        toggle(File::Player, "Rearview Driving", I18N(L"Mirrors"),
               I18N(L"The rear-view mirrors in the cockpit. Each mirror is another view to draw."), 3),
        toggle(File::Player, "Rearview Cull", I18N(L"Rear-view cull"),
               I18N(L"Leaves out of the mirrors what the circuit marks as not needed there: saves CPU and GPU time."), 3),
        choice(File::Player, "Rearview_Back_Clip", I18N(L"Mirror draw distance"),
               I18N(L"How far behind you the mirrors draw: shorter saves CPU and GPU time, but cars and scenery further "
                    L"back drop out of the mirrors."),
               3, kMirrorDistance),
        toggle(File::Player, "Rearview Particles", I18N(L"Particles in the mirrors"),
               I18N(L"Smoke and spray in the mirrors too."), 3),
        toggle(File::Player, "Virtual Rearview In Cockpit", I18N(L"Virtual mirror"),
               I18N(L"A virtual rear-view mirror at the top of the view, in the cockpit."), 3),
        // Virtual reality
        toggle(File::Player, "VR Enable Hidden Area Mask", I18N(L"Hidden area mask"),
               I18N(L"Skips the pixels the headset’s lenses never show."), 4),
        toggle(File::Player, "Vehicle Occlusion Culling", I18N(L"Vehicle occlusion culling"),
               I18N(L"Skips cars hidden behind other objects: saves CPU time in traffic."), 4),
        // Advanced performance
        choice(File::Player, "Auto Detail Framerate", I18N(L"Lower detail under"),
               I18N(L"When the frame rate drops below this, LMU lowers detail and visible cars by up to half. Scenery "
                    L"and cars can change detail mid-race. Set it to your headset’s refresh rate or a little under."),
               5, kAutoDetail),
        toggle(File::Player, "Load Opponent Cockpits", I18N(L"Opponents’ cockpits"),
               I18N(L"Loads the other cars’ gauges and screens. Off saves CPU time and video memory; leave it on if you "
                    L"swap cars in multiplayer."),
               5),
        choice(File::Player, "UI Refresh Rate", I18N(L"Interface refresh in a race"),
               I18N(L"How often LMU redraws its own interface while you drive. Lower can save CPU time on LMU’s main "
                    L"thread; the HUD updates less smoothly."),
               5, kUiRate),
    };
    return list;
}

std::wstring user_folder(const std::wstring& exe) {
    if (exe.empty())
        return L"";
    return (std::filesystem::path(exe).parent_path() / L"UserData").wstring();
}

std::wstring file_path(const std::wstring& userFolder, File file) {
    const std::filesystem::path u(userFolder);
    return (file == File::Config ? u / L"Config_DX11_VR.ini" : u / L"player" / L"Settings.JSON").wstring();
}

bool read_value(File file, const std::string& text, const char* key, std::string& value) {
    std::size_t b = 0, e = 0;
    if (!span(file, text, key, b, e))
        return false;
    value = text.substr(b, e - b);
    return true;
}

bool write_value(File file, std::string& text, const char* key, const std::string& value) {
    std::size_t b = 0, e = 0;
    if (!span(file, text, key, b, e))
        return false;
    text.replace(b, e - b, value);
    return true;
}

int option_index(const Def& d, const std::string& value) {
    double v = 0;
    const bool isNumber = number(value, v);
    for (std::size_t i = 0; i < d.count; ++i) {
        double o = 0;
        if (isNumber && number(d.options[i].value, o) ? std::fabs(o - v) < 1e-6 : value == d.options[i].value)
            return static_cast<int>(i);
    }
    return -1;
}

bool toggle_on(const std::string& value, bool& known) {
    known = true;
    if (value == "true" || value == "1")
        return true;
    if (value == "false" || value == "0")
        return false;
    known = false;
    return false;
}

std::string toggle_value(File file, bool on) {
    return file == File::Config ? (on ? "1" : "0") : (on ? "true" : "false");
}

bool parse_fov_scale(const std::string& value, double& x, double& y) {
    std::string s;
    for (char c : value)
        if (c != '(' && c != ')' && c != ' ')
            s += c;
    const std::size_t comma = s.find(',');
    return comma != std::string::npos && number(s.substr(0, comma), x) && number(s.substr(comma + 1), y);
}

// ---- Files ----

bool read_file(const std::wstring& path, std::string& bytes) {
    bytes.clear();
    const HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                 OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE)
        return false;
    LARGE_INTEGER size{};
    bool ok = GetFileSizeEx(f, &size) && size.QuadPart >= 0 && size.QuadPart < (16 << 20);
    if (ok) {
        bytes.resize(static_cast<std::size_t>(size.QuadPart));
        DWORD got = 0;
        ok = bytes.empty() || (ReadFile(f, bytes.data(), static_cast<DWORD>(bytes.size()), &got, nullptr) && got == bytes.size());
    }
    CloseHandle(f);
    if (!ok)
        bytes.clear();
    return ok;
}

namespace {

bool read_only(const std::wstring& path) {
    const DWORD a = GetFileAttributesW(path.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_READONLY);
}

}  // namespace

Snapshot read_all(const std::wstring& userFolder) {
    Snapshot s;
    s.userFolder = userFolder;
    const std::wstring config = file_path(userFolder, File::Config), player = file_path(userFolder, File::Player);
    s.configOk = !userFolder.empty() && read_file(config, s.configText);
    s.playerOk = !userFolder.empty() && read_file(player, s.playerText);
    s.configReadOnly = s.configOk && read_only(config);
    s.playerReadOnly = s.playerOk && read_only(player);
    for (const Def& d : defs()) {
        std::string v;
        const std::string& text = d.file == File::Config ? s.configText : s.playerText;
        const bool there = read_value(d.file, text, d.key, v);
        s.values.push_back(there ? v : std::string());
        s.present.push_back(there);
    }
    if (s.configOk)
        read_value(File::Config, s.configText, "VRFOVScaling", s.fovScale);
    return s;
}

std::wstring backup_path(const std::wstring& path) { return path + L".sim-qvfr-backup"; }

bool has_backup(const std::wstring& path) {
    return GetFileAttributesW(backup_path(path).c_str()) != INVALID_FILE_ATTRIBUTES;
}

std::wstring replace_file(const std::wstring& path, const std::string& bytes) {
    if (read_only(path))
        return I18N(L"LMU’s file is read-only, so it was left as it is. Clear its read-only flag to change it here.");
    if (!has_backup(path) && !CopyFileW(path.c_str(), backup_path(path).c_str(), TRUE))
        return I18N(L"Couldn’t keep a copy of LMU’s file before changing it, so it was left as it is.");
    const std::wstring temp = path + L".sim-qvfr-new";
    const HANDLE f = CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE)
        return I18N(L"Couldn’t write next to LMU’s file, so it was left as it is.");
    DWORD wrote = 0;
    const bool ok = (bytes.empty() || (WriteFile(f, bytes.data(), static_cast<DWORD>(bytes.size()), &wrote, nullptr) &&
                                       wrote == bytes.size())) &&
                    FlushFileBuffers(f);
    CloseHandle(f);
    // ReplaceFileW keeps the file's attributes and permissions; MoveFileExW where it can't (another file system).
    if (!ok || (!ReplaceFileW(path.c_str(), temp.c_str(), nullptr, REPLACEFILE_IGNORE_MERGE_ERRORS, nullptr, nullptr) &&
                !MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))) {
        DeleteFileW(temp.c_str());
        return I18N(L"Couldn’t replace LMU’s file, so it was left as it is.");
    }
    return L"";
}

std::wstring restore_backup(const std::wstring& path) {
    std::string bytes;
    if (!read_file(backup_path(path), bytes))
        return I18N(L"There is no copy of LMU’s file to put back.");
    return replace_file(path, bytes);
}

std::wstring set_setting(const std::wstring& userFolder, File file, const char* key, const std::string& value) {
    const std::wstring path = file_path(userFolder, file);
    std::string text;
    if (!read_file(path, text))
        return I18N(L"Couldn’t read LMU’s file.");
    std::string before;
    if (read_value(file, text, key, before) && before == value)
        return L"";  // already so: nothing written
    if (!write_value(file, text, key, value))
        return I18N(L"LMU’s file has no such setting, so it was left as it is.");
    return replace_file(path, text);
}

}  // namespace simqvfr::launcher::lmu_settings
