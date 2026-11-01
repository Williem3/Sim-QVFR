"""Find instructions whose memory operand uses given 32-bit displacements (struct field offsets).

Usage: python disp_refs.py <exe> <disp_hex> [<disp_hex> ...]
Byte-scans .text for each displacement, then disassembles backwards from a few start offsets to find
an instruction that contains it as a memory displacement. Prints rva, function start, and the text.
"""
import bisect
import struct
import sys

import capstone
import pefile


def main(exe, disps):
    pe = pefile.PE(exe, fast_load=True)
    base = pe.OPTIONAL_HEADER.ImageBase
    text = next(s for s in pe.sections if s.Name.startswith(b".text"))
    code = text.get_data()
    tva = text.VirtualAddress
    pdata = next(s for s in pe.sections if s.Name.startswith(b".pdata")).get_data()
    funcs = sorted((b, e) for b, e, _ in struct.iter_unpack("<III", pdata[: len(pdata) // 12 * 12]) if b)
    starts = [f[0] for f in funcs]
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
    md.detail = True
    for d in disps:
        value = int(d, 16)
        needle = struct.pack("<i", value)
        pos = code.find(needle)
        seen = set()
        while pos != -1:
            for back in range(2, 9):
                off = pos - back
                if off < 0:
                    continue
                ins = next(md.disasm(code[off : off + 16], base + tva + off), None)
                if ins is None or off + ins.size <= pos:
                    continue
                if any(op.type == capstone.x86.X86_OP_MEM and op.mem.disp == value for op in ins.operands):
                    rva = tva + off
                    if rva not in seen:
                        seen.add(rva)
                        i = bisect.bisect_right(starts, rva) - 1
                        fs = funcs[i][0] if i >= 0 and funcs[i][0] <= rva < funcs[i][1] else 0
                        print(f"0x{value:x}\t0x{rva:x}\tfn 0x{fs:x}\t{ins.mnemonic} {ins.op_str}")
                    break
            pos = code.find(needle, pos + 1)


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2:])
