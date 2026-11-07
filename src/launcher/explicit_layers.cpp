#include "explicit_layers.h"

#include <filesystem>
#include <sstream>

#include "launch_plan.h"
#include "layer_order.h"

namespace simqvfr::launcher {
namespace fs = std::filesystem;

namespace {

constexpr char kLedgerHeader[] = "Sim-QVFR OpenXR registrations";

uint64_t ticks(const FILETIME& t) { return (static_cast<uint64_t>(t.dwHighDateTime) << 32) | t.dwLowDateTime; }

// Written whole or not at all (a temporary file moved over the old one): a ledger is never half a list.
bool write_whole(const std::wstring& path, const std::string& text) {
    const std::wstring temporary = path + L".new";
    if (!write_file(temporary, text))
        return false;
    if (MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        return true;
    DeleteFileW(temporary.c_str());
    return false;
}

bool read_stamp(std::istringstream& in, ProcessStamp& p) {
    unsigned long pid = 0;
    unsigned long long created = 0;
    if (!(in >> pid >> created))
        return false;
    p = {static_cast<DWORD>(pid), created};
    return true;
}

std::wstring status_text(LSTATUS s) { return L"error " + std::to_wstring(s); }

}  // namespace

ProcessStamp process_stamp(HANDLE process) {
    ProcessStamp p;
    p.pid = GetProcessId(process);
    FILETIME created{}, exited{}, kernel{}, user{};
    if (GetProcessTimes(process, &created, &exited, &kernel, &user))
        p.created = ticks(created);
    return p;
}

bool stamp_running(const ProcessStamp& p) {
    if (!p.pid)
        return false;
    const HANDLE process = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, p.pid);
    if (!process)
        return GetLastError() == ERROR_ACCESS_DENIED;  // there, but not ours to look at: assume it still runs
    const bool same = process_stamp(process).created == p.created;
    const bool running = same && WaitForSingleObject(process, 0) == WAIT_TIMEOUT;
    CloseHandle(process);
    return running;
}

std::string ledger_text(const Ledger& ledger) {
    std::string out = std::string(kLedgerHeader) + " (" + narrow(std::wstring(L"HKEY_LOCAL_MACHINE\\") + kExplicitLayersKey) +
                      "): the values added for one launch, removed when its game exits\r\n";
    out += "launcher " + std::to_string(ledger.launcher.pid) + " " + std::to_string(ledger.launcher.created) + "\r\n";
    if (ledger.game.pid)
        out += "game " + std::to_string(ledger.game.pid) + " " + std::to_string(ledger.game.created) + "\r\n";
    for (const std::wstring& v : ledger.values)
        out += "value " + narrow(v) + "\r\n";
    return out;
}

bool parse_ledger(const std::string& text, Ledger& out) {
    out = {};
    std::istringstream lines(text);
    std::string line;
    if (!std::getline(lines, line) || line.rfind(kLedgerHeader, 0) != 0)
        return false;
    while (std::getline(lines, line)) {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (line.empty())
            continue;
        std::istringstream in(line);
        std::string word;
        in >> word;
        if (word == "launcher" || word == "game") {
            if (!read_stamp(in, word == "game" ? out.game : out.launcher))
                return false;
        } else if (word == "value" && line.size() > 6) {
            out.values.push_back(widen(line.substr(6)));
        } else {
            return false;
        }
    }
    return true;
}

bool register_layers(HKEY root, const wchar_t* subkey, bool machine, const std::vector<std::wstring>& manifests,
                     const std::wstring& ledgerPath, const std::wstring& backup, std::vector<std::wstring>& notes,
                     std::wstring& error) {
    const std::wstring where = std::wstring(machine ? L"HKLM\\" : L"HKCU\\") + subkey;
    HKEY key = nullptr;
    const bool existed = RegOpenKeyExW(root, subkey, 0, KEY_QUERY_VALUE | KEY_WOW64_64KEY, &key) == ERROR_SUCCESS;
    if (existed) {
        RegCloseKey(key);
        if (!backup.empty()) {
            if (!backup_key(root, subkey, machine, backup)) {
                error = L"cannot write the backup " + backup + L"; nothing was registered";
                return false;
            }
            notes.push_back(L"backup of " + where + L" as it was: " + backup);
        }
    }
    Ledger ledger;
    ledger.launcher = process_stamp(GetCurrentProcess());
    std::error_code ec;
    fs::create_directories(fs::path(ledgerPath).parent_path(), ec);
    if (!write_whole(ledgerPath, ledger_text(ledger))) {
        error = L"cannot write " + ledgerPath + L"; nothing was registered";
        return false;
    }
    // Opens the key as it is; creates it only if it doesn't exist (never replaces it).
    DWORD disposition = 0;
    const LSTATUS created = RegCreateKeyExW(root, subkey, 0, nullptr, REG_OPTION_NON_VOLATILE,
                                            KEY_QUERY_VALUE | KEY_SET_VALUE | KEY_WOW64_64KEY, nullptr, &key, &disposition);
    if (created != ERROR_SUCCESS) {
        DeleteFileW(ledgerPath.c_str());
        error = (created == ERROR_ACCESS_DENIED ? L"administrator rights are needed to register in " : L"cannot open ") + where +
                L" (" + status_text(created) + L"); nothing was registered";
        return false;
    }
    if (disposition == REG_CREATED_NEW_KEY)
        notes.push_back(L"created " + where + L" (it didn't exist; it stays, empty, after the launch)");
    bool ok = true;
    for (const std::wstring& manifest : manifests) {
        if (RegQueryValueExW(key, manifest.c_str(), nullptr, nullptr, nullptr, nullptr) == ERROR_SUCCESS) {
            notes.push_back(L"already registered, left as it is (not ours): " + manifest);
            continue;
        }
        ledger.values.push_back(manifest);
        if (!write_whole(ledgerPath, ledger_text(ledger))) {
            ledger.values.pop_back();
            error = L"cannot write " + ledgerPath;
            ok = false;
            break;
        }
        const DWORD on = 0;  // the loader's convention: 0 is enabled
        const LSTATUS set = RegSetValueExW(key, manifest.c_str(), 0, REG_DWORD, reinterpret_cast<const BYTE*>(&on), sizeof(on));
        if (set != ERROR_SUCCESS) {
            error = L"cannot add " + manifest + L" to " + where + L" (" + status_text(set) + L")";
            ok = false;
            break;
        }
        notes.push_back(L"registered for this launch: " + manifest);
    }
    RegCloseKey(key);
    if (!ok) {
        unregister_layers(root, subkey, ledgerPath, notes);
        return false;
    }
    return true;
}

bool ledger_game_started(const std::wstring& ledgerPath, const ProcessStamp& game) {
    std::string text;
    Ledger ledger;
    if (!read_file(ledgerPath, text) || !parse_ledger(text, ledger))
        return false;
    ledger.game = game;
    return write_whole(ledgerPath, ledger_text(ledger));
}

bool unregister_layers(HKEY root, const wchar_t* subkey, const std::wstring& ledgerPath, std::vector<std::wstring>& notes) {
    std::string text;
    if (!read_file(ledgerPath, text))
        return true;  // nothing listed
    Ledger ledger;
    if (!parse_ledger(text, ledger)) {
        notes.push_back(L"not a ledger, left alone: " + ledgerPath);
        return false;
    }
    HKEY key = nullptr;
    const LSTATUS opened = RegOpenKeyExW(root, subkey, 0, KEY_QUERY_VALUE | KEY_SET_VALUE | KEY_WOW64_64KEY, &key);
    if (opened == ERROR_FILE_NOT_FOUND) {
        notes.push_back(L"the key is gone, and with it the values listed in " + ledgerPath);
        DeleteFileW(ledgerPath.c_str());
        return true;
    }
    if (opened != ERROR_SUCCESS) {
        notes.push_back(L"cannot open the key to take out what " + ledgerPath + L" lists (" + status_text(opened) +
                        L"); it stays listed for the next start as administrator");
        return false;
    }
    std::vector<std::wstring> left;  // still ours, not taken out
    for (const std::wstring& v : ledger.values) {
        DWORD type = 0, data = 1, size = sizeof(data);
        const LSTATUS q = RegQueryValueExW(key, v.c_str(), nullptr, &type, reinterpret_cast<BYTE*>(&data), &size);
        if (q == ERROR_FILE_NOT_FOUND) {
            notes.push_back(L"already gone: " + v);
        } else if (q != ERROR_SUCCESS || type != REG_DWORD || size != sizeof(data) || data != 0) {
            notes.push_back(L"changed since it was added, left as it is: " + v);
        } else if (const LSTATUS d = RegDeleteValueW(key, v.c_str()); d == ERROR_SUCCESS || d == ERROR_FILE_NOT_FOUND) {
            notes.push_back(L"taken out: " + v);
        } else {
            notes.push_back(L"cannot take out " + v + L" (" + status_text(d) + L"); it stays listed");
            left.push_back(v);
        }
    }
    RegCloseKey(key);
    if (left.empty()) {
        DeleteFileW(ledgerPath.c_str());
        return true;
    }
    ledger.values = left;
    write_whole(ledgerPath, ledger_text(ledger));
    return false;
}

void sweep_ledgers(HKEY root, const wchar_t* subkey, const std::wstring& folder, std::vector<std::wstring>& notes,
                   const std::function<bool(const ProcessStamp&)>& running) {
    std::error_code ec;
    for (fs::directory_iterator it(folder, ec), end; !ec && it != end; it.increment(ec)) {
        if (!it->is_regular_file(ec) || it->path().extension() != L".txt")
            continue;
        std::string text;
        Ledger ledger;
        if (!read_file(it->path().wstring(), text) || !parse_ledger(text, ledger))
            continue;
        if (running(ledger.game.pid ? ledger.game : ledger.launcher))
            continue;  // its launch is still on: its own launcher or waiting copy takes them out
        notes.push_back(L"left from an earlier launch: " + it->path().wstring());
        unregister_layers(root, subkey, it->path().wstring(), notes);
    }
}

}  // namespace simqvfr::launcher
