// The signature on Sim-QVFR's layer DLL, checked before a launch through the game's anti-cheat (Run with anti-cheat).
//
// Easy Anti-Cheat refuses a DLL whose signature Windows can't trust ("Untrusted system file"), and the game then starts
// without Sim-QVFR. Seen on 1.2.0-beta: PCs without the root certificate the signature ends at ("Certum Trusted
// Network CA 2"), only its older sibling. Windows adds a missing root from Microsoft's trusted-root list the first time
// a chain that needs it is built online (as the file's Properties > Digital Signatures does); EAC doesn't trigger that.
// So the launcher builds the chain itself (WinVerifyTrust, then the plain chain online; nothing changed by Sim-QVFR), which
// fixes those PCs, and refuses the launch with the reason when Windows still doesn't trust the file.
#pragma once

#include <string>

namespace simqvfr::launcher {

enum class SignatureState { Trusted, Unsigned, Untrusted, Missing, TimedOut };

struct SignatureCheck {
    SignatureState state = SignatureState::Missing;
    long status = 0;          // WinVerifyTrust's result (HRESULT)
    std::wstring signer;      // the signing certificate's name (the publisher); empty: unknown
    std::wstring root;        // the chain's last certificate ("Certum Trusted Network CA 2"); empty: unknown
};

// The longest the check waits: a missing root is fetched online, and an offline PC's fetches time out.
inline constexpr unsigned kSignatureCheckMs = 20000;

// Checks `file`'s embedded signature (Authenticode), online where Windows needs to fetch a certificate.
SignatureCheck check_signature(const std::wstring& file, unsigned timeoutMs = kSignatureCheckMs);

// What went wrong, in plain words ("Windows doesn't trust the root certificate"); empty when trusted.
std::wstring signature_problem(const SignatureCheck& check);
// The launch report's line: "trusted, signed by X, root Y" or the problem.
std::wstring signature_line(const SignatureCheck& check);
// Why an anti-cheat launch is refused, one line for the window's action bar and the report, with what to do; empty when
// the file is trusted.
std::wstring anticheat_signature_error(const SignatureCheck& check, const std::wstring& file);

}  // namespace simqvfr::launcher
