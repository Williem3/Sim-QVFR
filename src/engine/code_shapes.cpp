// The patch shapes of code_patch.h built on code_redirect (masked indexes, a guarded call) and vtable_patch: no state
// of their own, so a game mod's DLL links them too (its code_redirect goes to the core's, mod_api/core_shim.cpp).
#include <windows.h>

#include <cstring>

#include "../log.h"
#include "code_patch.h"

namespace simqvfr {

bool code_mask_movsxd_shl_index(uintptr_t rva, const uint8_t (&anchor)[7], uint8_t mask) {
    // REX.W 63 /r (mod = 11), then REX.W C1 /4 ib on the same register.
    const uint8_t rex = anchor[0], modrm = anchor[2];
    const bool shape = (rex & 0xFA) == 0x48 && anchor[1] == 0x63 && (modrm >> 6) == 3 && (anchor[3] & 0xFE) == 0x48 &&
                       anchor[4] == 0xC1 && (anchor[5] >> 3) == ((0xC0 | (4 << 3)) >> 3);
    const int dst = ((modrm >> 3) & 7) | ((rex & 4) ? 8 : 0);
    const int src = (modrm & 7) | ((rex & 1) ? 8 : 0);
    if (!shape || ((anchor[5] & 7) | ((anchor[3] & 1) ? 8 : 0)) != dst) {
        logf("code_patch: 0x%llx is not movsxd r64, r32; shl r64, imm8", static_cast<unsigned long long>(rva));
        return false;
    }
    uint8_t code[16];
    size_t n = 0;
    if (dst >= 8 || src >= 8)  // mov dst32, src32 (zero-extends)
        code[n++] = static_cast<uint8_t>(0x40 | (dst >= 8 ? 4 : 0) | (src >= 8 ? 1 : 0));
    code[n++] = 0x8B;
    code[n++] = static_cast<uint8_t>(0xC0 | ((dst & 7) << 3) | (src & 7));
    if (dst >= 8)  // and dst32, mask
        code[n++] = 0x41;
    code[n++] = 0x83;
    code[n++] = static_cast<uint8_t>(0xC0 | (4 << 3) | (dst & 7));
    code[n++] = mask;
    std::memcpy(code + n, anchor + 3, 4);  // the original shl
    n += 4;
    return code_redirect(rva, anchor, sizeof(anchor), code, n);
}

bool code_call_if_below(uintptr_t rva, const uint8_t (&anchor)[5], uintptr_t target, uint8_t reg, uint8_t limit,
                        int elseClearByte) {
    if (anchor[0] != 0xE8 || reg > 7 || elseClearByte > 127) {
        logf("code_patch: 0x%llx is not call rel32 (or bad register)", static_cast<unsigned long long>(rva));
        return false;
    }
    const bool clear = elseClearByte >= 0;
    uint8_t code[24];
    size_t n = 0;
    code[n++] = 0x48;  // mov rax, target
    code[n++] = 0xB8;
    std::memcpy(code + n, &target, 8);
    n += 8;
    code[n++] = 0x83;  // cmp reg32, limit
    code[n++] = static_cast<uint8_t>(0xC0 | (7 << 3) | reg);
    code[n++] = limit;
    code[n++] = 0x73;  // jae: skip the call (and the jmp over the clear)
    code[n++] = clear ? 0x04 : 0x02;
    code[n++] = 0xFF;  // call rax
    code[n++] = 0xD0;
    if (clear) {
        code[n++] = 0xEB;  // jmp over the clear
        code[n++] = 0x04;
        code[n++] = 0xC6;  // mov byte [rcx + disp8], 0
        code[n++] = 0x41;
        code[n++] = static_cast<uint8_t>(elseClearByte);
        code[n++] = 0x00;
    }
    return code_redirect(rva, anchor, sizeof(anchor), code, n);
}

bool code_mask_load32(uintptr_t rva, const uint8_t (&anchor)[6], uint8_t mask) {
    // 8B /r with mod = 10 (base + disp32), no REX, base not rsp (no SIB).
    const uint8_t modrm = anchor[1];
    if (anchor[0] != 0x8B || (modrm >> 6) != 2 || (modrm & 7) == 4) {
        logf("code_patch: 0x%llx is not mov r32, [base + disp32]", static_cast<unsigned long long>(rva));
        return false;
    }
    uint8_t code[9];
    std::memcpy(code, anchor, 6);
    code[6] = 0x83;  // and reg, mask
    code[7] = static_cast<uint8_t>(0xC0 | (4 << 3) | ((modrm >> 3) & 7));
    code[8] = mask;
    return code_redirect(rva, anchor, sizeof(anchor), code, sizeof(code));
}

bool code_mask_imul_index(uintptr_t rva, const uint8_t (&anchor)[7], uint8_t mask) {
    // REX.W + 69 /r imm32 with a register operand (mod = 11) only.
    const uint8_t rex = anchor[0], modrm = anchor[2];
    if ((rex & 0xF8) != 0x48 || anchor[1] != 0x69 || (modrm >> 6) != 3) {
        logf("code_patch: 0x%llx is not imul r64, r64, imm32", static_cast<unsigned long long>(rva));
        return false;
    }
    const int dst = ((modrm >> 3) & 7) | ((rex & 4) ? 8 : 0);
    const int src = (modrm & 7) | ((rex & 1) ? 8 : 0);
    const auto modrm_of = [](int reg, int rm) { return static_cast<uint8_t>(0xC0 | ((reg & 7) << 3) | (rm & 7)); };
    const uint8_t hiDst = dst >= 8, hiSrc = src >= 8;

    uint8_t code[32];
    size_t n = 0;
    code[n++] = static_cast<uint8_t>(0x48 | (hiDst << 2) | hiSrc);  // mov dst, src
    code[n++] = 0x8B;
    code[n++] = modrm_of(dst, src);
    code[n++] = static_cast<uint8_t>(0x48 | hiDst);  // and dst, mask
    code[n++] = 0x83;
    code[n++] = modrm_of(4, dst);
    code[n++] = mask;
    code[n++] = static_cast<uint8_t>(0x48 | (hiDst << 2) | hiDst);  // imul dst, dst, imm32
    code[n++] = 0x69;
    code[n++] = modrm_of(dst, dst);
    std::memcpy(code + n, anchor + 3, 4);
    n += 4;
    return code_redirect(rva, anchor, sizeof(anchor), code, n);
}

bool vtable_patch(void** vtable, size_t slot, void* hook, void** original) {
    // Already ours (a retry after a partial failure): saving it as the original would recurse.
    if (vtable[slot] == hook)
        return true;
    // Keep execute when the page had it; only add write.
    MEMORY_BASIC_INFORMATION info{};
    if (!VirtualQuery(&vtable[slot], &info, sizeof(info)))
        return false;
    constexpr DWORD kExecute = PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    const DWORD writable = (info.Protect & kExecute) ? PAGE_EXECUTE_READWRITE : PAGE_READWRITE;
    DWORD old = 0;
    if (!VirtualProtect(&vtable[slot], sizeof(void*), writable, &old))
        return false;
    *original = vtable[slot];
    vtable[slot] = hook;
    VirtualProtect(&vtable[slot], sizeof(void*), old, &old);
    return true;
}

}  // namespace simqvfr
