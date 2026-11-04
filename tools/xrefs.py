"""Find code references to strings in a large x64 PE (no symbols) and the functions that hold them.

Usage: python xrefs.py <exe> <out.tsv> <regex> [<regex> ...]
Scans .text for RIP-relative LEA/MOV operands (7-byte encodings) whose target is the start of a
string matching any regex. Strings are ASCII or UTF-16LE (Unreal's TEXT(); shown as u"..."). Function
bounds come from .pdata (x64 unwind table).
Output: string_rva, string, ref_rva, function_start, function_end.
"""
import bisect
import re
import struct
import sys

import pefile

LEA_MOV = re.compile(rb"[\x48\x4c][\x8d\x8b][\x05\x0d\x15\x1d\x25\x2d\x35\x3d]", re.S)


def main(exe, out, patterns):
    pe = pefile.PE(exe, fast_load=True)
    data = pe.__data__
    text = next(s for s in pe.sections if s.Name.startswith(b".text"))
    rdata = [s for s in pe.sections if s.Name.startswith((b".rdata", b".data"))]

    wanted = {}
    regexes = [re.compile(p.encode()) for p in patterns]
    for s in rdata:
        raw = s.get_data()
        for m in re.finditer(rb"[\x20-\x7e]{4,}\x00", raw):
            txt = m.group()[:-1]
            if any(r.search(txt) for r in regexes):
                wanted[s.VirtualAddress + m.start()] = txt.decode()
        for m in re.finditer(rb"(?:[\x20-\x7e]\x00){4,}\x00\x00", raw):
            if m.start() % 2:
                continue
            txt = m.group()[:-2].decode("utf-16-le").encode()
            if any(r.search(txt) for r in regexes):
                wanted[s.VirtualAddress + m.start()] = 'u"' + txt.decode() + '"'

    funcs = []
    pdata = next((s for s in pe.sections if s.Name.startswith(b".pdata")), None)
    if pdata:
        raw = pdata.get_data()
        for i in range(0, len(raw) - 11, 12):
            begin, end, _ = struct.unpack_from("<III", raw, i)
            if begin:
                funcs.append((begin, end))
    funcs.sort()
    starts = [f[0] for f in funcs]

    code = text.get_data()
    base = text.VirtualAddress
    rows = []
    for m in LEA_MOV.finditer(code):
        off = m.start()
        disp = struct.unpack_from("<i", code, off + 3)[0]
        rva = base + off + 7 + disp
        if rva in wanted:
            i = bisect.bisect_right(starts, base + off) - 1
            fs, fe = funcs[i] if i >= 0 and funcs[i][0] <= base + off < funcs[i][1] else (0, 0)
            rows.append((rva, wanted[rva], base + off, fs, fe))
    with open(out, "w", encoding="utf-8") as f:
        f.write("string_rva\tstring\tref_rva\tfunc_start\tfunc_end\n")
        for rva, s, ref, fs, fe in sorted(rows, key=lambda r: (r[3], r[2])):
            f.write(f"0x{rva:x}\t{s[:160]}\t0x{ref:x}\t0x{fs:x}\t0x{fe:x}\n")
    print(f"{len(wanted)} strings matched, {len(rows)} references, {len(funcs)} functions in .pdata")


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2], sys.argv[3:])
