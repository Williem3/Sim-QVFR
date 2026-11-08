"""Find the code that references any address in a data range (RIP-relative), in a large x64 PE without symbols.

Usage: python rip_refs.py <exe> <start_rva_hex> <end_rva_hex> [--max N]
Globals reached by base + index (per-eye slots, arrays of structs) often have no cross-reference at the element a
disassembler shows: the code takes the address of the base or of a neighbour. This lists every instruction whose
RIP-relative operand lands in [start, end): a vectorised scan for 32-bit displacements that hit the range, each
confirmed by decoding the instruction with capstone. Output: rva, function start (.pdata), target, instruction.
"""
import bisect
import struct
import sys

import capstone
import numpy as np
import pefile


def main(exe, start, end, limit):
    pe = pefile.PE(exe, fast_load=True)
    base = pe.OPTIONAL_HEADER.ImageBase
    text = next(s for s in pe.sections if s.Name.startswith(b".text"))
    code, tb = text.get_data(), text.VirtualAddress
    pdata = next(s for s in pe.sections if s.Name.startswith(b".pdata")).get_data()
    funcs = sorted((b, e) for b, e, _ in struct.iter_unpack("<III", pdata[: len(pdata) // 12 * 12]) if b)
    starts = [f[0] for f in funcs]

    def func_of(rva):
        i = bisect.bisect_right(starts, rva) - 1
        return funcs[i][0] if i >= 0 and funcs[i][0] <= rva < funcs[i][1] else None

    a = np.frombuffer(code, dtype=np.uint8).astype(np.int64)
    n = len(a) - 4
    disp = (a[:n] | (a[1:n + 1] << 8) | (a[2:n + 2] << 16) | (a[3:n + 3] << 24)).astype(np.int64)
    disp = np.where(disp >= 1 << 31, disp - (1 << 32), disp)
    offs = np.arange(n, dtype=np.int64)
    cand = set()
    for k in (4, 5, 6, 8):  # bytes from the displacement to the instruction's end (immediates after it)
        t = tb + offs + k + disp
        cand.update(int(o) for o in np.nonzero((t >= start) & (t < end))[0])
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
    md.detail = True
    found = {}
    for o in sorted(cand):
        # The instruction starts 1-8 bytes before its displacement. Try the longest prefix first and keep only a decode
        # whose displacement field really sits at `o` (a start inside an instruction decodes as something else).
        for back in range(8, 0, -1):
            s = o - back
            if s < 0:
                continue
            ins = next(md.disasm(code[s:s + 16], base + tb + s), None)
            if ins is None or ins.size < back + 4 or getattr(ins, "disp_offset", back) != back:
                continue
            for op in ins.operands:
                if op.type == capstone.x86.X86_OP_MEM and op.mem.base == capstone.x86.X86_REG_RIP:
                    target = ins.address + ins.size + op.mem.disp - base
                    if start <= target < end and ins.address - base not in found:
                        found[ins.address - base] = (target, f"{ins.mnemonic} {ins.op_str}")
            if (s + tb) in found:
                break
    print(f"{len(found)} reference(s) into 0x{start:x}-0x{end:x}")
    for i, (rva, (target, text_)) in enumerate(sorted(found.items())):
        if i >= limit:
            print(f"... {len(found) - limit} more (--max)")
            break
        f = func_of(rva)
        print(f"0x{rva:x}  fn 0x{f:x}  -> 0x{target:x}  {text_}" if f else f"0x{rva:x}  fn ?  -> 0x{target:x}  {text_}")


if __name__ == "__main__":
    args = [x for x in sys.argv[1:] if not x.startswith("--")]
    lim = 400
    if "--max" in sys.argv:
        lim = int(sys.argv[sys.argv.index("--max") + 1])
        args = [x for x in args if x != str(lim)]
    if len(args) != 3:
        print(__doc__)
        sys.exit(2)
    main(args[0], int(args[1], 16), int(args[2], 16), lim)
