// The flag search follows the approach of ACEvoPerf's engine-flag scan (MIT licence, copyright 2026 ecstra), narrowed
// to one flag and written anew here.
#include "stereo_flag.h"

#include <cstdint>
#include <cstring>
#include <string_view>

#include <windows.h>

#include "../../log.h"

namespace simqvfr {
namespace {

struct Range {
    const uint8_t* lo = nullptr;
    const uint8_t* hi = nullptr;
    bool has(const void* p, size_t n = 1) const {
        const auto* b = static_cast<const uint8_t*>(p);
        return b >= lo && b + n <= hi;
    }
};

// A flag's registration: `lea rdx, [name]` ... `lea rax, [storage]; mov [rsp+20h], rax` (the current value; [rsp+28h]
// the default) ... `call` the registration constructor (one per flag type).
struct Registration {
    const uint8_t* constructor = nullptr;
    uint8_t* storage[2] = {};
};

const uint8_t* rip_target(const uint8_t* insn, size_t length) {
    int32_t disp;
    std::memcpy(&disp, insn + length - 4, 4);
    return insn + length + disp;
}

// The registration whose name load points at `name`.
bool find_registration(const Range& text, const Range& image, const uint8_t* name, Registration& out) {
    for (const uint8_t* p = text.lo; p + 7 <= text.hi; ++p) {
        if (p[0] != 0x48 || p[1] != 0x8D || p[2] != 0x15 || rip_target(p, 7) != name)  // lea rdx, [rip+x]
            continue;
        // The registration call after the name load; the values' addresses are set up in the bytes before it.
        const uint8_t* call = nullptr;
        for (const uint8_t* q = p + 7; q + 5 <= text.hi && q < p + 240; ++q)
            if (q[0] == 0xE8) {
                call = q;
                break;
            }
        if (!call || !text.has(rip_target(call, 5)))
            continue;
        Registration r;
        r.constructor = rip_target(call, 5);
        const uint8_t* from = call - text.lo > 220 ? call - 220 : text.lo;
        for (const uint8_t* q = from; q + 7 <= call; ++q) {
            if (q[0] != 0x48 || q[1] != 0x8D || q[2] != 0x05)  // lea rax, [rip+x]
                continue;
            const uint8_t* target = rip_target(q, 7);
            for (const uint8_t* m = q + 7; m < q + 15 && m + 5 <= call; ++m)
                if (m[0] == 0x48 && m[1] == 0x89 && m[2] == 0x44 && m[3] == 0x24 && (m[4] == 0x20 || m[4] == 0x28)) {
                    if (image.has(target))  // the last before the call wins
                        r.storage[m[4] == 0x20 ? 0 : 1] = const_cast<uint8_t*>(target);
                    break;
                }
        }
        if (r.storage[0]) {
            out = r;
            return true;
        }
    }
    return false;
}

const uint8_t* find_string(const Range& rdata, std::string_view s) {
    for (const uint8_t* p = rdata.lo; p + s.size() + 1 <= rdata.hi; ++p)
        if (*p == static_cast<uint8_t>(s[0]) && std::memcmp(p, s.data(), s.size()) == 0 && p[s.size()] == 0 &&
            (p == rdata.lo || p[-1] == 0))
            return p;
    return nullptr;
}

bool writable(const void* p) {
    MEMORY_BASIC_INFORMATION m{};
    if (!VirtualQuery(p, &m, sizeof(m)) || m.State != MEM_COMMIT)
        return false;
    const DWORD protect = m.Protect & 0xFF;
    return protect == PAGE_READWRITE || protect == PAGE_WRITECOPY || protect == PAGE_EXECUTE_READWRITE ||
           protect == PAGE_EXECUTE_WRITECOPY;
}

}  // namespace

bool ace_single_pass_stereo_on() {
    const ULONGLONG t0 = GetTickCount64();
    auto* base = reinterpret_cast<const uint8_t*>(GetModuleHandleW(nullptr));
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    Range image{base, base + nt->OptionalHeader.SizeOfImage}, text, rdata;
    const IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
        const Range r{base + sec[i].VirtualAddress, base + sec[i].VirtualAddress + sec[i].Misc.VirtualSize};
        if (std::memcmp(sec[i].Name, ".text", 6) == 0)
            text = r;
        else if (std::memcmp(sec[i].Name, ".rdata", 7) == 0)
            rdata = r;
    }
    const auto refuse = [&](const char* why) {
        logf("ace stereo: ACE's single-pass stereo NOT turned on (%s): no focus views on the API path", why);
        return false;
    };
    if (!text.lo || !rdata.lo)
        return refuse("the exe's sections were not found");
    const uint8_t* name = find_string(rdata, "single_pass_stereo");
    const uint8_t* known = find_string(rdata, "no_intro");  // a bool flag: its registration names the bool type
    if (!name || !known)
        return refuse("its flag name is not in this build");
    Registration flag, boolean;
    if (!find_registration(text, image, name, flag) || !find_registration(text, image, known, boolean))
        return refuse("its registration was not found");
    if (flag.constructor != boolean.constructor)
        return refuse("it is not registered as a bool in this build");
    int written = 0;
    bool was = false;
    for (uint8_t* storage : flag.storage) {
        if (!storage || !writable(storage) || *storage > 1)
            continue;
        if (storage == flag.storage[0])
            was = *storage != 0;
        *storage = 1;
        ++written;
    }
    if (!written)
        return refuse("its value is not writable");
    logf("ace stereo: ACE's single-pass stereo turned on (was %s; %d value(s) written, found in %llu ms)",
         was ? "already on" : "off", written, GetTickCount64() - t0);
    return true;
}

}  // namespace simqvfr
