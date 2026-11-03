// A file's SHA-256 as lowercase hex (empty if it cannot be read): the game build check, in the layer
// (probe.cpp) and in the launcher's launch checks (launcher/preflight.h). Link bcrypt.
#pragma once

#include <cstdio>
#include <string>
#include <vector>

#include <windows.h>
#include <bcrypt.h>

namespace aceqv {

inline std::string sha256_file(const wchar_t* path) {
    HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                              FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return {};
    std::string hex;
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    if (BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0)) &&
        BCRYPT_SUCCESS(BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0))) {
        std::vector<UCHAR> buffer(1 << 20);
        DWORD read = 0;
        bool ok = true;
        while (ReadFile(file, buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr) && read > 0)
            ok &= BCRYPT_SUCCESS(BCryptHashData(hash, buffer.data(), read, 0));
        UCHAR digest[32];
        if (ok && BCRYPT_SUCCESS(BCryptFinishHash(hash, digest, sizeof(digest), 0))) {
            char h[65];
            for (int i = 0; i < 32; ++i)
                std::snprintf(h + i * 2, 3, "%02x", digest[i]);
            hex.assign(h, 64);
        }
    }
    if (hash)
        BCryptDestroyHash(hash);
    if (alg)
        BCryptCloseAlgorithmProvider(alg, 0);
    CloseHandle(file);
    return hex;
}

}  // namespace aceqv
