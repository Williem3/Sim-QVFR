"""Label the direct callees of a function by the strings each callee references.

Usage: python label_callees.py <exe> <function_rva_hex> [max_strings_per_callee]
Follows incremental-link `jmp rel32` thunks to real entries, then lists string literals referenced
(RIP-relative LEA/MOV) anywhere in the callee's .pdata range, e.g. Optick/PIX zone names, log
text and source-path asserts. A callee with no strings is shown with its size only.
"""
import re
import struct
import sys

import capstone

from func_entry import Image

RIP_REF = re.compile(rb"[\x48\x4c][\x8d\x8b][\x05\x0d\x15\x1d\x25\x2d\x35\x3d]")


def follow_thunks(pe, rva):
    for _ in range(4):
        b = pe.get_data(rva, 5)
        if b[0] != 0xE9:
            break
        rva = rva + 5 + struct.unpack_from("<i", b, 1)[0]
    return rva


def strings_in(img, begin, end, limit):
    pe = img.pe
    code = pe.get_data(begin, end - begin)
    out = []
    for m in RIP_REF.finditer(code):
        off = m.start()
        target = begin + off + 7 + struct.unpack_from("<i", code, off + 3)[0]
        try:
            raw = pe.get_data(target, 120).split(b"\0")[0]
        except Exception:
            continue
        if len(raw) >= 5 and all(32 <= c < 127 for c in raw):
            s = raw.decode()
            if s not in out:
                out.append(s)
        if len(out) >= limit:
            break
    return out


def main(exe, rva_hex, limit="4"):
    img = Image(exe)
    pe = img.pe
    base = pe.OPTIONAL_HEADER.ImageBase
    entry = img.primary(int(rva_hex, 16))
    rf = img.runtime_function(entry)
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
    seen = set()
    for ins in md.disasm(pe.get_data(rf[0], rf[1] - rf[0]), base + rf[0]):
        if ins.mnemonic != "call" or not ins.op_str.startswith("0x"):
            continue
        target = follow_thunks(pe, int(ins.op_str, 16) - base)
        callee = img.primary(target) or target
        if callee in seen:
            continue
        seen.add(callee)
        crf = img.runtime_function(callee)
        size = (crf[1] - crf[0]) if crf else 0
        labels = strings_in(img, crf[0], crf[1], int(limit)) if crf else []
        print(f"0x{ins.address - base:x} -> 0x{callee:x} ({size} B) " + " | ".join(l[:70] for l in labels))


if __name__ == "__main__":
    main(*sys.argv[1:])
