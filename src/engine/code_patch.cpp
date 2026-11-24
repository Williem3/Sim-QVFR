#include "code_patch.h"

#include <cstdio>
#include <cstring>
#include <vector>

#include <MinHook.h>

#include "../log.h"
#include "../xr.h"

#include <tlhelp32.h>

namespace simqvfr {
namespace {

uint8_t* stubPage = nullptr;
size_t stubUsed = 0;
constexpr size_t kStubPageSize = 0x1000;

// Executable memory below the module, so `jmp rel32` reaches it from the module's code and back.
uint8_t* stub_alloc(size_t size) {
    if (!stubPage) {
        SYSTEM_INFO si;
        GetSystemInfo(&si);
        const uintptr_t granularity = si.dwAllocationGranularity;
        const uintptr_t base = module_base();
        for (uintptr_t offset = granularity; offset < 0x70000000 && !stubPage; offset += granularity) {
            const uintptr_t at = (base - offset) & ~(granularity - 1);
            stubPage = static_cast<uint8_t*>(VirtualAlloc(reinterpret_cast<void*>(at), kStubPageSize,
                                                          MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE));
        }
        if (!stubPage)
            return nullptr;
    }
    if (stubUsed + size > kStubPageSize)
        return nullptr;
    uint8_t* p = stubPage + stubUsed;
    stubUsed += (size + 15) & ~size_t{15};
    return p;
}

// The process's other threads suspended for a live code write (as MinHook does): ACC and X-Plane patch at session
// start with the game's threads running, and a multi-byte write could be half-seen. Nothing here may allocate or log
// while threads are held (one may hold the heap or log lock): the ids and handles are gathered first.
class Freeze {
public:
    Freeze() {
        const DWORD self = GetCurrentThreadId(), pid = GetCurrentProcessId();
        const HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        if (snap == INVALID_HANDLE_VALUE)
            return;
        THREADENTRY32 te{};
        te.dwSize = sizeof(te);
        for (BOOL more = Thread32First(snap, &te); more; more = Thread32Next(snap, &te))
            if (te.dwSize >= FIELD_OFFSET(THREADENTRY32, th32OwnerProcessID) + sizeof(te.th32OwnerProcessID) &&
                te.th32OwnerProcessID == pid && te.th32ThreadID != self)
                ids_.push_back(te.th32ThreadID);
        CloseHandle(snap);
        held_.reserve(ids_.size());  // no allocation once the first thread is held
        for (const DWORD id : ids_) {
            const HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, id);
            if (!h)
                continue;
            if (SuspendThread(h) == static_cast<DWORD>(-1)) {
                CloseHandle(h);
                continue;
            }
            held_.push_back(h);
        }
    }
    ~Freeze() {
        for (const HANDLE h : held_) {
            ResumeThread(h);
            CloseHandle(h);
        }
    }
    // True when a held thread is stopped inside (begin, end): it would resume in the middle of new instructions.
    // At `begin` itself is fine, the thread runs the new bytes from their start.
    bool inside(uintptr_t begin, uintptr_t end) const {
#ifdef _WIN64
        for (const HANDLE h : held_) {
            CONTEXT c{};
            c.ContextFlags = CONTEXT_CONTROL;
            if (GetThreadContext(h, &c) && c.Rip > begin && c.Rip < end)
                return true;
        }
#endif
        return false;
    }
    Freeze(const Freeze&) = delete;
    Freeze& operator=(const Freeze&) = delete;

private:
    std::vector<DWORD> ids_;
    std::vector<HANDLE> held_;
};

bool rel32(const void* from_end, const void* to, int32_t* out) {
    const int64_t d = reinterpret_cast<intptr_t>(to) - reinterpret_cast<intptr_t>(from_end);
    if (d < INT32_MIN || d > INT32_MAX)
        return false;
    *out = static_cast<int32_t>(d);
    return true;
}

}  // namespace

uintptr_t module_base() { return reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr)); }

bool readable(const void* address, size_t size) {
    MEMORY_BASIC_INFORMATION mbi;
    return VirtualQuery(address, &mbi, sizeof(mbi)) && mbi.State == MEM_COMMIT &&
           !(mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) &&
           reinterpret_cast<uintptr_t>(address) + size <= reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
}

std::string module_offset(const void* address) {
    HMODULE module = nullptr;
    char name[MAX_PATH] = "?";
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           static_cast<LPCSTR>(address), &module) &&
        module) {
        char path[MAX_PATH];
        if (GetModuleFileNameA(module, path, MAX_PATH)) {
            const char* slash = std::strrchr(path, '\\');
            std::snprintf(name, sizeof(name), "%s", slash ? slash + 1 : path);
        }
    }
    char out[MAX_PATH + 32];
    std::snprintf(out, sizeof(out), "%s+0x%llx", name,
                  static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(address) - reinterpret_cast<uintptr_t>(module)));
    return out;
}

bool minhook_ready(const char* tag, const char* outcome) {
    const MH_STATUS init = MH_Initialize();
    if (init == MH_OK || init == MH_ERROR_ALREADY_INITIALIZED)
        return true;
    if (tag)
        logf("%s: MH_Initialize failed%s", tag, outcome);
    return false;
}

bool hook_function(void* target, void* detour, void** original) {
    if (!target || !detour || !original || !minhook_ready("hook"))
        return false;
    const MH_STATUS created = MH_CreateHook(target, detour, original);
    if (created != MH_OK && created != MH_ERROR_ALREADY_CREATED)
        return false;
    const MH_STATUS enabled = MH_EnableHook(target);
    return enabled == MH_OK || enabled == MH_ERROR_ENABLED;
}

bool unhook_function(void* target) {
    if (!target)
        return false;
    const MH_STATUS s = MH_DisableHook(target);
    return s == MH_OK || s == MH_ERROR_DISABLED;
}

bool code_matches(uintptr_t rva, const uint8_t* anchor, size_t size) {
    const void* p = reinterpret_cast<const void*>(module_base() + rva);
    return readable(p, size) && std::memcmp(p, anchor, size) == 0;
}

bool code_write(uintptr_t rva, const void* bytes, size_t size) {
    void* p = reinterpret_cast<void*>(module_base() + rva);
    const uintptr_t begin = reinterpret_cast<uintptr_t>(p);
    DWORD protect = 0;
    if (!VirtualProtect(p, size, PAGE_EXECUTE_READWRITE, &protect))
        return false;
    // A thread stopped mid-range is let run on a little, then held again (it's in a few instructions at most).
    bool written = false;
    for (int attempt = 0; attempt < 50 && !written; ++attempt) {
        {
            const Freeze freeze;
            if (!freeze.inside(begin, begin + size)) {
                std::memcpy(p, bytes, size);
                FlushInstructionCache(GetCurrentProcess(), p, size);
                written = true;
            }
        }  // the threads run again here
        if (!written)
            Sleep(1);
    }
    VirtualProtect(p, size, protect, &protect);
    if (!written)
        logf("code_patch: a thread stayed inside 0x%llx (+%zu): not written", static_cast<unsigned long long>(rva), size);
    return written;
}

bool code_redirect(uintptr_t rva, const uint8_t* anchor, size_t size, const uint8_t* stubCode, size_t stubSize) {
    if (size < 5 || !code_matches(rva, anchor, size)) {
        logf("code_patch: anchor mismatch at 0x%llx", static_cast<unsigned long long>(rva));
        return false;
    }
    uint8_t* stub = stub_alloc(stubSize + 5);
    if (!stub) {
        log_line("code_patch: no stub memory near the module");
        return false;
    }
    uint8_t* site = reinterpret_cast<uint8_t*>(module_base() + rva);
    int32_t back = 0, there = 0;
    if (!rel32(stub + stubSize + 5, site + size, &back) || !rel32(site + 5, stub, &there)) {
        log_line("code_patch: stub out of rel32 reach");
        return false;
    }
    std::memcpy(stub, stubCode, stubSize);
    stub[stubSize] = 0xE9;  // jmp back
    std::memcpy(stub + stubSize + 1, &back, 4);
    FlushInstructionCache(GetCurrentProcess(), stub, stubSize + 5);

    uint8_t jump[32];
    if (size > sizeof(jump))
        return false;
    std::memset(jump, 0x90, size);
    jump[0] = 0xE9;
    std::memcpy(jump + 1, &there, 4);
    return code_write(rva, jump, size);
}

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

uintptr_t vtable_slot(uintptr_t slotRva) {
    const void* p = reinterpret_cast<const void*>(module_base() + slotRva);
    if (!readable(p, sizeof(uintptr_t)))
        return 0;
    uintptr_t fn = 0;
    std::memcpy(&fn, p, sizeof(fn));
    return fn - module_base();
}

bool vtable_slot_replace(uintptr_t slotRva, uintptr_t expectedRva, const void* replacement) {
    const uintptr_t now = vtable_slot(slotRva), wanted = reinterpret_cast<uintptr_t>(replacement);
    if (now + module_base() == wanted)
        return true;
    if (now != expectedRva) {
        logf("code_patch: vtable slot 0x%llx holds 0x%llx, expected 0x%llx", static_cast<unsigned long long>(slotRva),
             static_cast<unsigned long long>(now), static_cast<unsigned long long>(expectedRva));
        return false;
    }
    return code_write(slotRva, &wanted, sizeof(wanted));
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
