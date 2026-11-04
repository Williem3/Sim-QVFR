#include "probe.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <MinHook.h>

#include "../games/ace/abi.h"
#include "../sha256_file.h"
#include "../log.h"
#include "../xr.h"

namespace aceqv {
namespace {

constexpr size_t kProbeCount = std::size(ace::kProbeTargets);
constexpr int kStackFrames = 14;
constexpr uint64_t kFullLogCalls = 12;        // per probe, with call stack
constexpr uint64_t kSequenceEveryFrames = 1800;
// Counted in xrEndFrame calls reaching this layer, which VectorXR frame generation doubles.
constexpr int kSequenceFrames = 6;

// Registers the stub pushed at hook entry, lowest address first.
struct EntryRegisters {
    uint64_t r15, r14, r13, r12, rdi, rsi, rbp, rbx, r9, r8, rdx, rcx;
    uint64_t returnAddress;
};

struct Probe {
    void* target = nullptr;
    void* original = nullptr;
    uint8_t* stub = nullptr;
    std::atomic<uint64_t> calls{0};
    std::atomic<void (*)(void*)> observer{nullptr};
};

Probe probes[kProbeCount];
std::uintptr_t base = 0;
bool installed = false;
std::atomic<uint64_t> frame{0};
std::atomic<bool> sequenceActive{false};
std::atomic<int> sequenceFramesLeft{0};
size_t stubJumpSlot = 0;

std::string describe(uint64_t address) {
    HMODULE module = nullptr;
    char name[MAX_PATH] = "?";
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCSTR>(address), &module) &&
        module) {
        char path[MAX_PATH];
        if (GetModuleFileNameA(module, path, MAX_PATH)) {
            const char* slash = std::strrchr(path, '\\');
            std::snprintf(name, sizeof(name), "%s", slash ? slash + 1 : path);
        }
    }
    char out[MAX_PATH + 32];
    std::snprintf(out, sizeof(out), "%s+0x%llx", name,
                  static_cast<unsigned long long>(address - reinterpret_cast<uint64_t>(module)));
    return out;
}

// Unwinds from the hooked function's caller using the exe's own unwind tables.
int walk_stack(const EntryRegisters& regs, uint64_t (&frames)[kStackFrames]) {
    CONTEXT ctx = {};
    ctx.Rip = regs.returnAddress;
    ctx.Rsp = reinterpret_cast<uint64_t>(&regs.returnAddress) + 8;
    ctx.Rbx = regs.rbx;
    ctx.Rbp = regs.rbp;
    ctx.Rsi = regs.rsi;
    ctx.Rdi = regs.rdi;
    ctx.R12 = regs.r12;
    ctx.R13 = regs.r13;
    ctx.R14 = regs.r14;
    ctx.R15 = regs.r15;
    int count = 0;
    __try {
        while (count < kStackFrames && ctx.Rip) {
            frames[count++] = ctx.Rip;
            DWORD64 imageBase = 0;
            PRUNTIME_FUNCTION fn = RtlLookupFunctionEntry(ctx.Rip, &imageBase, nullptr);
            if (!fn) {
                ctx.Rip = *reinterpret_cast<uint64_t*>(ctx.Rsp);
                ctx.Rsp += 8;
                continue;
            }
            void* handlerData = nullptr;
            DWORD64 establisher = 0;
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, ctx.Rip, fn, &ctx, &handlerData, &establisher, nullptr);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return count;
}

void log_call(size_t id, const EntryRegisters& regs, uint64_t call, bool withStack) {
    char line[1400];
    int n = std::snprintf(line, sizeof(line), "probe %s call=%llu frame=%llu this=%p arg2=%lld caller=%s",
                          ace::kProbeTargets[id].name, static_cast<unsigned long long>(call),
                          static_cast<unsigned long long>(frame.load()), reinterpret_cast<void*>(regs.rcx),
                          static_cast<long long>(regs.rdx), describe(regs.returnAddress).c_str());
    if (withStack) {
        uint64_t frames[kStackFrames];
        int count = walk_stack(regs, frames);
        n += std::snprintf(line + n, sizeof(line) - n, " stack:");
        for (int i = 1; i < count && n < static_cast<int>(sizeof(line)) - 64; ++i)
            n += std::snprintf(line + n, sizeof(line) - n, " %s", describe(frames[i]).c_str());
    }
    log_line(line);
}

// Called by every stub before it jumps to the original function. Must never throw.
void __stdcall on_probe(EntryRegisters* regs, size_t id) {
    const uint64_t call = ++probes[id].calls;
    if (auto* observer = probes[id].observer.load(std::memory_order_relaxed))
        observer(reinterpret_cast<void*>(regs->rcx));
    if (call <= kFullLogCalls)
        log_call(id, *regs, call, true);
    else if (sequenceActive.load(std::memory_order_relaxed))
        log_call(id, *regs, call, false);
}

// push rcx..r15; sub rsp,0x68; save xmm0-3; on_probe(&regs, id); restore; jmp [original]
uint8_t* build_stub(size_t id) {
    static const uint8_t prologue[] = {
        0x51, 0x52, 0x41, 0x50, 0x41, 0x51, 0x53, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57,
        0x48, 0x83, 0xEC, 0x68,
        0xF3, 0x0F, 0x7F, 0x44, 0x24, 0x20, 0xF3, 0x0F, 0x7F, 0x4C, 0x24, 0x30,
        0xF3, 0x0F, 0x7F, 0x54, 0x24, 0x40, 0xF3, 0x0F, 0x7F, 0x5C, 0x24, 0x50,
        0x48, 0x8D, 0x4C, 0x24, 0x68};
    static const uint8_t epilogue[] = {
        0xF3, 0x0F, 0x6F, 0x44, 0x24, 0x20, 0xF3, 0x0F, 0x6F, 0x4C, 0x24, 0x30,
        0xF3, 0x0F, 0x6F, 0x54, 0x24, 0x40, 0xF3, 0x0F, 0x6F, 0x5C, 0x24, 0x50,
        0x48, 0x83, 0xC4, 0x68,
        0x41, 0x5F, 0x41, 0x5E, 0x41, 0x5D, 0x41, 0x5C, 0x5F, 0x5E, 0x5D, 0x5B, 0x41, 0x59, 0x41, 0x58, 0x5A, 0x59,
        0xFF, 0x25, 0x00, 0x00, 0x00, 0x00};
    auto* stub = static_cast<uint8_t*>(VirtualAlloc(nullptr, 256, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!stub)
        return nullptr;
    uint8_t* p = stub;
    std::memcpy(p, prologue, sizeof(prologue));
    p += sizeof(prologue);
    *p++ = 0xBA;  // mov edx, id
    const uint32_t id32 = static_cast<uint32_t>(id);
    std::memcpy(p, &id32, 4);
    p += 4;
    *p++ = 0x48;  // mov rax, on_probe
    *p++ = 0xB8;
    const uint64_t handler = reinterpret_cast<uint64_t>(&on_probe);
    std::memcpy(p, &handler, 8);
    p += 8;
    *p++ = 0xFF;  // call rax
    *p++ = 0xD0;
    std::memcpy(p, epilogue, sizeof(epilogue));
    p += sizeof(epilogue);
    stubJumpSlot = static_cast<size_t>(p - stub);  // `jmp [rip+0]` reads the 8 bytes that follow it
    return stub;
}

void set_stub_target(uint8_t* stub, void* original) {
    const uint64_t target = reinterpret_cast<uint64_t>(original);
    std::memcpy(stub + stubJumpSlot, &target, 8);
    FlushInstructionCache(GetCurrentProcess(), stub, 256);
}

bool readable(const void* address, size_t size) {
    MEMORY_BASIC_INFORMATION mbi;
    return VirtualQuery(address, &mbi, sizeof(mbi)) && mbi.State == MEM_COMMIT &&
           !(mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) &&
           reinterpret_cast<uintptr_t>(address) + size <= reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
}

bool verified_build() {
    wchar_t path[MAX_PATH] = {};
    const DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
    const wchar_t* name = wcsrchr(path, L'\\');
    if (n == 0 || n >= MAX_PATH || _wcsicmp(name ? name + 1 : path, ace::kExecutableName) != 0) {
        log_line("probe: not AssettoCorsaEVO.exe; probes refused");
        return false;
    }
    const std::string sha = aceqv::sha256_file(path);
    if (sha != ace::kExecutableSha256) {
        logf("probe: unsupported exe sha256=%s (expected %s); probes refused", sha.c_str(), ace::kExecutableSha256);
        return false;
    }
    for (const auto& t : ace::kProbeTargets) {
        const void* p = reinterpret_cast<const void*>(base + t.rva);
        if (!readable(p, t.anchorSize) || std::memcmp(p, t.anchor, t.anchorSize) != 0) {
            logf("probe: anchor mismatch %s rva=0x%llx; probes refused", t.name, static_cast<unsigned long long>(t.rva));
            return false;
        }
    }
    return true;
}

}  // namespace

bool probe_start() {
    if (installed)
        return true;
    base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    if (!verified_build())
        return false;
    // views4 may have initialised MinHook already, at xrCreateInstance.
    if (const MH_STATUS init = MH_Initialize(); init != MH_OK && init != MH_ERROR_ALREADY_INITIALIZED) {
        log_line("probe: MH_Initialize failed");
        return false;
    }
    // Each probe stands alone: MinHook's decoder cannot relocate some prologues (VEX-encoded AVX
    // instructions such as set_fov's `vmovss`), and one unhookable target must not cost the rest.
    size_t queued = 0;
    for (size_t i = 0; i < kProbeCount; ++i) {
        Probe& p = probes[i];
        p.target = reinterpret_cast<void*>(base + ace::kProbeTargets[i].rva);
        p.stub = build_stub(i);
        const MH_STATUS status = p.stub ? MH_CreateHook(p.target, p.stub, &p.original) : MH_ERROR_MEMORY_ALLOC;
        if (status != MH_OK) {
            logf("probe: %s skipped (MinHook %s)", ace::kProbeTargets[i].name, MH_StatusToString(status));
            p.target = nullptr;
            continue;
        }
        set_stub_target(p.stub, p.original);
        MH_QueueEnableHook(p.target);
        ++queued;
    }
    if (queued == 0 || MH_ApplyQueued() != MH_OK) {
        log_line("probe: no probe could be enabled");
        MH_Uninitialize();
        return false;
    }
    installed = true;
    logf("probe: verified AssettoCorsaEVO.exe; %zu/%zu log-only probes installed (rendering unchanged)", queued,
         kProbeCount);
    return true;
}

void probe_stop() {
    if (!installed)
        return;
    MH_DisableHook(MH_ALL_HOOKS);
    MH_Uninitialize();
    installed = false;
    log_line("probe: removed");
}

bool probe_sequence_active() { return sequenceActive.load(std::memory_order_relaxed); }

bool probe_set_observer(const char* name, void (*fn)(void* self)) {
    for (size_t i = 0; i < kProbeCount; ++i)
        if (std::strcmp(ace::kProbeTargets[i].name, name) == 0) {
            probes[i].observer = fn;
            return true;
        }
    return false;
}

void probe_frame_end() {
    const uint64_t f = ++frame;
    if (sequenceActive) {
        if (--sequenceFramesLeft <= 0) {
            sequenceActive = false;
            char line[512];
            int n = std::snprintf(line, sizeof(line), "probe totals frame=%llu:", static_cast<unsigned long long>(f));
            for (size_t i = 0; i < kProbeCount; ++i)
                n += std::snprintf(line + n, sizeof(line) - n, " %s=%llu", ace::kProbeTargets[i].name,
                                   static_cast<unsigned long long>(probes[i].calls.load()));
            log_line(line);
        }
    } else if (f % kSequenceEveryFrames == 0) {
        logf("probe sequence begin frame=%llu (next %d frames, every probed call in order)",
             static_cast<unsigned long long>(f), kSequenceFrames);
        sequenceFramesLeft = kSequenceFrames;
        sequenceActive = true;
    }
}

}  // namespace aceqv
