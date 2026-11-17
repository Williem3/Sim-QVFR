#include "signature_check.h"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <future>
#include <iterator>
#include <memory>
#include <thread>

#include <windows.h>
#include <wincrypt.h>
#include <softpub.h>
#include <wintrust.h>

namespace simqvfr::launcher {

namespace {

std::wstring cert_name(PCCERT_CONTEXT cert) {
    if (!cert)
        return {};
    wchar_t name[256] = {};
    CertGetNameStringW(cert, CERT_NAME_SIMPLE_DISPLAY_TYPE, 0, nullptr, name, static_cast<DWORD>(std::size(name)));
    return name;
}

// The check itself, on its own thread (check_signature waits for it).
SignatureCheck verify(const std::wstring& file) {
    SignatureCheck out;
    std::error_code ec;
    if (!std::filesystem::is_regular_file(file, ec))
        return out;  // Missing

    WINTRUST_FILE_INFO fileInfo{sizeof(fileInfo)};
    fileInfo.pcwszFilePath = file.c_str();
    WINTRUST_DATA data{sizeof(data)};
    data.dwUIChoice = WTD_UI_NONE;
    data.fdwRevocationChecks = WTD_REVOKE_NONE;  // trust only: an offline PC with the root installed still passes
    data.dwUnionChoice = WTD_CHOICE_FILE;
    data.pFile = &fileInfo;
    data.dwStateAction = WTD_STATEACTION_VERIFY;
    GUID action = WINTRUST_ACTION_GENERIC_VERIFY_V2;
    out.status = WinVerifyTrust(static_cast<HWND>(INVALID_HANDLE_VALUE), &action, &data);

    // The signer, from the state WinVerifyTrust kept (also when it didn't trust the file).
    PCCERT_CONTEXT signerCert = nullptr;
    if (data.hWVTStateData) {
        if (CRYPT_PROVIDER_DATA* provider = WTHelperProvDataFromStateData(data.hWVTStateData))
            if (CRYPT_PROVIDER_SGNR* signer = WTHelperGetProvSignerFromChain(provider, 0, FALSE, 0))
                if (signer->csCertChain > 0 && signer->pasCertChain && signer->pasCertChain[0].pCert)
                    signerCert = CertDuplicateCertificateContext(signer->pasCertChain[0].pCert);
        data.dwStateAction = WTD_STATEACTION_CLOSE;
        WinVerifyTrust(static_cast<HWND>(INVALID_HANDLE_VALUE), &action, &data);
    }

    // WinVerifyTrust may end the chain at Certum's older root through a cross-certificate, while Easy Anti-Cheat refused
    // the DLL on PCs without "Certum Trusted Network CA 2" (2 testers, 1.2.0-beta). The plain chain, built online as
    // the file's Properties do (and certificationdiagnose.ps1's X509Chain), ends at CA 2 and gets Windows to add it
    // when missing: that's what fixed both. It must be trusted too. Time and revocation are WinVerifyTrust's (a
    // timestamped signature outlives its certificate; an offline PC can't check revocation).
    if (signerCert) {
        out.signer = cert_name(signerCert);
        CERT_CHAIN_PARA para{sizeof(para)};
        PCCERT_CHAIN_CONTEXT chain = nullptr;
        if (CertGetCertificateChain(nullptr, signerCert, nullptr, signerCert->hCertStore, &para,
                                    CERT_CHAIN_REVOCATION_CHECK_CHAIN_EXCLUDE_ROOT, nullptr, &chain) &&
            chain) {
            if (chain->cChain > 0 && chain->rgpChain[0]->cElement > 0) {
                const CERT_SIMPLE_CHAIN* simple = chain->rgpChain[0];
                out.root = cert_name(simple->rgpElement[simple->cElement - 1]->pCertContext);
            }
            const DWORD ignored = CERT_TRUST_IS_NOT_TIME_VALID | CERT_TRUST_IS_NOT_TIME_NESTED |
                                  CERT_TRUST_REVOCATION_STATUS_UNKNOWN | CERT_TRUST_IS_OFFLINE_REVOCATION |
                                  CERT_TRUST_CTL_IS_NOT_TIME_VALID;
            const DWORD errors = chain->TrustStatus.dwErrorStatus & ~ignored;
            if (out.status == ERROR_SUCCESS && errors != 0)
                out.status = (errors & CERT_TRUST_IS_UNTRUSTED_ROOT)   ? CERT_E_UNTRUSTEDROOT
                             : (errors & CERT_TRUST_IS_PARTIAL_CHAIN) ? CERT_E_CHAINING
                             : (errors & CERT_TRUST_IS_REVOKED)       ? CERT_E_REVOKED
                                                                      : CERT_E_CHAINING;
            CertFreeCertificateChain(chain);
        }
        CertFreeCertificateContext(signerCert);
    }

    switch (out.status) {
        case ERROR_SUCCESS: out.state = SignatureState::Trusted; break;
        case TRUST_E_NOSIGNATURE:
        case TRUST_E_SUBJECT_FORM_UNKNOWN:
        case TRUST_E_PROVIDER_UNKNOWN: out.state = SignatureState::Unsigned; break;
        default: out.state = SignatureState::Untrusted; break;
    }
    return out;
}

}  // namespace

SignatureCheck check_signature(const std::wstring& file, unsigned timeoutMs) {
    // A thread of its own, so an offline PC's certificate fetches can't hold the launch past timeoutMs; the thread
    // owns its copy of the path and its promise, and finishes on its own if it's given up on.
    auto promise = std::make_shared<std::promise<SignatureCheck>>();
    std::future<SignatureCheck> result = promise->get_future();
    std::thread([promise, file] { promise->set_value(verify(file)); }).detach();
    if (result.wait_for(std::chrono::milliseconds(timeoutMs)) != std::future_status::ready) {
        SignatureCheck out;
        out.state = SignatureState::TimedOut;
        return out;
    }
    return result.get();
}

std::wstring signature_problem(const SignatureCheck& check) {
    switch (check.state) {
        case SignatureState::Trusted: return {};
        case SignatureState::Missing: return L"the file isn't there";
        case SignatureState::Unsigned: return L"the file isn't signed (a development or test build)";
        case SignatureState::TimedOut: return L"Windows didn't finish checking the signature in time (no internet?)";
        case SignatureState::Untrusted: break;
    }
    switch (static_cast<unsigned long>(check.status)) {
        case static_cast<unsigned long>(CERT_E_UNTRUSTEDROOT):
            return L"Windows doesn't trust the root certificate" + (check.root.empty() ? L"" : L" (" + check.root + L")");
        case static_cast<unsigned long>(CERT_E_CHAINING):
            return L"Windows couldn't fetch the root certificate (no internet, or root certificate updates are off)";
        case static_cast<unsigned long>(TRUST_E_BAD_DIGEST): return L"the file was changed after it was signed";
        case static_cast<unsigned long>(CERT_E_EXPIRED): return L"the certificate has expired";
        case static_cast<unsigned long>(CERT_E_REVOKED):
        case static_cast<unsigned long>(TRUST_E_EXPLICIT_DISTRUST): return L"the certificate is blocked on this PC";
        default: break;
    }
    wchar_t code[64];
    swprintf_s(code, L"Windows doesn't trust the signature (0x%08lX)", static_cast<unsigned long>(check.status));
    return code;
}

std::wstring signature_line(const SignatureCheck& check) {
    if (check.state != SignatureState::Trusted)
        return L"NOT trusted: " + signature_problem(check);
    std::wstring line = L"trusted";
    if (!check.signer.empty())
        line += L", signed by " + check.signer;
    if (!check.root.empty())
        line += L", root " + check.root;
    return line;
}

std::wstring anticheat_signature_error(const SignatureCheck& check, const std::wstring& file) {
    if (check.state == SignatureState::Trusted)
        return {};
    std::wstring what;
    switch (check.state) {
        case SignatureState::Unsigned:
        case SignatureState::Missing: what = L"Use the signed files from the release ZIP."; break;
        default:
            what = check.status == TRUST_E_BAD_DIGEST
                       ? L"Unpack the release ZIP again."
                       : L"Connect to the internet and launch again: Windows adds the missing root certificate itself.";
            break;
    }
    return L"Run with anti-cheat is on, and " + signature_problem(check) + L" for " + file +
           L". Easy Anti-Cheat would refuse it and the game would start without Sim-QVFR. " + what +
           L" Or turn Run with anti-cheat off.";
}

}  // namespace simqvfr::launcher
