#include "game.h"

#include <windows.h>
#include <tlhelp32.h>

#include <algorithm>
#include <cstring>
#include <cwchar>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <sstream>
#include <vector>

#include "ace/game.h"
#include "acc/game.h"
#include "lmu/game.h"
#include "scssoftware/ats/game.h"
#include "scssoftware/ets2/game.h"
#include "xplane/game.h"
#include "../json.h"
#include "../module_path.h"

namespace simqvfr::games {

namespace {

// Every game Sim-QVFR supports. A new game adds its game.h here (src/games/README.md).
constexpr const GameInfo* kGames[] = {&::ace::kGame, &::acc::kGame, &::xplane::kGame, &::lmu::kGame, &::ats::kGame,
                                     &::ets2::kGame};

const GameInfo* chosen = &::ace::kGame;

// ---- Games from mods ----

constexpr std::size_t kModGamesMax = 32;

// One game a mod adds, with the strings its GameInfo points at.
struct OwnedGame {
    std::string id;
    std::wstring name, shortName, exe, steamFolder, exeFolder, steamAppId, launchArgs;
    std::vector<std::wstring> antiCheat;
    std::vector<const wchar_t*> antiCheatList;  // null-terminated, into antiCheat
    GameInfo info{};
    ModGame mod;
};

std::mutex modGamesLock;
bool modGamesRead = false;
std::deque<OwnedGame> modGames;           // never shrinks: GameInfo pointers stay valid for the process
std::vector<const GameInfo*> listed = [] {  // kGames, then modGames' (room reserved: the pointer all_games hands out
    std::vector<const GameInfo*> v;          // stays valid while a game is added)
    v.reserve(std::size(kGames) + kModGamesMax);
    v.assign(std::begin(kGames), std::end(kGames));
    return v;
}();

std::wstring widen(const std::string& s) {
    if (s.empty())
        return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<std::size_t>(n > 0 ? n : 0), L'\0');
    if (n > 0)
        MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

// A shown or path text: no control characters, at most `max`.
std::wstring clean(const std::string& s, std::size_t max) {
    std::wstring w;
    for (wchar_t c : widen(s))
        if (c >= 0x20)
            w += c;
    if (w.size() > max)
        w.resize(max);
    return w;
}

bool name_ok(const std::string& s) {
    if (s.empty() || s.size() > 32)
        return false;
    for (char c : s)
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-'))
            return false;
    return true;
}

// A file name (no folder) ending in .exe.
bool exe_ok(const std::wstring& w) {
    return w.size() > 4 && w.size() <= 64 && w.find_first_of(L"\\/:*?\"<>|") == std::wstring::npos &&
           _wcsicmp(w.c_str() + w.size() - 4, L".exe") == 0;
}

// A relative folder: no drive, no "..", no leading separator.
bool folder_ok(const std::wstring& w) {
    return w.size() <= 120 && w.find(L':') == std::wstring::npos && w.find(L"..") == std::wstring::npos &&
           (w.empty() || (w[0] != L'\\' && w[0] != L'/'));
}

const GameInfo* listed_by_id(const char* id) {
    for (const GameInfo* g : listed)
        if (_stricmp(id, g->id) == 0)
            return g;
    return nullptr;
}

const GameInfo* listed_by_exe(const wchar_t* exe) {
    for (const GameInfo* g : listed)
        if (_wcsicmp(exe, g->exeName) == 0)
            return g;
    return nullptr;
}

GameFeatures features_of(const json::Value* f) {
    GameFeatures out{};
    if (!f || f->type != json::Value::Type::Object)
        return out;
    const auto on = [f](const char* key) {
        const json::Value* v = f->find(key);
        return v && v->type == json::Value::Type::Bool && v->boolean;
    };
    // What a mod's game can offer: the core's own features. Those tied to one game's code (Cheeky's DLSS, the road
    // focus, car lighting, object detail…) aren't offered.
    out.liveResolution = on("liveResolution");
    out.helmet = on("helmet");
    out.fovCrop = on("fovCrop");
    out.sharpening = on("sharpening");
    out.focusMask = on("focusMask");
    out.eyeOff = on("eyeOff");
    out.multiView = on("multiView");
    out.layerMultiView = on("layerMultiView");
    return out;
}

QvfrImage image_of(const std::string& s) {
    if (s == "eyeColumns")
        return QvfrImage::EyeColumns;
    if (s == "oneViewSize")
        return QvfrImage::OneViewSize;
    if (s == "separateViews")
        return QvfrImage::SeparateViews;
    return QvfrImage::DoubleWide;
}

// The new games in one mod's mod.json (caller holds modGamesLock).
int add_games_of(const std::filesystem::path& modFolder) {
    std::error_code ec;
    const std::filesystem::path manifest = modFolder / L"mod.json";
    if (!std::filesystem::is_regular_file(manifest, ec) || std::filesystem::file_size(manifest, ec) > 256 * 1024)
        return 0;
    std::ifstream in(manifest, std::ios::binary);
    std::ostringstream text;
    text << in.rdbuf();
    json::Value root;
    if (!json::parse(text.str(), root) || root.type != json::Value::Type::Object)
        return 0;
    const std::string modId = root.string_at("id");
    const std::wstring folderName = modFolder.filename().wstring();
    if (!name_ok(modId) || _wcsicmp(widen(modId).c_str(), folderName.c_str()) != 0)
        return 0;
    const std::wstring dll = clean(root.string_at("dll"), 64);
    if (dll.size() <= 4 || dll.find_first_of(L"\\/:") != std::wstring::npos || dll.find(L"..") != std::wstring::npos ||
        _wcsicmp(dll.c_str() + dll.size() - 4, L".dll") != 0)
        return 0;
    const json::Value* list = root.find("games");
    if (!list || list->type != json::Value::Type::Array)
        return 0;
    int added = 0;
    for (const json::Value& g : list->items) {
        if (g.type != json::Value::Type::Object || modGames.size() >= kModGamesMax)
            continue;
        const std::string id = g.string_at("id");
        const std::wstring exe = clean(g.string_at("exe"), 64);
        // An entry without an exe names a game Sim-QVFR has: the mod's settings show there; nothing to add.
        if (!name_ok(id) || exe.empty() || !exe_ok(exe) || listed_by_id(id.c_str()) || listed_by_exe(exe.c_str()))
            continue;
        OwnedGame& o = modGames.emplace_back();
        o.id = id;
        o.name = clean(g.string_at("name"), 60);
        if (o.name.empty())
            o.name = widen(id);
        o.shortName = clean(g.string_at("short"), 8);
        if (o.shortName.empty())
            o.shortName = o.name.substr(0, 4);
        o.exe = exe;
        o.steamFolder = clean(g.string_at("steamFolder"), 120);
        o.exeFolder = clean(g.string_at("exeFolder"), 120);
        if (!folder_ok(o.steamFolder) || !folder_ok(o.exeFolder))
            o.steamFolder.clear(), o.exeFolder.clear();
        o.steamAppId = clean(g.string_at("steamAppId"), 12);
        for (wchar_t c : o.steamAppId)
            if (c < L'0' || c > L'9') {
                o.steamAppId.clear();
                break;
            }
        o.launchArgs = clean(g.string_at("launchArgs"), 200);
        if (const json::Value* ac = g.find("antiCheat"); ac && ac->type == json::Value::Type::Array)
            for (const json::Value& a : ac->items)
                if (a.type == json::Value::Type::String && o.antiCheat.size() < 4 && exe_ok(clean(a.text, 64)))
                    o.antiCheat.push_back(clean(a.text, 64));
        for (const std::wstring& a : o.antiCheat)
            o.antiCheatList.push_back(a.c_str());
        o.antiCheatList.push_back(nullptr);
        o.info.id = o.id.c_str();
        o.info.name = o.name.c_str();
        o.info.shortName = o.shortName.c_str();
        o.info.exeName = o.exe.c_str();
        o.info.steamFolder = o.steamFolder.c_str();
        o.info.exeFolder = o.exeFolder.c_str();
        o.info.steamAppId = o.steamAppId.c_str();
        o.info.launchArgs = o.launchArgs.c_str();
        o.info.openComposite = nullptr;
        o.info.dlssDll = nullptr;
        o.info.features = features_of(g.find("features"));
        o.info.builds = nullptr;
        o.info.buildCount = 0;
        o.info.antiCheat = o.antiCheat.empty() ? nullptr : o.antiCheatList.data();
        o.info.anyBuild = true;  // the mod knows which builds it runs on, not Sim-QVFR
        o.info.qvfrImage = image_of(g.string_at("image"));
        o.mod.modId = folderName;
        o.mod.dll = dll;
        o.mod.game = &o.info;
        listed.push_back(&o.info);
        ++added;
    }
    return added;
}

int add_from(const std::wstring& modsFolder) {
    std::error_code ec;
    if (modsFolder.empty() || !std::filesystem::is_directory(modsFolder, ec))
        return 0;
    std::vector<std::filesystem::path> folders;
    for (std::filesystem::directory_iterator it(modsFolder, ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code e;
        if (it->is_directory(e) && !it->is_symlink(e) && it->path().filename().wstring()[0] != L'.')
            folders.push_back(it->path());
    }
    std::sort(folders.begin(), folders.end());
    int added = 0;
    for (const std::filesystem::path& f : folders)
        added += add_games_of(f);
    return added;
}

// mods\ beside this module (the launcher's exe, or the core: its folder's parent when that is "layer").
std::wstring own_mods_folder() {
    const std::filesystem::path here = module_folder(reinterpret_cast<const void*>(&own_mods_folder));
    const bool inLayer = _wcsicmp(here.filename().c_str(), L"layer") == 0;
    return ((inLayer ? here.parent_path() : here) / L"mods").wstring();
}

// The games, mods' read once (caller holds modGamesLock).
const std::vector<const GameInfo*>& games_locked() {
    if (!modGamesRead) {
        modGamesRead = true;
        add_from(own_mods_folder());
    }
    return listed;
}

}  // namespace

const GameInfo* const* all_games(std::size_t& count) {
    std::lock_guard<std::mutex> lock(modGamesLock);
    const auto& v = games_locked();
    count = v.size();
    return v.data();
}

const GameInfo* game_by_id(const char* id) {
    std::lock_guard<std::mutex> lock(modGamesLock);
    for (const GameInfo* g : games_locked())
        if (id && _stricmp(id, g->id) == 0)
            return g;
    return nullptr;
}

const ModGame* mod_game(const GameInfo* game) {
    std::lock_guard<std::mutex> lock(modGamesLock);
    for (const OwnedGame& o : modGames)
        if (&o.info == game)
            return &o.mod;
    return nullptr;
}

int rescan_mod_games() {
    std::lock_guard<std::mutex> lock(modGamesLock);
    games_locked();
    return add_from(own_mods_folder());
}

int add_mod_games_from(const std::wstring& modsFolder) {
    std::lock_guard<std::mutex> lock(modGamesLock);
    games_locked();
    return add_from(modsFolder);
}

const GameInfo& launcher_game() { return *chosen; }

void set_launcher_game(const GameInfo& game) { chosen = &game; }

const GameInfo* game_for_exe(const wchar_t* fileName) {
    std::lock_guard<std::mutex> lock(modGamesLock);
    for (const GameInfo* g : games_locked())
        if (fileName && _wcsicmp(fileName, g->exeName) == 0)
            return g;
    return nullptr;
}

const GameInfo* current_game() {
    wchar_t path[MAX_PATH] = {};
    const DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH)
        return nullptr;
    const wchar_t* name = wcsrchr(path, L'\\');
    return game_for_exe(name ? name + 1 : path);
}

bool supported_build(const GameInfo& game, const std::string& sha256) {
    for (std::size_t i = 0; i < game.buildCount; ++i)
        if (sha256 == game.builds[i])
            return true;
    return false;
}

bool anticheat_chosen(const wchar_t* ini) {
    return ini && *ini && GetPrivateProfileIntW(L"launcher", L"anticheat", 0, ini) != 0;
}

bool anticheat_running(const GameInfo& game, std::wstring& found) {
    found.clear();
    if (!game.antiCheat)
        return false;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE)
        return false;
    const DWORD self = GetCurrentProcessId();
    DWORD parent = 0;
    PROCESSENTRY32W e = {sizeof(e)};
    for (BOOL ok = Process32FirstW(snap, &e); ok; ok = Process32NextW(snap, &e))
        if (e.th32ProcessID == self)
            parent = e.th32ParentProcessID;
    e = {sizeof(e)};
    for (BOOL ok = Process32FirstW(snap, &e); ok && found.empty(); ok = Process32NextW(snap, &e))
        for (const wchar_t* const* name = game.antiCheat; *name; ++name)
            if (_wcsicmp(e.szExeFile, *name) == 0) {
                // A bootstrapper that is still up, anywhere, counts too: it may have started this process and its
                // parent link can't be told from a reused PID.
                found = std::wstring(e.szExeFile) + (e.th32ProcessID == parent ? L" (started this game)" : L"");
                break;
            }
    CloseHandle(snap);
    return !found.empty();
}

}  // namespace simqvfr::games
