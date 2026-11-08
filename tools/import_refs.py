"""Find the code that uses imported functions, in a large x64 PE without symbols.

Usage: python import_refs.py <exe> <name substring> [<name substring> ...]
For each import whose name contains a substring (any case): its IAT slot, then every RIP-relative reference to the
slot in .text (`call [rip+x]`, `jmp [rip+x]`, `mov reg, [rip+x]`), with the function holding it (bounds from .pdata).
Works on the decrypted image dump of a packed exe too (its import table is the file's).
"""
import bisect
import struct
import sys

import pefile


def main(exe, wanted):
    pe = pefile.PE(exe, fast_load=True)
    pe.parse_data_directories(directories=[pefile.DIRECTORY_ENTRY["IMAGE_DIRECTORY_ENTRY_IMPORT"]])
    base = pe.OPTIONAL_HEADER.ImageBase
    slots = {}
    for entry in getattr(pe, "DIRECTORY_ENTRY_IMPORT", []):
        for imp in entry.imports:
            name = imp.name.decode(errors="replace") if imp.name else f"#{imp.ordinal}"
            if any(w.lower() in name.lower() for w in wanted):
                slots[imp.address - base] = f"{entry.dll.decode(errors='replace')}!{name}"
    if not slots:
        print("no matching imports")
        return
    text = next(s for s in pe.sections if s.Name.startswith(b".text"))
    code, tb = text.get_data(), text.VirtualAddress
    pdata = next(s for s in pe.sections if s.Name.startswith(b".pdata")).get_data()
    funcs = sorted((b, e) for b, e, _ in struct.iter_unpack("<III", pdata[: len(pdata) // 12 * 12]) if b)
    starts = [f[0] for f in funcs]

    def func_of(rva):
        i = bisect.bisect_right(starts, rva) - 1
        return funcs[i][0] if i >= 0 and funcs[i][0] <= rva < funcs[i][1] else None

    # Encodings ending in a disp32 that is RIP-relative: FF 15 (call), FF 25 (jmp), REX + 8B /r with mod=00 rm=101.
    hits = {slot: [] for slot in slots}
    for off in range(len(code) - 6):
        b0, b1 = code[off], code[off + 1]
        if b0 == 0xFF and b1 in (0x15, 0x25):
            ilen, kind = 6, "call" if b1 == 0x15 else "jmp"
            disp_at = off + 2
        elif b0 in (0x48, 0x4C) and b1 == 0x8B and (code[off + 2] & 0xC7) == 0x05:
            ilen, kind = 7, "mov"
            disp_at = off + 3
        else:
            continue
        (disp,) = struct.unpack_from("<i", code, disp_at)
        target = tb + off + ilen + disp
        if target in hits:
            rva = tb + off
            hits[target].append((rva, kind, func_of(rva)))
    # A `jmp [slot]` outside any function is an import thunk: the code calls the thunk with `call rel32` / `jmp rel32`.
    thunks = {rva: slot for slot, refs in hits.items() for rva, kind, f in refs if kind == "jmp" and f is None}
    via = {rva: [] for rva in thunks}
    for off in range(len(code) - 5):
        if code[off] in (0xE8, 0xE9):
            (rel,) = struct.unpack_from("<i", code, off + 1)
            target = tb + off + 5 + rel
            if target in via:
                via[target].append((tb + off, "call" if code[off] == 0xE8 else "jmp", func_of(tb + off)))
    for slot, name in sorted(slots.items(), key=lambda kv: kv[1]):
        refs = hits[slot]
        print(f"{name}: IAT 0x{slot:x}, {len(refs)} reference(s)")
        for rva, kind, f in refs:
            print(f"  0x{rva:x} {kind:4} in function 0x{f:x}" if f else f"  0x{rva:x} {kind:4} (no .pdata function)")
            for crva, ckind, cf in via.get(rva, []):
                print(f"    0x{crva:x} {ckind} to the thunk, in function 0x{cf:x}" if cf else
                      f"    0x{crva:x} {ckind} to the thunk (no .pdata function)")


if __name__ == "__main__":
    if len(sys.argv) < 3:
        print(__doc__)
        sys.exit(2)
    main(sys.argv[1], sys.argv[2:])
