#include "update_check.h"

#include <cstdlib>
#include <cwctype>
#include <vector>

#include <windows.h>
#include <winhttp.h>

#include "json.h"
#include "launch_plan.h"

namespace simqvfr::launcher {
namespace {

// The version's leading numbers: up to the first character that is neither a digit nor a dot (after a "v").
std::vector<int> numbers(const std::wstring& version) {
    std::vector<int> out;
    size_t i = 0;
    while (i < version.size() && (std::iswspace(version[i]) || version[i] == L'v' || version[i] == L'V'))
        ++i;
    while (i < version.size() && std::iswdigit(version[i])) {  // none for "dev" and the like
        int n = 0;
        while (i < version.size() && std::iswdigit(version[i]))
            n = n * 10 + (version[i++] - L'0');
        out.push_back(n);
        if (i < version.size() && version[i] == L'.')
            ++i;
        else
            break;
    }
    return out;
}

}  // namespace

bool known_version(const std::wstring& version) { return !numbers(version).empty(); }

int compare_versions(const std::wstring& a, const std::wstring& b) {
    const std::vector<int> x = numbers(a), y = numbers(b);
    if (x.empty() || y.empty())
        return 0;
    for (size_t i = 0; i < std::max(x.size(), y.size()); ++i) {
        const int p = i < x.size() ? x[i] : 0, q = i < y.size() ? y[i] : 0;
        if (p != q)
            return p < q ? -1 : 1;
    }
    return 0;
}

namespace {

// The release's package: its "sim-qvfr-<version>.zip" asset with the SHA-256 GitHub gives it; null for none.
const json::Value* package_zip(const json::Value& release) {
    const json::Value* assets = release.find("assets");
    if (!assets || assets->type != json::Value::Type::Array)
        return nullptr;
    for (const json::Value& a : assets->items) {
        const std::string name = a.string_at("name"), digest = a.string_at("digest");
        const json::Value* size = a.find("size");
        if (name.rfind("sim-qvfr-", 0) == 0 && name.size() >= 4 && name.compare(name.size() - 4, 4, ".zip") == 0 &&
            digest.rfind("sha256:", 0) == 0 && size && size->type == json::Value::Type::Number)
            return &a;
    }
    return nullptr;
}

}  // namespace

bool newest_release(const std::string& text, Release& release) {
    json::Value root;
    if (!json::parse(text, root) || root.type != json::Value::Type::Array)
        return false;
    // GitHub lists them newest first; the highest version wins all the same (a release edited later moves up).
    // Only releases with a Sim-QVFR package count: the repository's older ace-quadview alphas (2026.11.x tags,
    // ace-quadview-*.zip) would otherwise outrank 1.0.0 and offer a downgrade.
    const json::Value *best = nullptr, *bestZip = nullptr;
    for (const json::Value& r : root.items) {
        const json::Value* draft = r.find("draft");
        if (r.type != json::Value::Type::Object || (draft && draft->boolean) || r.string_at("tag_name").empty())
            continue;
        const json::Value* zip = package_zip(r);
        if (zip && (!best || compare_versions(widen(r.string_at("tag_name")), widen(best->string_at("tag_name"))) > 0)) {
            best = &r;
            bestZip = zip;
        }
    }
    if (!best)
        return false;
    release = {widen(best->string_at("tag_name")), widen(best->string_at("html_url"))};
    release.zipUrl = bestZip->string_at("browser_download_url");
    release.zipSha256 = bestZip->string_at("digest").substr(7);
    release.zipSize = std::strtoull(bestZip->find("size")->text.c_str(), nullptr, 10);
    return true;
}

bool fetch_releases(std::string& body, std::wstring& error) {
    body.clear();
    HINTERNET session = WinHttpOpen(L"sim-qvfr-launcher", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                                    WINHTTP_NO_PROXY_BYPASS, 0);
    HINTERNET connect = session ? WinHttpConnect(session, kReleasesApiHost, INTERNET_DEFAULT_HTTPS_PORT, 0) : nullptr;
    HINTERNET request = connect ? WinHttpOpenRequest(connect, L"GET", kReleasesApiPath, nullptr, WINHTTP_NO_REFERER,
                                                     WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE)
                                : nullptr;
    bool ok = false;
    if (request) {
        WinHttpSetTimeouts(request, 5000, 5000, 5000, 10000);
        const wchar_t* headers = L"Accept: application/vnd.github+json\r\n";
        DWORD status = 0, size = sizeof(status);
        if (WinHttpSendRequest(request, headers, static_cast<DWORD>(-1), WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
            WinHttpReceiveResponse(request, nullptr) &&
            WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                                &status, &size, WINHTTP_NO_HEADER_INDEX)) {
            for (DWORD available = 0; WinHttpQueryDataAvailable(request, &available) && available > 0;) {
                std::string chunk(available, '\0');
                DWORD read = 0;
                if (!WinHttpReadData(request, chunk.data(), available, &read))
                    break;
                body.append(chunk.data(), read);
                if (body.size() > 4 * 1024 * 1024)  // not a releases list
                    break;
            }
            ok = status == 200;
            if (!ok)
                error = L"GitHub answered " + std::to_wstring(status);
        } else {
            error = L"GitHub could not be reached (error " + std::to_wstring(GetLastError()) + L")";
        }
    } else {
        error = L"GitHub could not be reached";
    }
    if (request)
        WinHttpCloseHandle(request);
    if (connect)
        WinHttpCloseHandle(connect);
    if (session)
        WinHttpCloseHandle(session);
    return ok;
}

}  // namespace simqvfr::launcher
