// Small in-place patches on a pinned game exe: anchor checks, executable memory within rel32 reach of
// the module, trampolines that replace one instruction, and vtable slots.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace simqvfr {

std::uintptr_t module_base();
// True when the code at `rva` starts with `anchor` (and is readable).
bool code_matches(std::uintptr_t rva, const std::uint8_t* anchor, std::size_t size);
// Overwrites code at `rva` (protection switched temporarily, instruction cache flushed), with the process's other
// threads suspended and none stopped inside the range (false if one stays there).
bool code_write(std::uintptr_t rva, const void* bytes, std::size_t size);
// Replaces the 7-byte `imul r64, r64, imm32` at `rva` with a jump to a stub that computes
// `dst = (src & mask) * imm32`, then continues after the original instruction. `anchor` is the
// original 7 bytes. Used to fold view indexes 2 and 3 onto 0 and 1 for fixed 2-or-3-slot arrays.
bool code_mask_imul_index(std::uintptr_t rva, const std::uint8_t (&anchor)[7], std::uint8_t mask);
// Replaces the 7-byte `movsxd dst, src32; shl dst, n` at `rva` with `dst = (src & mask) << n`.
bool code_mask_movsxd_shl_index(std::uintptr_t rva, const std::uint8_t (&anchor)[7], std::uint8_t mask);
// Appends `and r32, mask` to the 6-byte `mov r32, [base + disp32]` at `rva` (legacy registers).
bool code_mask_load32(std::uintptr_t rva, const std::uint8_t (&anchor)[6], std::uint8_t mask);
// Replaces the 5-byte `call rel32` at `rva` with `if (reg32 < limit) call target` (unsigned;
// `reg` is a legacy register number, e.g. 6 = esi, 7 = edi). rax is clobbered.
// With `elseClearByte` >= 0, the skipped case instead does `mov byte [rcx + elseClearByte], 0`.
bool code_call_if_below(std::uintptr_t rva, const std::uint8_t (&anchor)[5], std::uintptr_t target, std::uint8_t reg,
                        std::uint8_t limit, int elseClearByte = -1);
// Replaces the `size` bytes at `rva` (which must equal `anchor`, at least 5) with a jump to a stub
// running `stub` and then jumping back to rva + size. `stub` must be position independent.
bool code_redirect(std::uintptr_t rva, const std::uint8_t* anchor, std::size_t size, const std::uint8_t* stub,
                   std::size_t stubSize);
// The function the vtable slot at `slotRva` points to, as an RVA (0 if unreadable).
std::uintptr_t vtable_slot(std::uintptr_t slotRva);
// Points the vtable slot at `slotRva` at `replacement`. The slot must hold the function at `expectedRva` (the
// build's own) or already `replacement` (a second call); otherwise nothing is written.
bool vtable_slot_replace(std::uintptr_t slotRva, std::uintptr_t expectedRva, const void* replacement);

// Points slot `slot` of a live COM vtable (any module, e.g. d3d12.dll's) at `hook` and saves what it held in
// `original`, whatever that was (unlike vtable_slot_replace, no expected function). A slot already holding `hook` is
// left as is (a retry after a partial failure: saving it as the original would recurse).
bool vtable_patch(void** vtable, std::size_t slot, void* hook, void** original);
template <class Fn> bool vtable_hook(void** vtable, std::size_t slot, Fn hook, Fn* original) {
    return vtable_patch(vtable, slot, reinterpret_cast<void*>(hook), reinterpret_cast<void**>(original));
}

// True when `size` bytes at `address` are committed and readable (within one region).
bool readable(const void* address, std::size_t size);
// "module.dll+0x1234" for a code address, as the logs' call stacks print it ("?+0x<address>" outside any module).
std::string module_offset(const void* address);

// MH_Initialize that accepts MinHook already being initialised (the modules share it, in no fixed order). On another
// failure it logs "<tag>: MH_Initialize failed<outcome>" (nothing when `tag` is null) and returns false.
bool minhook_ready(const char* tag, const char* outcome = "");

}  // namespace simqvfr
