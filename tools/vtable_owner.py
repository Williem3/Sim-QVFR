"""Find vtables containing the given function/thunk RVAs and name their class via MSVC RTTI.

Usage: python vtable_owner.py <exe> <rva_hex> [...]
A vtable slot holds base+rva. vtable[-1] points at the RTTI Complete Object Locator, whose
TypeDescriptor (rva at +0x0c) holds the decorated name ".?AV<name>@@".
"""
import struct
import sys

import pefile


def main(exe, rvas):
    pe = pefile.PE(exe, fast_load=True)
    base = pe.OPTIONAL_HEADER.ImageBase
    rdata = next(s for s in pe.sections if s.Name.startswith(b".rdata"))
    raw, rb = rdata.get_data(), rdata.VirtualAddress
    for r in rvas:
        target = struct.pack("<Q", base + int(r, 16))
        pos = raw.find(target)
        hits = 0
        while pos != -1 and hits < 6:
            if pos % 8 == 0:
                slot_rva = rb + pos
                # walk back to the vtable start: the qword before it is the COL pointer
                start = pos
                while start >= 8:
                    prev = struct.unpack_from("<Q", raw, start - 8)[0]
                    col_rva = prev - base
                    try:
                        col = pe.get_data(col_rva, 0x18)
                        sig, _, _, td_rva = struct.unpack_from("<IIII", col)
                        if sig == 1 and 0 < td_rva < pe.OPTIONAL_HEADER.SizeOfImage:
                            name = pe.get_data(td_rva + 0x10, 160).split(b"\0")[0].decode(errors="replace")
                            if name.startswith(".?A"):
                                print(f"0x{int(r, 16):x}: slot {(pos - start) // 8} of vtable 0x{rb + start:x} class {name}")
                                break
                    except Exception:
                        pass
                    start -= 8
                hits += 1
            pos = raw.find(target, pos + 1)
        if hits == 0:
            print(f"0x{int(r, 16):x}: not found in .rdata")


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2:])
