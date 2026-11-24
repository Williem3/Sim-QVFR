#include "mod_update.h"

#include <algorithm>
#include <cstdlib>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <vector>

#include <windows.h>

#include "../json.h"
#include "../lang.h"
#include "../mod_api/mod_trust.h"
#include "../sha256_file.h"
#include "mod_catalog.h"
#include "update_check.h"

namespace simqvfr::launcher {

namespace {

namespace fs = std::filesystem;
using lang::tr;
using lang::trf;

constexpr unsigned long long kMaxModDownload = 64ull << 20;

std::string narrow(const std::wstring& w) {
    std::string s;
    for (wchar_t c : w)
        s += c < 0x80 ? static_cast<char>(c) : '?';
    return s;
}

std::wstring ascii_wide(const std::string& s) { return std::wstring(s.begin(), s.end()); }

std::string lower(std::string s) {
    for (char& c : s)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// A version fit for a folder's name: letters, digits, dots and dashes.
std::wstring version_part(const std::wstring& version) {
    std::wstring out;
    for (wchar_t c : version)
        if (std::iswalnum(c) || c == L'.' || c == L'-')
            out += c;
    return out.empty() ? L"previous" : out.substr(0, 40);
}

bool read_text(const fs::path& path, std::string& text) {
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return false;
    std::ostringstream s;
    s << in.rdbuf();
    text = s.str();
    return true;
}

// mods\.backup\<id>-<version> folders of mod `id`, newest first.
std::vector<fs::path> backups_of(const fs::path& mods, const std::wstring& id) {
    std::vector<std::pair<fs::file_time_type, fs::path>> found;
    std::error_code ec;
    const std::wstring prefix = id + L"-";
    for (fs::directory_iterator it(mods / L".backup", ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code e;
        const std::wstring name = it->path().filename().wstring();
        if (it->is_directory(e) && !it->is_symlink(e) && name.size() > prefix.size() && name.rfind(prefix, 0) == 0)
            found.emplace_back(it->last_write_time(e), it->path());
    }
    std::sort(found.begin(), found.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    std::vector<fs::path> out;
    for (auto& f : found)
        out.push_back(f.second);
    return out;
}

// Moves `from` to `to` (same volume: a rename).
bool move(const fs::path& from, const fs::path& to) { return MoveFileExW(from.c_str(), to.c_str(), 0) != 0; }

// The mod in place swapped for `incoming`: the mod in place into mods\.backup\<id>-<version> (older backups of it
// removed: one is kept), `incoming` into its place; on a failure the old one goes back.
InstallResult swap_in(const fs::path& mods, const std::wstring& id, const fs::path& incoming, const std::wstring& version) {
    InstallResult r;
    const fs::path target = mods / id, backups = mods / L".backup";
    std::error_code ec;
    fs::create_directories(backups, ec);
    fs::path backup = backups / (id + L"-" + version_part(version));
    for (int n = 2; fs::exists(backup, ec) && n < 100; ++n)
        backup = backups / (id + L"-" + version_part(version) + L"-" + std::to_wstring(n));
    const bool had = fs::exists(target, ec);
    if (had && !move(target, backup)) {
        r.error = tr(L"the mod’s folder couldn’t be moved aside (is a game running?)");
        return r;
    }
    if (!move(incoming, target)) {
        if (had)
            move(backup, target);
        r.error = tr(L"the new mod couldn’t be put in place; the old one is back");
        return r;
    }
    // One backup kept: the one just made. Older ones of this mod go (they're Sim-QVFR's own copies of its own mod).
    for (const fs::path& old : backups_of(mods, id))
        if (old != backup)
            fs::remove_all(old, ec);
    r.ok = true;
    r.backup = had ? backup.wstring() : std::wstring();
    return r;
}

}  // namespace

bool newest_mod_release(const std::string& text, const std::wstring& id, bool prereleases, ModRelease& release) {
    json::Value root;
    if (!json::parse(text, root) || root.type != json::Value::Type::Array || id.empty())
        return false;
    const std::string tagPrefix = "mod-" + narrow(id) + "-v", assetPrefix = "sqvfr-mod-" + narrow(id) + "-";
    bool found = false;
    for (const json::Value& r : root.items) {
        if (r.type != json::Value::Type::Object)
            continue;
        const json::Value* draft = r.find("draft");
        const json::Value* pre = r.find("prerelease");
        if ((draft && draft->boolean) || (!prereleases && pre && pre->boolean))
            continue;
        const std::string tag = r.string_at("tag_name");
        if (tag.rfind(tagPrefix, 0) != 0 || tag.size() == tagPrefix.size())
            continue;
        const std::string version = tag.substr(tagPrefix.size());
        if (!known_version(ascii_wide(version)))
            continue;
        const std::string zipName = assetPrefix + version + ".zip";
        const json::Value* assets = r.find("assets");
        if (!assets || assets->type != json::Value::Type::Array)
            continue;
        for (const json::Value& a : assets->items) {
            const std::string digest = a.string_at("digest"), url = a.string_at("browser_download_url");
            const json::Value* size = a.find("size");
            if (a.string_at("name") != zipName || digest.rfind("sha256:", 0) != 0 || digest.size() != 7 + 64 || !size ||
                size->type != json::Value::Type::Number || !trusted_download(url))
                continue;
            if (found && compare_versions(ascii_wide(version), release.version) <= 0)
                break;
            release.tag = ascii_wide(tag);
            release.version = ascii_wide(version);
            release.zipUrl = url;
            release.zipSha256 = lower(digest.substr(7));
            release.zipSize = std::strtoull(size->text.c_str(), nullptr, 10);
            found = true;
            break;
        }
    }
    return found;
}

int compare_mod_versions(const std::wstring& installed, const std::wstring& release) {
    const bool a = known_version(installed), b = known_version(release);
    if (!a || !b)
        return a == b ? 0 : (a ? 1 : -1);
    return compare_versions(installed, release);
}

bool find_mod_backup(const std::wstring& modsFolder, const std::wstring& id, std::wstring& folder, std::wstring& version) {
    const std::vector<fs::path> list = backups_of(modsFolder, id);
    if (list.empty())
        return false;
    folder = list.front().wstring();
    std::string text;
    ModEntry m;
    m.id = id;
    std::wstring error;
    version = read_text(list.front() / L"mod.json", text) && parse_mod_manifest(text, m, error) ? m.version : std::wstring();
    if (version.empty())
        version = list.front().filename().wstring().substr(id.size() + 1);
    return true;
}

std::wstring check_staged_mod(const std::wstring& stagedText, const std::wstring& id) {
    const fs::path staged = stagedText;
    std::string text;
    if (!read_text(staged / L"SHA256.txt", text))
        return tr(L"the mod’s zip has no SHA256.txt");
    const std::map<std::wstring, std::string> list = parse_sha256_list(text);
    if (list.empty())
        return tr(L"the mod’s SHA256.txt lists nothing");
    for (const auto& [path, hash] : list) {
        if (path.find(L"..") != std::wstring::npos || fs::path(path).is_absolute())
            return tr(L"the mod’s SHA256.txt names a file outside it");
        if (lower(sha256_file((staged / path).c_str())) != lower(hash))
            return trf(L"{file} isn’t as its SHA256.txt says", {{L"file", path}});
    }
    std::error_code ec;
    for (fs::recursive_directory_iterator it(staged, ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code e;
        if (!it->is_regular_file(e))
            continue;
        std::wstring rel = fs::relative(it->path(), staged, e).wstring();
        std::replace(rel.begin(), rel.end(), L'/', L'\\');
        if (_wcsicmp(rel.c_str(), L"SHA256.txt") != 0 && list.find(rel) == list.end())
            return trf(L"{file} isn’t in the mod’s SHA256.txt", {{L"file", rel}});
    }
    if (list.find(L"mod.json") == list.end() || !read_text(staged / L"mod.json", text))
        return tr(L"the mod’s zip has no mod.json");
    ModEntry m;
    m.id = id;
    std::wstring error;
    if (!parse_mod_manifest(text, m, error))
        return trf(L"its mod.json: {why}", {{L"why", error}});
    if (list.find(m.dll) == list.end())
        return tr(L"the mod’s zip has no DLL its mod.json names");
    return {};
}

InstallResult install_mod_release(const std::wstring& modsFolder, const std::wstring& id, const ModRelease& release,
                                  const std::wstring& oldVersion, const std::function<void(const InstallProgress&)>& progress) {
    InstallResult r;
    const fs::path mods = modsFolder, work = mods / L".update", zip = work / (id + L".zip"), staged = work / (id + L"-staged");
    if (!trusted_download(release.zipUrl) || release.zipSha256.size() != 64 || release.zipSize == 0 ||
        release.zipSize > kMaxModDownload) {
        r.error = tr(L"the mod’s download isn’t one to trust");
        return r;
    }
    std::error_code ec;
    fs::remove_all(staged, ec);
    fs::create_directories(staged, ec);
    progress({tr(L"Downloading"), 0});
    if (!download_asset(release.zipUrl, zip.wstring(), release.zipSize, progress, r.error))
        return r;
    progress({tr(L"Checking the download"), 1});
    if (lower(sha256_file(zip.c_str())) != release.zipSha256) {
        r.error = tr(L"the download doesn’t match the release (its SHA-256 differs)");
        return r;
    }
    progress({tr(L"Unpacking"), 0});
    if (!unpack_zip(zip.wstring(), staged.wstring(), r.error))
        return r;
    r.error = check_staged_mod(staged.wstring(), id);
    if (!r.error.empty())
        return r;
    progress({tr(L"Installing"), 1});
    r = swap_in(mods, id, staged, oldVersion);
    fs::remove(zip, ec);
    return r;
}

StagedMod stage_mod_zip(const std::wstring& modsFolder, const std::wstring& zipText) {
    StagedMod s;
    const fs::path mods = modsFolder, zip = zipText, work = mods / L".update" / L"add";
    std::error_code ec;
    if (_wcsicmp(zip.extension().c_str(), L".zip") != 0 || !fs::is_regular_file(zip, ec)) {
        s.error = tr(L"that isn’t a .zip file");
        return s;
    }
    if (fs::file_size(zip, ec) > kMaxModDownload) {
        s.error = tr(L"the zip is larger than a mod can be (64 MB)");
        return s;
    }
    fs::remove_all(work, ec);
    fs::create_directories(work, ec);
    if (!unpack_zip(zip.wstring(), work.wstring(), s.error))
        return s;
    // The mod's folder: the zip's root when it has mod.json, else its only folder that does.
    fs::path root = work;
    if (!fs::is_regular_file(work / L"mod.json", ec)) {
        std::vector<fs::path> folders;
        for (fs::directory_iterator it(work, ec), end; !ec && it != end; it.increment(ec)) {
            std::error_code e;
            if (it->is_directory(e) && fs::is_regular_file(it->path() / L"mod.json", e))
                folders.push_back(it->path());
        }
        if (folders.size() != 1) {
            s.error = folders.empty() ? tr(L"the zip has no mod.json: it isn’t a Sim-QVFR mod")
                                      : tr(L"the zip holds more than one mod: add them one at a time");
            return s;
        }
        root = folders.front();
    }
    std::string text;
    ModEntry m;
    std::wstring error;
    if (!read_text(root / L"mod.json", text) || !parse_mod_manifest(text, m, error)) {
        s.error = trf(L"its mod.json: {why}", {{L"why", error}});
        return s;
    }
    if (!fs::is_regular_file(root / m.dll, ec)) {
        s.error = trf(L"the zip has no {dll}, which its mod.json names", {{L"dll", m.dll}});
        return s;
    }
    // A zip that claims the id of a mod Sim-QVFR ships would replace it, and be taken for it: only the maintainer's
    // signed DLL may (mod_api/mod_trust.h). A community mod picks its own id.
    if (mod_api::first_party_mod(m.id)) {
        const mod_api::ModFacts f = mod_api::read_mod_facts((root / m.dll).wstring());
        if (!f.signatureTrusted || !mod_api::official_thumbprint(f.thumbprint)) {
            s.error = trf(L"it uses the id “{id}” of a mod Sim-QVFR ships, and isn’t signed by Sim-QVFR",
                          {{L"id", m.id}});
            return s;
        }
    }
    if (fs::is_regular_file(root / L"SHA256.txt", ec)) {
        s.error = check_staged_mod(root.wstring(), m.id);
        if (!s.error.empty())
            return s;
    }
    s.folder = root.wstring();
    s.id = m.id;
    s.name = m.name;
    s.author = m.author;
    s.version = m.version;
    s.source = m.source;
    s.license = m.license;
    s.replaces = fs::exists(mods / m.id, ec);
    return s;
}

InstallResult add_staged_mod(const std::wstring& modsFolder, const StagedMod& staged, const std::wstring& oldVersion) {
    InstallResult r;
    if (!staged.error.empty() || staged.folder.empty() || staged.id.empty()) {
        r.error = staged.error.empty() ? tr(L"there is no mod to add") : staged.error;
        return r;
    }
    // Out of .update\add first (its own folder, so the swap moves one folder).
    const fs::path mods = modsFolder, incoming = mods / L".update" / (staged.id + L"-add");
    std::error_code ec;
    fs::remove_all(incoming, ec);
    if (!move(staged.folder, incoming)) {
        r.error = tr(L"the unpacked mod couldn’t be moved into the mods folder");
        return r;
    }
    r = swap_in(mods, staged.id, incoming, oldVersion);
    fs::remove_all(mods / L".update" / L"add", ec);
    return r;
}

InstallResult restore_mod_backup(const std::wstring& modsFolder, const std::wstring& id, const std::wstring& currentVersion) {
    InstallResult r;
    std::wstring folder, version;
    if (!find_mod_backup(modsFolder, id, folder, version)) {
        r.error = tr(L"there is no earlier version of this mod to go back to");
        return r;
    }
    // The backup out of .backup first (so the swap's own backup can't be confused with it), then swapped in.
    const fs::path mods = modsFolder, incoming = mods / L".update" / (id + L"-restore");
    std::error_code ec;
    fs::create_directories(mods / L".update", ec);
    fs::remove_all(incoming, ec);
    if (!move(folder, incoming)) {
        r.error = tr(L"the earlier version couldn’t be moved (is a game running?)");
        return r;
    }
    r = swap_in(mods, id, incoming, currentVersion);
    if (!r.ok)
        move(incoming, folder);
    return r;
}

}  // namespace simqvfr::launcher
