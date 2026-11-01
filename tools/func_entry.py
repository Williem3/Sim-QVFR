"""Resolve an RVA to its primary function entry through x64 chained unwind info, and list callers.

Usage: python func_entry.py <exe> <rva_hex> [<rva_hex> ...]
MSVC splits functions into chunks whose .pdata entries carry UNW_FLAG_CHAININFO pointing at the
parent RUNTIME_FUNCTION. The primary entry is the only safe hook target. Also lists direct
`call rel32` sites to the entry, with the primary entry of each caller.
"""
import bisect
import re
import struct
import sys

import pefile

UNW_FLAG_CHAININFO = 0x4


class Image:
    def __init__(self, exe):
        self.pe = pefile.PE(exe, fast_load=True)
        raw = next(s for s in self.pe.sections if s.Name.startswith(b".pdata")).get_data()
        self.funcs = sorted(t for t in struct.iter_unpack("<III", raw[: len(raw) // 12 * 12]) if t[0])
        self.starts = [f[0] for f in self.funcs]

    def runtime_function(self, rva):
        i = bisect.bisect_right(self.starts, rva) - 1
        if i >= 0 and self.funcs[i][0] <= rva < self.funcs[i][1]:
            return self.funcs[i]
        return None

    def primary(self, rva):
        rf = self.runtime_function(rva)
        seen = 0
        while rf and seen < 16:
            begin, end, unwind = rf
            head = self.pe.get_data(unwind & ~1, 4)
            flags, count = head[0] >> 3, head[2]
            if not flags & UNW_FLAG_CHAININFO:
                return begin
            chained_at = (unwind & ~1) + 4 + ((count + 1) & ~1) * 2
            rf = struct.unpack("<III", self.pe.get_data(chained_at, 12))
            seen += 1
        return rf[0] if rf else None


def main(exe, rvas):
    img = Image(exe)
    text = next(s for s in img.pe.sections if s.Name.startswith(b".text"))
    code, tb = text.get_data(), text.VirtualAddress
    for r in rvas:
        rva = int(r, 16)
        entry = img.primary(rva)
        print(f"0x{rva:x} -> primary entry 0x{entry:x}" if entry else f"0x{rva:x} -> no .pdata entry")
        if not entry:
            continue
        # Incremental-link thunks: callers reach the entry through `jmp rel32` stubs outside .pdata.
        targets = {entry}
        for m in re.finditer(rb"\xe9", code):
            off = m.start()
            if tb + off + 5 + struct.unpack_from("<i", code, off + 1)[0] == entry and not img.runtime_function(tb + off):
                targets.add(tb + off)
                print(f"    thunk 0x{tb + off:x}")
        callers = set()
        for m in re.finditer(rb"\xe8", code):
            off = m.start()
            if tb + off + 5 + struct.unpack_from("<i", code, off + 1)[0] in targets:
                callers.add((tb + off, img.primary(tb + off)))
        for site, owner in sorted(callers):
            print(f"    called from 0x{site:x} in 0x{owner:x}" if owner else f"    called from 0x{site:x}")


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2:])
