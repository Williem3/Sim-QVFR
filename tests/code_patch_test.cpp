// Offline check of code_mask_imul_index: each of views4's five camera-slot `imul dst, view, 0x184`
// encodings is placed in a small function near this module, patched, and called with views 0..3.
// Expected after the patch: view & 1 times 0x184 (views 2 and 3 fold onto 0 and 1).
#include <cstdio>
#include <cstring>

#include "games/ace/abi.h"
#include "engine/code_patch.h"
#include "games/ace/stubs.h"
#include "xr.h"

namespace {

using Fn = unsigned long long (*)(unsigned long long view);

__declspec(noinline) void count_call(int* counter) { ++*counter; }

unsigned char* alloc_near_module() {
    const auto base = aceqv::module_base();
    for (uintptr_t offset = 0x100000; offset < 0x40000000; offset += 0x10000) {
        void* p = VirtualAlloc(reinterpret_cast<void*>(base + 0x10000000 + offset), 0x1000, MEM_RESERVE | MEM_COMMIT,
                               PAGE_EXECUTE_READWRITE);
        if (p)
            return static_cast<unsigned char*>(p);
    }
    return nullptr;
}

// push rbx,rbp,rdi,rsi,r12-r15; mov src, rcx; <imul>; mov rax, dst; pop ...; ret
size_t emit(unsigned char* p, const unsigned char (&imul)[7]) {
    const unsigned char rex = imul[0], modrm = imul[2];
    const int dst = ((modrm >> 3) & 7) | ((rex & 4) ? 8 : 0), src = (modrm & 7) | ((rex & 1) ? 8 : 0);
    static const unsigned char push[] = {0x53, 0x55, 0x57, 0x56, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57};
    static const unsigned char pop[] = {0x41, 0x5F, 0x41, 0x5E, 0x41, 0x5D, 0x41, 0x5C, 0x5E, 0x5F, 0x5D, 0x5B, 0xC3};
    size_t n = 0;
    std::memcpy(p + n, push, sizeof(push));
    n += sizeof(push);
    p[n++] = static_cast<unsigned char>(0x48 | ((src >= 8) << 2));  // mov src, rcx
    p[n++] = 0x8B;
    p[n++] = static_cast<unsigned char>(0xC0 | ((src & 7) << 3) | 1);
    std::memcpy(p + n, imul, 7);
    n += 7;
    p[n++] = static_cast<unsigned char>(0x48 | (dst >= 8 ? 1 : 0));  // mov rax, dst
    p[n++] = 0x8B;
    p[n++] = static_cast<unsigned char>(0xC0 | (dst & 7));
    std::memcpy(p + n, pop, sizeof(pop));
    n += sizeof(pop);
    return n;
}

}  // namespace

int main() {
    unsigned char* page = alloc_near_module();
    if (!page) {
        std::puts("RESULT: FAIL (no memory near the module)");
        return 1;
    }
    int failures = 0;
    size_t offset = 0;
    for (const auto& site : ace::kCameraSlotIndexSites) {
        unsigned char* fn = page + offset;
        const size_t size = emit(fn, site.anchor);
        offset += (size + 15) & ~size_t{15};
        const uintptr_t imulRva = reinterpret_cast<uintptr_t>(fn) + 12 + 3 - aceqv::module_base();
        const auto call = reinterpret_cast<Fn>(fn);
        bool before = call(3) == 3 * 0x184;
        if (!aceqv::code_mask_imul_index(imulRva, site.anchor, 1)) {
            std::printf("site 0x%llx: patch refused\n", static_cast<unsigned long long>(site.rva));
            ++failures;
            continue;
        }
        unsigned long long got[4];
        bool ok = before;
        for (unsigned long long v = 0; v < 4; ++v) {
            got[v] = call(v);
            ok &= got[v] == (v & 1) * 0x184;
        }
        std::printf("site 0x%llx: before %s, after views 0..3 -> 0x%llx 0x%llx 0x%llx 0x%llx %s\n",
                    static_cast<unsigned long long>(site.rva), before ? "ok" : "WRONG", got[0], got[1], got[2], got[3],
                    ok ? "ok" : "WRONG");
        failures += !ok;
    }
    // History-slot indexes: `movsxd dst, view32; shl dst, 5` -> (view & 1) << 5.
    for (const auto& site : ace::kHistorySlotIndexSites) {
        unsigned char* fn = page + offset;
        const unsigned char modrm = site.anchor[2];
        const int src = modrm & 7, dst = (modrm >> 3) & 7;  // the sites use legacy registers only
        static const unsigned char push[] = {0x53, 0x57, 0x56};
        static const unsigned char pop[] = {0x5E, 0x5F, 0x5B, 0xC3};
        size_t n = 0;
        std::memcpy(fn + n, push, sizeof(push));
        n += sizeof(push);
        fn[n++] = 0x8B;  // mov src32, ecx
        fn[n++] = static_cast<unsigned char>(0xC0 | (src << 3) | 1);
        const size_t siteAt = n;
        std::memcpy(fn + n, site.anchor, 7);
        n += 7;
        fn[n++] = 0x48;  // mov rax, dst
        fn[n++] = 0x8B;
        fn[n++] = static_cast<unsigned char>(0xC0 | dst);
        std::memcpy(fn + n, pop, sizeof(pop));
        n += sizeof(pop);
        offset += (n + 15) & ~size_t{15};
        const auto call = reinterpret_cast<Fn>(fn);
        const bool before = call(3) == (3u << 5);
        const uintptr_t rva = reinterpret_cast<uintptr_t>(fn + siteAt) - aceqv::module_base();
        if (!aceqv::code_mask_movsxd_shl_index(rva, site.anchor, 1)) {
            std::printf("history site 0x%llx: patch refused\n", static_cast<unsigned long long>(site.rva));
            ++failures;
            continue;
        }
        bool ok = before;
        unsigned long long got[4];
        for (unsigned long long v = 0; v < 4; ++v) {
            got[v] = call(v);
            ok &= got[v] == ((v & 1) << 5);
        }
        std::printf("history site 0x%llx: before %s, after views 0..3 -> 0x%llx 0x%llx 0x%llx 0x%llx %s\n",
                    static_cast<unsigned long long>(site.rva), before ? "ok" : "WRONG", got[0], got[1], got[2], got[3],
                    ok ? "ok" : "WRONG");
        failures += !ok;
    }

    // Pass view reads: `mov edx, [base + 0x9ac]` -> view & 1.
    for (const auto& site : ace::kPassViewReadSites) {
        unsigned char* fn = page + offset;
        const int base = site.anchor[1] & 7;  // rdi or rbx
        size_t n = 0;
        fn[n++] = 0x53;  // push rbx
        fn[n++] = 0x57;  // push rdi
        fn[n++] = 0x48;  // mov base, rcx
        fn[n++] = 0x8B;
        fn[n++] = static_cast<unsigned char>(0xC0 | (base << 3) | 1);
        const size_t siteAt = n;
        std::memcpy(fn + n, site.anchor, 6);
        n += 6;
        fn[n++] = 0x8B;  // mov eax, edx
        fn[n++] = 0xC2;
        fn[n++] = 0x5F;  // pop rdi
        fn[n++] = 0x5B;  // pop rbx
        fn[n++] = 0xC3;
        offset += (n + 15) & ~size_t{15};
        static int pass[0x9ac / 4 + 1];
        const auto call = reinterpret_cast<int (*)(void*)>(fn);
        pass[0x9ac / 4] = 3;
        const bool before = call(pass) == 3;
        const uintptr_t rva = reinterpret_cast<uintptr_t>(fn + siteAt) - aceqv::module_base();
        const bool patched = aceqv::code_mask_load32(rva, site.anchor, 1);
        int got[4];
        bool ok = before && patched;
        for (int v = 0; v < 4; ++v) {
            pass[0x9ac / 4] = v;
            got[v] = call(pass);
            ok &= got[v] == (v & 1);
        }
        std::printf("pass view site 0x%llx: before %s, after views 0..3 -> %d %d %d %d %s\n",
                    static_cast<unsigned long long>(site.rva), before ? "ok" : "WRONG", got[0], got[1], got[2], got[3],
                    ok ? "ok" : "WRONG");
        failures += !ok;
    }

    // Velocity pass entry: `movsxd rbp, r8d; mov rbx, rdx` -> rbp = view & 1, rbx unchanged.
    {
        unsigned char* fn = page + offset;
        size_t n = 0;
        fn[n++] = 0x55;  // push rbp
        fn[n++] = 0x53;  // push rbx
        const size_t siteAt = n;
        std::memcpy(fn + n, ace::kVelocityPassViewAnchor, sizeof(ace::kVelocityPassViewAnchor));
        n += sizeof(ace::kVelocityPassViewAnchor);
        const unsigned char tail[] = {0x48, 0x8B, 0xC5,   // mov rax, rbp
                                      0x48, 0x2B, 0xC3,   // sub rax, rbx  (rbx = rdx = 0x100)
                                      0x5B, 0x5D, 0xC3};  // pop rbx; pop rbp; ret
        std::memcpy(fn + n, tail, sizeof(tail));
        n += sizeof(tail);
        offset += (n + 15) & ~size_t{15};
        const auto call = reinterpret_cast<long long (*)(void*, void*, long long)>(fn);
        void* const rdx = reinterpret_cast<void*>(0x100);
        const bool before = call(nullptr, rdx, 3) == 3 - 0x100;
        const uintptr_t rva = reinterpret_cast<uintptr_t>(fn + siteAt) - aceqv::module_base();
        const bool patched = aceqv::code_redirect(rva, ace::kVelocityPassViewAnchor, sizeof(ace::kVelocityPassViewAnchor),
                                                  ace::kVelocityPassViewStub, sizeof(ace::kVelocityPassViewStub));
        long long got[4];
        bool ok = before && patched;
        for (long long v = 0; v < 4; ++v) {
            got[v] = call(nullptr, rdx, v) + 0x100;
            ok &= got[v] == (v & 1);
        }
        std::printf("velocity pass view: before %s, after views 0..3 -> %lld %lld %lld %lld %s\n", before ? "ok" : "WRONG",
                    got[0], got[1], got[2], got[3], ok ? "ok" : "WRONG");
        failures += !ok;
    }

    // Slot advance: `call rel32` -> called only for views 0 and 1 (view in esi or edi).
    const unsigned char viewRegisters[] = {6, 7};  // esi, edi
    for (const unsigned char reg : viewRegisters) {
        unsigned char* fn = page + offset;
        size_t n = 0;
        fn[n++] = static_cast<unsigned char>(0x50 | reg);  // push esi/edi (64-bit)
        fn[n++] = 0x8B;                                    // mov reg32, ecx (view)
        fn[n++] = static_cast<unsigned char>(0xC0 | (reg << 3) | 1);
        fn[n++] = 0x48;  // mov rcx, rdx (counter)
        fn[n++] = 0x8B;
        fn[n++] = 0xCA;
        fn[n++] = 0x48;  // sub rsp, 0x28
        fn[n++] = 0x83;
        fn[n++] = 0xEC;
        fn[n++] = 0x28;
        const size_t siteAt = n;
        const int32_t rel = static_cast<int32_t>(reinterpret_cast<intptr_t>(&count_call) -
                                                 reinterpret_cast<intptr_t>(fn + siteAt + 5));
        unsigned char call[5] = {0xE8};
        std::memcpy(call + 1, &rel, 4);
        std::memcpy(fn + n, call, 5);
        n += 5;
        const unsigned char tail[] = {0x48, 0x83, 0xC4, 0x28, static_cast<unsigned char>(0x58 | reg), 0xC3};
        std::memcpy(fn + n, tail, sizeof(tail));
        n += sizeof(tail);
        offset += (n + 15) & ~size_t{15};
        // A fake slot: the counter count_call increments at +0, the per-frame flag at +0x18.
        struct alignas(8) Slot {
            int counter;
            unsigned char pad[ace::kSlotFrameFlag - 4];
            unsigned char flag;
        } slot{};
        const auto run = reinterpret_cast<void (*)(long long, Slot*)>(fn);
        run(3, &slot);
        const bool before = slot.counter == 1;
        const uintptr_t rva = reinterpret_cast<uintptr_t>(fn + siteAt) - aceqv::module_base();
        const bool patched = aceqv::code_call_if_below(rva, call, reinterpret_cast<uintptr_t>(&count_call), reg, 2,
                                                       ace::kSlotFrameFlag);
        int calls[4], flags[4];
        bool ok = before && patched;
        for (int v = 0; v < 4; ++v) {
            slot.counter = 0;
            slot.flag = 1;
            run(v, &slot);
            calls[v] = slot.counter;
            flags[v] = slot.flag;
            ok &= slot.counter == (v < 2 ? 1 : 0) && slot.flag == (v < 2 ? 1 : 0);
        }
        std::printf("slot advance (reg %d): before %s, views 0..3 counter %d %d %d %d, flag %d %d %d %d %s\n", reg,
                    before ? "ok" : "WRONG", calls[0], calls[1], calls[2], calls[3], flags[0], flags[1], flags[2],
                    flags[3], ok ? "ok" : "WRONG");
        failures += !ok;
    }

    // views4 "slots": views 2 and 3 -> phantom renderers P2 and P3, views 0-1 unchanged.
    const uint64_t phantom = 0x123456789A00ull, phantom2 = 0x0FEDCBA98000ull, renderer = 0x7FF612340000ull;
    const auto pick = [&](unsigned long long v) { return v == 2 ? phantom2 : phantom; };
    using Fn2 = unsigned long long (*)(unsigned long long, unsigned long long, unsigned long long);
    // Builds: push saved; <setup>; <site>; <result into rax>; pop; ret. Returns the site's rva.
    const auto build = [&](std::initializer_list<unsigned char> saved, std::initializer_list<unsigned char> setup,
                           const unsigned char* site, size_t siteSize, std::initializer_list<unsigned char> result,
                           std::initializer_list<unsigned char> restore, Fn2* fn) {
        unsigned char* p = page + offset;
        size_t n = 0;
        for (auto b : saved) p[n++] = b;
        for (auto b : setup) p[n++] = b;
        const size_t at = n;
        std::memcpy(p + n, site, siteSize);
        n += siteSize;
        for (auto b : result) p[n++] = b;
        for (auto b : restore) p[n++] = b;
        p[n++] = 0xC3;
        offset += (n + 15) & ~size_t{15};
        *fn = reinterpret_cast<Fn2>(p);
        return reinterpret_cast<uintptr_t>(p + at) - aceqv::module_base();
    };
    bool patchedNow = false;
    const auto check = [&](const char* name, Fn2 fn, uintptr_t rva, const unsigned char* anchor, size_t size,
                           const std::vector<uint8_t>& stub, auto expect, auto args) {
        bool ok = true;
        patchedNow = false;
        for (unsigned long long v = 0; v < 3; ++v)  // before the patch: the original for every view
            ok &= fn(args(v).a, args(v).b, args(v).c) == expect(v, false);
        ok &= aceqv::code_redirect(rva, anchor, size, stub.data(), stub.size());
        patchedNow = true;
        unsigned long long got[4];
        for (unsigned long long v = 0; v < 4; ++v) {
            got[v] = fn(args(v).a, args(v).b, args(v).c);
            ok &= got[v] == expect(v, true);
        }
        std::printf("%s: views 0..3 -> 0x%llx 0x%llx 0x%llx 0x%llx %s\n", name, got[0], got[1], got[2], got[3],
                    ok ? "ok" : "WRONG");
        failures += !ok;
    };
    struct Args {
        unsigned long long a, b, c;
    };
    for (const auto& site : ace::kHistorySlotIndexSites) {
        const unsigned char modrm = site.anchor[2], dst = (modrm >> 3) & 7, src = modrm & 7;
        Fn2 fn = nullptr;
        const uintptr_t rva = build({0x53, 0x57, 0x56, 0x41, 0x55},                                   // rbx rdi rsi r13
                                    {0x4C, 0x8B, 0xEA, 0x8B, static_cast<unsigned char>(0xC1 | (src << 3))},  // r13 = rdx; src = ecx
                                    site.anchor, 7, {0x48, 0x8B, static_cast<unsigned char>(0xC0 | dst)},
                                    {0x41, 0x5D, 0x5E, 0x5F, 0x5B}, &fn);
        char name[64];
        std::snprintf(name, sizeof(name), "slots history index 0x%llx", static_cast<unsigned long long>(site.rva));
        check(name, fn, rva, site.anchor, 7, aceqv::stub_history_index(site.anchor, phantom2, phantom),
              [&](unsigned long long v, bool patched) { return patched && v >= 2 ? pick(v) - renderer : v << 5; },
              [&](unsigned long long v) { return Args{v, renderer, 0}; });
    }
    {
        Fn2 fn = nullptr;
        const uintptr_t rva = build({0x41, 0x54, 0x41, 0x55, 0x41, 0x56}, {0x4C, 0x8B, 0xE1, 0x4C, 0x8B, 0xEA},  // r12 = rcx; r13 = rdx
                                    ace::kCreatorBaseAnchor, 10, {0x49, 0x8B, 0xC6}, {0x41, 0x5E, 0x41, 0x5D, 0x41, 0x5C}, &fn);
        check("slots creator base", fn, rva, ace::kCreatorBaseAnchor, 10,
              aceqv::stub_creator_base(ace::kCreatorBaseAnchor, phantom2, phantom),
              [&](unsigned long long v, bool patched) { return patched && v >= 2 ? pick(v) : renderer + (v << 5); },
              [&](unsigned long long v) { return Args{v, renderer, 0}; });
    }
    {
        Fn2 fn = nullptr;
        const uintptr_t rva = build({0x41, 0x54, 0x41, 0x55, 0x56}, {0x4C, 0x8B, 0xE1, 0x4C, 0x8B, 0xEA},
                                    ace::kCreatorIndexAnchor, 7, {0x48, 0x8B, 0xC6}, {0x5E, 0x41, 0x5D, 0x41, 0x5C}, &fn);
        check("slots creator index", fn, rva, ace::kCreatorIndexAnchor, 7,
              aceqv::stub_creator_index(ace::kCreatorIndexAnchor, phantom2, phantom),
              [&](unsigned long long v, bool patched) { return patched && v >= 2 ? pick(v) - renderer : v << 5; },
              [&](unsigned long long v) { return Args{v, renderer, 0}; });
    }
    {
        // esi = view (already incremented), rdi = what the previous iteration left: renderer + field +
        // (view - 1) * 0x20, except that view 3 follows view 2's redirect (P2 + field).
        const unsigned long long field = 0x75f8;
        Fn2 fn = nullptr;
        const uintptr_t rva = build({0x56, 0x57, 0x41, 0x55}, {0x8B, 0xF1, 0x48, 0x8B, 0xFA, 0x4D, 0x8B, 0xE8},  // esi, rdi, r13
                                    ace::kImportStepAnchor, 7, {0x48, 0x8B, 0xC7}, {0x41, 0x5D, 0x5F, 0x5E}, &fn);
        check("slots import step", fn, rva, ace::kImportStepAnchor, 7,
              aceqv::stub_import_step(ace::kImportStepAnchor, phantom2, phantom),
              [&](unsigned long long v, bool patched) {
                  return patched && v >= 2 ? pick(v) + field : renderer + field + v * 0x20;
              },
              [&](unsigned long long v) {
                  // Before the patch every iteration is renderer-relative; after it, view 3 starts
                  // from where view 2 was redirected. `patchedNow` is set by check() between phases.
                  const bool chained = v == 3 && patchedNow;
                  return Args{v, chained ? phantom2 + field : renderer + field + (v - 1) * 0x20, renderer};
              });
    }

    // views4 "cameras": camera-slot `imul dst, view, 0x184` -> view 3: C' - base.
    for (size_t i = 0; i < std::size(ace::kCameraSlotIndexSites); ++i) {
        const auto& site = ace::kCameraSlotIndexSites[i];
        const unsigned char rex = site.anchor[0], modrm = site.anchor[2];
        const int dst = ((modrm >> 3) & 7) | ((rex & 4) ? 8 : 0), src = (modrm & 7) | ((rex & 1) ? 8 : 0);
        const int baseReg = ace::kCameraSlotIndexBase[i];
        // Save every callee-saved register; src = rcx, base = rdx; result = dst.
        const auto mov = [](int to, int from) {  // mov r64(to), r64(from)
            return std::vector<unsigned char>{static_cast<unsigned char>(0x48 | (to >= 8 ? 4 : 0) | (from >= 8 ? 1 : 0)),
                                              0x8B, static_cast<unsigned char>(0xC0 | ((to & 7) << 3) | (from & 7))};
        };
        unsigned char* p = page + offset;
        size_t n = 0;
        static const unsigned char push[] = {0x53, 0x55, 0x57, 0x56, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57};
        static const unsigned char pop[] = {0x41, 0x5F, 0x41, 0x5E, 0x41, 0x5D, 0x41, 0x5C, 0x5E, 0x5F, 0x5D, 0x5B, 0xC3};
        std::memcpy(p + n, push, sizeof(push));
        n += sizeof(push);
        for (auto b : mov(baseReg, 2)) p[n++] = b;  // base = rdx (before src, in case src is rdx-free)
        for (auto b : mov(src, 1)) p[n++] = b;      // view = rcx
        const size_t at = n;
        std::memcpy(p + n, site.anchor, 7);
        n += 7;
        for (auto b : mov(0, dst)) p[n++] = b;  // rax = dst
        std::memcpy(p + n, pop, sizeof(pop));
        n += sizeof(pop);
        offset += (n + 15) & ~size_t{15};
        const auto fn = reinterpret_cast<Fn2>(p);
        const uint64_t cameraPhantom = 0x2222333344440000ull, cameraPhantom2 = 0x1111222233330000ull;
        char name[80];
        std::snprintf(name, sizeof(name), "cameras slot index 0x%llx (base r%d)", static_cast<unsigned long long>(site.rva),
                      baseReg);
        check(name, fn, reinterpret_cast<uintptr_t>(p + at) - aceqv::module_base(), site.anchor, 7,
              aceqv::stub_camera_index(site.anchor, static_cast<uint8_t>(baseReg), cameraPhantom2, cameraPhantom),
              [&](unsigned long long v, bool patched) {
                  return patched && v >= 2 ? (v == 2 ? cameraPhantom2 : cameraPhantom) - renderer : v * 0x184;
              },
              [&](unsigned long long v) { return Args{v, renderer, 0}; });
    }

    // Pass setup eye branch: view != 0 -> slot-1 path becomes view & 1, and views 2-3 run it with rdi
    // = the phantom renderer. A local copy of the site with a short rel32 (the stub takes the target
    // itself). Result: bit 0 = right path, bit 1 = rdi is the phantom (r8).
    {
        static const unsigned char site[9] = {0x45, 0x85, 0xE4, 0x0F, 0x85, 0x07, 0x00, 0x00, 0x00};
        const uint64_t phantomRenderer = 0x0000123400560000ull;
        Fn2 fn = nullptr;
        const uintptr_t rva = build({0x41, 0x54, 0x57}, {0x4C, 0x8B, 0xE1, 0x48, 0x8B, 0xFA},  // r12 = rcx; rdi = rdx
                                    site, 9,
                                    {0xB8, 0x00, 0x00, 0x00, 0x00, 0xEB, 0x05, 0xB8, 0x01, 0x00, 0x00, 0x00,  // path
                                     0x4C, 0x39, 0xC7, 0x75, 0x03, 0x83, 0xC8, 0x02},  // cmp rdi, r8; jne; or eax, 2
                                    {0x5F, 0x41, 0x5C}, &fn);
        const uint64_t right = aceqv::module_base() + rva + 9 + 7;
        check("cameras eye branch", fn, rva, site, 9, aceqv::stub_eye_branch(right, phantomRenderer),
              [&](unsigned long long v, bool patched) {
                  return patched ? (v & 1) | (v >= 2 ? 2ull : 0ull) : (v != 0 ? 1ull : 0ull);
              },
              [&](unsigned long long v) { return Args{v, renderer, phantomRenderer}; });
    }
    // The paths' rejoin: views 2-3 restore rdi from the phantom's renderer field; the original store
    // still runs (rsi = a scratch pass). Result: rdi.
    {
        static uint64_t phantom[4] = {};
        static unsigned char pass[0x460] = {};
        phantom[ace::kPhantomRenderer / 8] = renderer;
        Fn2 fn = nullptr;
        const uintptr_t rva = build({0x41, 0x54, 0x57, 0x56}, {0x4C, 0x8B, 0xE1, 0x48, 0x8B, 0xFA, 0x49, 0x8B, 0xF0},
                                    ace::kPassRejoinAnchor, 6, {0x48, 0x8B, 0xC7}, {0x5E, 0x5F, 0x41, 0x5C}, &fn);
        const auto rdiFor = [&](unsigned long long v) { return v >= 2 ? reinterpret_cast<uint64_t>(phantom) : renderer; };
        check("cameras pass rejoin", fn, rva, ace::kPassRejoinAnchor, 6,
              aceqv::stub_pass_rejoin(ace::kPassRejoinAnchor, static_cast<uint8_t>(ace::kPhantomRenderer)),
              [&](unsigned long long v, bool patched) { return patched ? renderer : rdiFor(v); },
              [&](unsigned long long v) { return Args{v, rdiFor(v), reinterpret_cast<uint64_t>(pass)}; });
    }

    // Normal reconstruction dispatch size: edx = (r14 + 7) >> 3 unless the override is nonzero. The
    // test passes the width as r14 = rcx; r8d = rdx just carries through the original's shift.
    {
        static uint32_t groupsOverride = 0;
        Fn2 fn = nullptr;
        const uintptr_t rva = build({0x41, 0x56}, {0x4C, 0x8B, 0xF1, 0x4C, 0x8B, 0xC2},  // r14 = rcx; r8 = rdx
                                    ace::kNormalReconstructDispatchAnchor, 11, {0x8B, 0xC2}, {0x41, 0x5E}, &fn);  // eax = edx
        bool ok = fn(6456, 1350, 0) == (6456 + 7) / 8;
        ok &= aceqv::code_redirect(rva, ace::kNormalReconstructDispatchAnchor, 11,
                                   aceqv::stub_dispatch_x_override(ace::kNormalReconstructDispatchAnchor,
                                                                   reinterpret_cast<uint64_t>(&groupsOverride)).data(),
                                   aceqv::stub_dispatch_x_override(ace::kNormalReconstructDispatchAnchor,
                                                                   reinterpret_cast<uint64_t>(&groupsOverride)).size());
        const unsigned long long plain = fn(6456, 1350, 0);
        groupsOverride = (2 * 1614 + 7) / 8;
        const unsigned long long capped = fn(6456, 1350, 0);
        groupsOverride = 0;
        ok &= plain == (6456 + 7) / 8 && capped == (2 * 1614 + 7) / 8;
        std::printf("normal reconstruction dispatch: %llu groups, capped %llu %s\n", plain, capped, ok ? "ok" : "WRONG");
        failures += !ok;
    }

    // Tile culling eye-structure probe: rdi = [rbp+0x98] as before, and table[view] = rdi.
    {
        static uint64_t table[4] = {};
        static uint64_t frame[0x20] = {};  // [rbp + 0x98] = frame[0x13]
        Fn2 fn = nullptr;
        const uintptr_t rva = build({0x53, 0x57, 0x55}, {0x8B, 0xD9, 0x48, 0x8B, 0xEA},  // ebx = ecx; rbp = rdx
                                    ace::kTileCullingEyeStructAnchor, 7, {0x48, 0x8B, 0xC7}, {0x5D, 0x5F, 0x5B}, &fn);  // rax = rdi
        const auto stub = aceqv::stub_record_rdi_by_view(ace::kTileCullingEyeStructAnchor, reinterpret_cast<uint64_t>(table));
        bool ok = aceqv::code_redirect(rva, ace::kTileCullingEyeStructAnchor, 7, stub.data(), stub.size());
        for (unsigned long long v = 0; v < 4; ++v) {
            frame[0x13] = 0xABC000 + v;
            ok &= fn(v, reinterpret_cast<uint64_t>(frame), 0) == 0xABC000 + v && table[v] == 0xABC000 + v;
        }
        std::printf("tile culling eye-structure probe: %s\n", ok ? "ok" : "WRONG");
        failures += !ok;
    }

    // Lighting loop bound: `mov eax,[r13+mode]; inc eax` -> 4 for mode 1, else mode + 1.
    {
        unsigned char* fn = page + offset;
        size_t n = 0;
        fn[n++] = 0x41;  // push r13
        fn[n++] = 0x55;
        fn[n++] = 0x4C;  // mov r13, rcx
        fn[n++] = 0x8B;
        fn[n++] = 0xE9;
        const size_t siteAt = n;
        std::memcpy(fn + n, ace::kLightingLoopBoundAnchor, sizeof(ace::kLightingLoopBoundAnchor));
        n += sizeof(ace::kLightingLoopBoundAnchor);
        fn[n++] = 0x41;  // pop r13
        fn[n++] = 0x5D;
        fn[n++] = 0xC3;
        offset += (n + 15) & ~size_t{15};
        static int renderer[(ace::kRendererActiveViewMode + 4) / 4];
        int* mode = &renderer[ace::kRendererActiveViewMode / 4];
        const auto call = reinterpret_cast<int (*)(void*)>(fn);
        *mode = 1;
        const bool before = call(renderer) == 2;
        const uintptr_t rva = reinterpret_cast<uintptr_t>(fn + siteAt) - aceqv::module_base();
        const bool patched = aceqv::code_redirect(rva, ace::kLightingLoopBoundAnchor, sizeof(ace::kLightingLoopBoundAnchor),
                                                  ace::kLightingLoopBoundStub, sizeof(ace::kLightingLoopBoundStub));
        int got[3];
        for (int m = 0; m < 3; ++m) {
            *mode = m;
            got[m] = call(renderer);
        }
        const bool ok = before && patched && got[0] == 1 && got[1] == 4 && got[2] == 3;
        std::printf("lighting bound: before %s, after modes 0/1/2 -> %d/%d/%d %s\n", before ? "ok" : "WRONG", got[0],
                    got[1], got[2], ok ? "ok" : "WRONG");
        failures += !ok;
    }

    std::printf("RESULT: %s\n", failures ? "FAIL" : "pass");
    return failures ? 1 : 0;
}
