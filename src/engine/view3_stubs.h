// Stub code for views4 "slots" and "cameras": the focus views' per-view slots live at phantom
// renderers in layer memory (view 2 at P2, view 3 at P3: P + array offset = that view's slot; see
// ace_abi.h). Each builder returns position-independent code for code_redirect, which appends the
// jump back. The renderer is in r13 at every history and creator site. Registers use x64 numbering
// (rax = 0, rsi = 6, rdi = 7, r12 = 12, ...).
#pragma once

#include <cstdint>
#include <cstring>
#include <vector>

namespace aceqv {

// A tiny assembler for these stubs: bytes plus short forward jumps to labels.
class StubAsm {
public:
    std::vector<std::uint8_t> code;
    void bytes(std::initializer_list<std::uint8_t> b) { code.insert(code.end(), b); }
    void raw(const std::uint8_t* p, std::size_t n) { code.insert(code.end(), p, p + n); }
    void imm64(std::uint64_t v) {
        const auto* p = reinterpret_cast<const std::uint8_t*>(&v);
        code.insert(code.end(), p, p + 8);
    }
    // jcc rel8 / jmp rel8 to a label bound later.
    void jump(std::uint8_t opcode, int label) {
        code.push_back(opcode);
        code.push_back(0);
        fixups_.push_back({code.size() - 1, label});
    }
    void bind(int label) {
        if (static_cast<std::size_t>(label) >= labels_.size())
            labels_.resize(label + 1, -1);
        labels_[label] = static_cast<long>(code.size());
    }
    std::vector<std::uint8_t> finish() {
        for (const auto& f : fixups_)
            code[f.at] = static_cast<std::uint8_t>(labels_[f.label] - static_cast<long>(f.at + 1));
        return code;
    }
    // mov reg64, imm64
    void mov_imm64(std::uint8_t reg, std::uint64_t v) {
        bytes({static_cast<std::uint8_t>(0x48 | (reg >= 8 ? 1 : 0)), static_cast<std::uint8_t>(0xB8 | (reg & 7))});
        imm64(v);
    }
    // sub dst64, src64
    void sub(std::uint8_t dst, std::uint8_t src) {
        bytes({static_cast<std::uint8_t>(0x48 | (src >= 8 ? 4 : 0) | (dst >= 8 ? 1 : 0)), 0x29,
               static_cast<std::uint8_t>(0xC0 | ((src & 7) << 3) | (dst & 7))});
    }
    // add dst64, src64
    void add(std::uint8_t dst, std::uint8_t src) {
        bytes({static_cast<std::uint8_t>(0x48 | (src >= 8 ? 4 : 0) | (dst >= 8 ? 1 : 0)), 0x01,
               static_cast<std::uint8_t>(0xC0 | ((src & 7) << 3) | (dst & 7))});
    }
    // cmp reg32, imm8 (r64 when wide)
    void cmp_imm8(std::uint8_t reg, std::uint8_t v, bool wide = false) {
        const std::uint8_t rex = static_cast<std::uint8_t>((wide ? 0x48 : 0x40) | (reg >= 8 ? 1 : 0));
        if (rex != 0x40)
            code.push_back(rex);
        bytes({0x83, static_cast<std::uint8_t>(0xF8 | (reg & 7)), v});
    }
    static constexpr std::uint8_t kJb = 0x72, kJe = 0x74, kJmp = 0xEB;

private:
    struct Fixup {
        std::size_t at;
        int label;
    };
    std::vector<Fixup> fixups_;
    std::vector<long> labels_;
};

// The common shape: view < 2 runs the original; view 2 and view 3 load their phantom value into
// `dst` (minus `base` unless base == 0xFF).
inline std::vector<std::uint8_t> stub_by_view(std::uint8_t view, bool wideCompare, std::uint8_t dst, std::uint8_t base,
                                              std::uint64_t phantom2, std::uint64_t phantom3,
                                              const std::uint8_t* original, std::size_t originalSize) {
    enum { kOriginal, kTwo, kSub, kEnd };
    StubAsm a;
    a.cmp_imm8(view, 2, wideCompare);
    a.jump(StubAsm::kJb, kOriginal);
    a.jump(StubAsm::kJe, kTwo);
    a.mov_imm64(dst, phantom3);
    a.jump(StubAsm::kJmp, kSub);
    a.bind(kTwo);
    a.mov_imm64(dst, phantom2);
    a.bind(kSub);
    if (base != 0xFF)
        a.sub(dst, base);
    a.jump(StubAsm::kJmp, kEnd);
    a.bind(kOriginal);
    a.raw(original, originalSize);
    a.bind(kEnd);
    return a.finish();
}

// Builder index sites `movsxd dst, view32; shl dst, 5` (7 bytes, legacy registers):
// view 2 / 3 -> dst = P2 / P3 - r13, otherwise the original.
inline std::vector<std::uint8_t> stub_history_index(const std::uint8_t (&original)[7], std::uint64_t phantom2,
                                                    std::uint64_t phantom3) {
    const std::uint8_t modrm = original[2];
    return stub_by_view(modrm & 7, false, (modrm >> 3) & 7, 13, phantom2, phantom3, original, 7);
}

// Creator `mov r14, r12; shl r14, 5; add r14, r13` (10 bytes): r12 == 2 / 3 -> r14 = P2 / P3.
inline std::vector<std::uint8_t> stub_creator_base(const std::uint8_t (&original)[10], std::uint64_t phantom2,
                                                   std::uint64_t phantom3) {
    return stub_by_view(12, true, 14, 0xFF, phantom2, phantom3, original, 10);
}

// Creator `mov rsi, r12; shl rsi, 5` (7 bytes): r12 == 2 / 3 -> rsi = P2 / P3 - r13.
inline std::vector<std::uint8_t> stub_creator_index(const std::uint8_t (&original)[7], std::uint64_t phantom2,
                                                    std::uint64_t phantom3) {
    return stub_by_view(12, true, 6, 13, phantom2, phantom3, original, 7);
}

// Camera-slot sites `imul dst, view, 0x184` (7 bytes, any registers): view 2 / 3 -> dst = C2 / C3 - base.
inline std::vector<std::uint8_t> stub_camera_index(const std::uint8_t (&original)[7], std::uint8_t base,
                                                   std::uint64_t phantom2, std::uint64_t phantom3) {
    const std::uint8_t rex = original[0], modrm = original[2];
    const std::uint8_t dst = ((modrm >> 3) & 7) | ((rex & 4) ? 8 : 0), view = (modrm & 7) | ((rex & 1) ? 8 : 0);
    return stub_by_view(view, false, dst, base, phantom2, phantom3, original, 7);
}

// Pass setup eye branch `test r12d, r12d; jne target` (9 bytes) -> views 2-3 first point rdi at the
// phantom renderer (its slots 0 and 1 are the focus slots), then `test r12d, 1; jnz target`: view 2
// takes the slot-0 path, view 3 the slot-1 path.
inline std::vector<std::uint8_t> stub_eye_branch(std::uint64_t target, std::uint64_t phantomRenderer) {
    enum { kEye, kLeft };
    StubAsm a;
    a.cmp_imm8(12, 2);  // cmp r12d, 2
    a.jump(StubAsm::kJb, kEye);
    a.mov_imm64(7, phantomRenderer);  // mov rdi, phantom
    a.bind(kEye);
    a.bytes({0x41, 0xF7, 0xC4, 0x01, 0x00, 0x00, 0x00});  // test r12d, 1
    a.jump(StubAsm::kJe, kLeft);
    a.bytes({0xFF, 0x25, 0x00, 0x00, 0x00, 0x00});  // jmp [rip + 0]
    a.imm64(target);
    a.bind(kLeft);
    return a.finish();
}

// The paths' rejoin (`mov [rsi+0x44c], eax`, 6 bytes): views 2-3 restore rdi to the real renderer
// from the phantom's field at `rendererField`.
inline std::vector<std::uint8_t> stub_pass_rejoin(const std::uint8_t (&original)[6], std::uint8_t rendererField) {
    enum { kSkip };
    StubAsm a;
    a.cmp_imm8(12, 2);  // cmp r12d, 2
    a.jump(StubAsm::kJb, kSkip);
    a.bytes({0x48, 0x8B, 0x7F, rendererField});  // mov rdi, [rdi + field]
    a.bind(kSkip);
    a.raw(original, 6);
    return a.finish();
}

// The normal reconstruction's dispatch size (11 bytes, edx = x groups): runs the original, then
// edx = *override when that is nonzero (rax clobbered; the next instruction sets up a call).
inline std::vector<std::uint8_t> stub_dispatch_x_override(const std::uint8_t (&original)[11], std::uint64_t overrideAddress) {
    enum { kKeep };
    StubAsm a;
    a.raw(original, 11);
    a.mov_imm64(0, overrideAddress);  // mov rax, &override
    a.bytes({0x8B, 0x00});            // mov eax, [rax]
    a.bytes({0x85, 0xC0});            // test eax, eax
    a.jump(StubAsm::kJe, kKeep);
    a.bytes({0x89, 0xC2});  // mov edx, eax
    a.bind(kKeep);
    return a.finish();
}

// Diagnostic: after the original 7 bytes (`mov rdi, [rbp+0x98]`), store rdi into table[ebx & 3]
// (ebx = the view). rax clobbered; the next instruction loads eax.
inline std::vector<std::uint8_t> stub_record_rdi_by_view(const std::uint8_t (&original)[7], std::uint64_t table) {
    StubAsm a;
    a.raw(original, 7);
    a.mov_imm64(0, table);                    // mov rax, table
    a.bytes({0x53});                          // push rbx
    a.bytes({0x83, 0xE3, 0x03});              // and ebx, 3
    a.bytes({0x48, 0x89, 0x3C, 0xD8});        // mov [rax + rbx*8], rdi
    a.bytes({0x5B});                          // pop rbx
    return a.finish();
}

// Import loop step `add rdi, 0x20; mov rcx, r13` (7 bytes, view in esi already incremented). The
// loop steps one pointer: entering view 2, rdi = r13 + field + 2 * 0x20 -> P2 + field; entering
// view 3, rdi is already in P2 (P2 + field + 0x20 after the add) -> P3 + field. rax clobbered.
inline std::vector<std::uint8_t> stub_import_step(const std::uint8_t (&original)[7], std::uint64_t phantom2,
                                                  std::uint64_t phantom3) {
    enum { kAfter, kTwo, kAdd };
    StubAsm a;
    a.raw(original, 4);  // add rdi, 0x20
    a.cmp_imm8(6, 2);    // cmp esi, 2
    a.jump(StubAsm::kJb, kAfter);
    a.jump(StubAsm::kJe, kTwo);
    a.mov_imm64(0, phantom3 - phantom2 - 0x20);  // view 3: from P2 + field + 0x20 to P3 + field
    a.jump(StubAsm::kJmp, kAdd);
    a.bind(kTwo);
    a.mov_imm64(0, phantom2 - 2 * 0x20);  // view 2: from r13 + field + 0x40 to P2 + field
    a.sub(0, 13);                          // rax -= r13
    a.bind(kAdd);
    a.add(7, 0);  // rdi += rax
    a.bind(kAfter);
    a.raw(original + 4, 3);  // mov rcx, r13
    return a.finish();
}

}  // namespace aceqv
