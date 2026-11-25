#include "mod_trust.h"

#include <windows.h>
#include <wincrypt.h>
#include <softpub.h>
#include <wintrust.h>

#include <cstdio>
#include <cstring>
#include <cwchar>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <iterator>

#include "../sha256_file.h"

namespace simqvfr::mod_api {

namespace {

// Certum code signing, until 2027-11-04 (package.ps1 -CertificateThumbprint).
constexpr const wchar_t* kOfficialThumbprints[] = {L"4683DA63C881A76D5A1E803751B481F621ADF9C1"};

std::string narrow(const std::wstring& w) {
    std::string out;
    for (wchar_t c : w)
        out += c < 0x80 ? static_cast<char>(c) : '?';
    return out;
}

std::wstring upper_hex(const BYTE* bytes, DWORD n) {
    std::wstring hex;
    wchar_t h[3];
    for (DWORD i = 0; i < n; ++i) {
        swprintf_s(h, L"%02X", bytes[i]);
        hex += h;
    }
    return hex;
}

std::wstring cert_name(PCCERT_CONTEXT cert) {
    wchar_t name[256] = {};
    CertGetNameStringW(cert, CERT_NAME_SIMPLE_DISPLAY_TYPE, 0, nullptr, name, static_cast<DWORD>(std::size(name)));
    return name;
}

std::wstring module_path(const void* addressInModule) {
    HMODULE module = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       static_cast<LPCWSTR>(addressInModule), &module);
    wchar_t path[MAX_PATH] = {};
    const DWORD n = GetModuleFileNameW(module, path, MAX_PATH);
    return n > 0 && n < MAX_PATH ? std::wstring(path) : std::wstring();
}

}  // namespace

bool official_thumbprint(const std::wstring& thumbprint) {
    if (thumbprint.empty())
        return false;
    for (const wchar_t* pinned : kOfficialThumbprints)
        if (_wcsicmp(pinned, thumbprint.c_str()) == 0)
            return true;
    return false;
}

bool first_party_mod(const std::wstring& id) {
    return _wcsicmp(id.c_str(), L"scs") == 0 || _wcsicmp(id.c_str(), L"xplane") == 0 || _wcsicmp(id.c_str(), L"ace") == 0 ||
           _wcsicmp(id.c_str(), L"lmu") == 0;
}

bool shipped_by_package(const std::wstring& dllPath, const std::string& sha256) {
    namespace fs = std::filesystem;
    if (sha256.size() != 64)
        return false;
    const fs::path dll = dllPath, folder = dll.parent_path(), mods = folder.parent_path(), package = mods.parent_path();
    if (_wcsicmp(mods.filename().c_str(), L"mods") != 0)
        return false;
    std::error_code ec;
    const fs::path list = package / L"SHA256.txt";
    if (!fs::exists(list, ec))
        return !ec;  // a build tree (no package manifest): the path decides
    std::ifstream in(list, std::ios::binary);
    if (!in)
        return false;
    // Each line: "<64 hex>  <relative/path>", as package.ps1 writes it.
    const std::wstring wanted = L"mods/" + folder.filename().wstring() + L"/" + dll.filename().wstring();
    std::string line;
    int lines = 0;
    while (std::getline(in, line) && ++lines < 4096) {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (line.size() < 67 || line.compare(64, 2, "  ") != 0)
            continue;
        std::wstring rel;
        for (size_t i = 66; i < line.size(); ++i)
            rel += static_cast<unsigned char>(line[i]) < 0x80 ? static_cast<wchar_t>(line[i]) : L'?';
        if (_wcsicmp(rel.c_str(), wanted.c_str()) == 0)
            return _strnicmp(line.c_str(), sha256.c_str(), 64) == 0;
    }
    return false;  // the package lists no such file: it isn't ours
}

TrustVerdict decide_mod_trust(const TrustInput& in) {
    TrustVerdict v;
    if (in.mod.sha256.empty()) {
        v.why = "the file can't be read";
        return v;
    }
    if (in.mod.signatureTrusted && official_thumbprint(in.mod.thumbprint)) {
        v.trust = ModTrust::Official;
        v.why = "signed by Sim-QVFR's maintainer";
        return v;
    }
    if (!in.coreOfficial && in.firstPartyPath && !in.onlineLaunch) {
        v.trust = ModTrust::FirstPartyUnsignedBuild;
        v.why = "shipped with this unsigned (development or nightly) build; nothing in it is signed";
        return v;
    }
    const std::string signer = in.mod.signatureTrusted && !in.mod.signer.empty() ? narrow(in.mod.signer) : "";
    if (in.approval.sha256.empty()) {
        v.why = "third-party and not approved: approve it in the launcher ([mods] <id>_sha256)";
        return v;
    }
    if (_stricmp(in.approval.sha256.c_str(), in.mod.sha256.c_str()) != 0) {
        v.why = "third-party and changed since it was approved: approve it again";
        return v;
    }
    if (in.onlineLaunch) {
        if (!in.approval.online) {
            v.why = "third-party, and Allow online is off for it while the anti-cheat runs";
            return v;
        }
        if (!in.mod.signatureTrusted) {
            v.why = "third-party and not signed by its author: it can't run while the anti-cheat runs";
            return v;
        }
    }
    v.trust = ModTrust::Community;
    v.why = "third-party, not made by Sim-QVFR; approved by the user" +
            std::string(in.onlineLaunch ? ", allowed online by the user" : "") +
            (signer.empty() ? std::string(", unsigned") : ", signed by its author " + signer);
    return v;
}

const char* trust_label(ModTrust trust) {
    switch (trust) {
        case ModTrust::Official: return "official";
        case ModTrust::FirstPartyUnsignedBuild: return "first-party, unsigned build";
        case ModTrust::Community: return "third-party";
        case ModTrust::Refused: break;
    }
    return "refused";
}

ModApproval read_mod_approval(const std::wstring& ini, const std::wstring& id) {
    ModApproval a;
    if (ini.empty() || id.empty())
        return a;
    wchar_t hash[128] = {};
    GetPrivateProfileStringW(L"mods", (id + L"_sha256").c_str(), L"", hash, static_cast<DWORD>(std::size(hash)),
                             ini.c_str());
    const std::wstring h = hash;
    bool hex = h.size() == 64;
    for (wchar_t c : h)
        hex &= iswxdigit(c) != 0;
    if (hex)
        a.sha256 = narrow(h);
    a.online = GetPrivateProfileIntW(L"mods", (id + L"_online").c_str(), 0, ini.c_str()) == 1;
    return a;
}

bool write_mod_approval(const std::wstring& ini, const std::wstring& id, const ModApproval& approval) {
    if (ini.empty() || id.empty())
        return false;
    const std::wstring hash(approval.sha256.begin(), approval.sha256.end());
    bool ok = WritePrivateProfileStringW(L"mods", (id + L"_sha256").c_str(), hash.empty() ? nullptr : hash.c_str(),
                                         ini.c_str()) != 0;
    ok &= WritePrivateProfileStringW(L"mods", (id + L"_online").c_str(),
                                     !hash.empty() && approval.online ? L"1" : nullptr, ini.c_str()) != 0;
    return ok;
}

ModFacts read_mod_facts(const std::wstring& path) {
    ModFacts f;
    f.sha256 = sha256_file(path.c_str());
    if (f.sha256.empty())
        return f;

    WINTRUST_FILE_INFO fileInfo{sizeof(fileInfo)};
    fileInfo.pcwszFilePath = path.c_str();
    WINTRUST_DATA data{sizeof(data)};
    data.dwUIChoice = WTD_UI_NONE;
    data.fdwRevocationChecks = WTD_REVOKE_NONE;
    data.dwUnionChoice = WTD_CHOICE_FILE;
    data.pFile = &fileInfo;
    data.dwStateAction = WTD_STATEACTION_VERIFY;
    data.dwProvFlags = WTD_CACHE_ONLY_URL_RETRIEVAL;  // nothing fetched from the game's process
    GUID action = WINTRUST_ACTION_GENERIC_VERIFY_V2;
    const LONG status = WinVerifyTrust(static_cast<HWND>(INVALID_HANDLE_VALUE), &action, &data);
    f.signatureTrusted = status == ERROR_SUCCESS;
    if (data.hWVTStateData) {
        if (CRYPT_PROVIDER_DATA* provider = WTHelperProvDataFromStateData(data.hWVTStateData))
            if (CRYPT_PROVIDER_SGNR* signer = WTHelperGetProvSignerFromChain(provider, 0, FALSE, 0))
                if (signer->csCertChain > 0 && signer->pasCertChain && signer->pasCertChain[0].pCert) {
                    PCCERT_CONTEXT cert = signer->pasCertChain[0].pCert;
                    f.signer = cert_name(cert);
                    BYTE sha1[20];
                    DWORD n = sizeof(sha1);
                    if (CertGetCertificateContextProperty(cert, CERT_SHA1_HASH_PROP_ID, sha1, &n))
                        f.thumbprint = upper_hex(sha1, n);
                }
        data.dwStateAction = WTD_STATEACTION_CLOSE;
        WinVerifyTrust(static_cast<HWND>(INVALID_HANDLE_VALUE), &action, &data);
    }
    if (!f.signatureTrusted) {  // a name or thumbprint from a signature Windows doesn't trust means nothing
        f.signer.clear();
        f.thumbprint.clear();
    }
    return f;
}

bool module_officially_signed(const void* addressInModule) {
    const std::wstring path = module_path(addressInModule);
    if (path.empty())
        return false;
    const ModFacts f = read_mod_facts(path);
    return f.signatureTrusted && official_thumbprint(f.thumbprint);
}

}  // namespace simqvfr::mod_api
