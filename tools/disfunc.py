"""Disassemble one function (by any RVA inside it) from a large x64 PE, annotating string refs.

Usage: python disfunc.py <exe> <rva_hex> [max_instructions]
Function bounds come from .pdata. RIP-relative operands that land on printable strings are shown.
"""
import bisect
import re
import struct
import sys

import capstone
import pefile


def main(exe, rva_hex, limit="4000"):
    pe = pefile.PE(exe, fast_load=True)
    rva = int(rva_hex, 16)
    pdata = next(s for s in pe.sections if s.Name.startswith(b".pdata")).get_data()
    funcs = sorted((b, e) for b, e, _ in struct.iter_unpack("<III", pdata[: len(pdata) // 12 * 12]) if b)
    i = bisect.bisect_right([f[0] for f in funcs], rva) - 1
    begin, end = funcs[i]
    if not (begin <= rva < end):
        begin, end = rva, rva + 0x400
    base = pe.OPTIONAL_HEADER.ImageBase
    code = pe.get_data(begin, end - begin)
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
    rip = re.compile(r"\[rip ([+-]) 0x([0-9a-f]+)\]")
    print(f"; function 0x{begin:x}-0x{end:x}")
    for n, ins in enumerate(md.disasm(code, base + begin)):
        if n >= int(limit):
            break
        note = ""
        m = rip.search(ins.op_str)
        if m:
            tgt = ins.address + ins.size + int(m.group(2), 16) * (1 if m.group(1) == "+" else -1) - base
            try:
                raw = pe.get_data(tgt, 96)
                s = raw.split(b"\0")[0]
                if len(s) >= 4 and all(32 <= c < 127 for c in s):
                    note = f'   ; "{s.decode()[:90]}"'
                else:
                    note = f"   ; rva 0x{tgt:x}"
            except Exception:
                note = f"   ; rva 0x{tgt:x}"
        print(f"  {ins.address - base:08x}  {ins.mnemonic:<8}{ins.op_str}{note}")


if __name__ == "__main__":
    main(*sys.argv[1:])
