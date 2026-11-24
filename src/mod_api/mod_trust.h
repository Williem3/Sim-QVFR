// Which game mods may load, and as what (docs/architecture/mod-manager-plan.md "Signing and trust", option A).
//
// A signature belongs to one file: Windows and anti-cheats judge each DLL by its own. So the maintainer's signature
// never covers a mod someone else built, whether it loads through the launcher or not. The core only decides whether
// a mod may load into this launch, and labels it:
//   - Official: Windows trusts its Authenticode signature, and the signing certificate is one pinned below
//     (kOfficialThumbprints). Only the maintainer can sign that way.
//   - First-party, unsigned build: a development or nightly build whose core isn't officially signed either, loading a
//     mod the package itself ships (its path is one of the first-party ones). No signature is lent: there is none.
//     Never in a launch protected by an anti-cheat.
//   - Community (third-party, not made by Sim-QVFR): the user approved this exact file (its SHA-256 in [mods] of
//     sim-qvfr.ini). In a launch protected by an anti-cheat it also needs the author's OWN trusted signature and the
//     user's "Allow online" for that mod ([mods] <id>_online=1): the anti-cheat then sees the author's name, not the
//     maintainer's, and the user chose the risk.
//   - Refused: anything else; the game then runs with its identity alone (the layer only logs).
#pragma once

#include <string>

namespace simqvfr::mod_api {

// The maintainer's code-signing certificates (SHA-1 thumbprints, as package.ps1 -CertificateThumbprint): a renewed
// certificate is added here, an old one kept while builds signed with it are in use.
bool official_thumbprint(const std::wstring& thumbprint);

// A mod the package itself ships (its folder under mods\): "scs".
bool first_party_mod(const std::wstring& id);

// What the core found out about a mod's file.
struct ModFacts {
    bool signatureTrusted = false;  // Windows trusts its Authenticode signature
    std::wstring signer;            // the signing certificate's name; empty: unsigned or unknown
    std::wstring thumbprint;        // the signing certificate's SHA-1, uppercase hex; empty: unsigned or unknown
    std::string sha256;             // the file's SHA-256, lowercase hex; empty: it couldn't be read
};

// The user's approval of a mod, from [mods] of sim-qvfr.ini: <id>_sha256 and <id>_online.
struct ModApproval {
    std::string sha256;   // the approved file's SHA-256; empty: never approved
    bool online = false;  // "Allow online": it may load in a launch an anti-cheat protects
};

struct TrustInput {
    ModFacts mod;
    ModApproval approval;
    bool coreOfficial = false;   // the core DLL itself is officially signed (a release), not a development build
    bool firstPartyPath = false; // the mod's path is one the package ships (mods\scs\sim-qvfr-scs.dll)
    bool onlineLaunch = false;   // the game's anti-cheat protects this launch
};

enum class ModTrust { Official, FirstPartyUnsignedBuild, Community, Refused };

struct TrustVerdict {
    ModTrust trust = ModTrust::Refused;
    std::string why;  // for the log: what it is, or why it was refused
};

TrustVerdict decide_mod_trust(const TrustInput& in);

// The label the log and the report use ("official", "first-party, unsigned build", "third-party", "refused").
const char* trust_label(ModTrust trust);

// The approval of mod `id` in `ini` ([mods] <id>_sha256, <id>_online).
ModApproval read_mod_approval(const std::wstring& ini, const std::wstring& id);
// Writes it (the launcher, when the user approves a mod). An empty sha256 takes the approval away.
bool write_mod_approval(const std::wstring& ini, const std::wstring& id, const ModApproval& approval);

// The file's signature and SHA-256. Offline: only certificates Windows already has are used (no fetch from the game's
// process), and revocation isn't checked.
ModFacts read_mod_facts(const std::wstring& path);
// Whether the module holding `addressInModule` (the core) is officially signed.
bool module_officially_signed(const void* addressInModule);

}  // namespace simqvfr::mod_api
