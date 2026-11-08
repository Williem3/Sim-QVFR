"""Check a game's abi.h anchors against its exe (or the decrypted image dump of a packed exe), before any test run.

Usage: python check_anchors.py <exe or image dump> <src/games/<id>/abi.h>
Pairs every `kName = 0x...` RVA with its `kNameAnchor[] = {...}` bytes and compares them with the file at that RVA.
Also checks `kNameSlot = <vtable rva> + <n> * 8` against `kName = 0x...` (the slot holds base + that function).
Prints each check; exits 1 if any fails. Anchors are the code the layer verifies at run time: a mismatch here is a
crash or a refused patch in the headset.
"""
import re
import struct
import sys

import pefile


def main(image, header):
    pe = pefile.PE(image, fast_load=True)
    base = pe.OPTIONAL_HEADER.ImageBase
    text = re.sub(r"//[^\n]*", "", open(header, encoding="utf-8").read())  # comments may hold hex too
    rvas = {}
    for m in re.finditer(r"constexpr\s+std::uintptr_t\s+(k\w+)\s*=\s*([^;]+);", text):
        expr = m.group(2).strip()
        if re.fullmatch(r"[0-9a-fA-Fx+*\s]+", expr):
            rvas[m.group(1)] = eval(expr, {}, {})  # numbers, + and * only (checked above)
    anchors = {}
    for m in re.finditer(r"constexpr\s+std::uint8_t\s+(k\w+)Anchor\[\]\s*=\s*\{([^}]*)\}", text):
        anchors[m.group(1)] = bytes(int(x, 16) for x in re.findall(r"0x[0-9a-fA-F]+", m.group(2)))
    failures = 0
    for name, want in anchors.items():
        if name not in rvas:
            print(f"?    {name}Anchor has no {name} RVA")
            failures += 1
            continue
        have = pe.get_data(rvas[name], len(want))
        ok = have == want
        failures += not ok
        print(f"{'ok ' if ok else 'BAD'}  {name} 0x{rvas[name]:x}: {want.hex()}" + ("" if ok else f" (file has {have.hex()})"))
    for name, slot in rvas.items():
        if not name.endswith("Slot"):
            continue
        fn = rvas.get(name[: -len("Slot")])
        if fn is None:
            continue
        (held,) = struct.unpack("<Q", pe.get_data(slot, 8))
        ok = held - base == fn
        failures += not ok
        print(f"{'ok ' if ok else 'BAD'}  {name} 0x{slot:x} holds 0x{held - base:x}, expected 0x{fn:x}")
    print(f"{len(anchors)} anchors, {failures} failure(s)")
    return 1 if failures else 0


if __name__ == "__main__":
    if len(sys.argv) != 3:
        print(__doc__)
        sys.exit(2)
    sys.exit(main(sys.argv[1], sys.argv[2]))
