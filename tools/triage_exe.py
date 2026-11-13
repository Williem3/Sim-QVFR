"""First look at a game for QVFR: what it renders with, how it reaches VR, what stands in the way.

Usage: python triage_exe.py <game exe> [--steam <Steam folder>] [--strings]
Prints a Markdown report for the game's recon notes (docs/recon-<id>.md, "## Triage"):
- the build: size, SHA-256 (GameInfo::builds), PE timestamp, file version;
- the code: sections with entropy (a packed or encrypted .text shows no code to static analysis), .pdata function
  count, RTTI names;
- graphics and VR APIs, from imports and from DLL names the exe loads by string (d3d11/d3d12/vulkan, openvr_api,
  a statically linked OpenXR loader, LibOVR), and the OpenXR extensions it names (quad views, eye gaze, ...);
- anti-cheat (EasyAntiCheat / EOS anti-cheat, BattlEye) and Steam DRM;
- the engine, from source paths and well-known strings;
- upscalers shipped in the game folder (DLSS, FSR, XeSS) with their versions;
- Steam's launch options for the app (appinfo.vdf), which name the VR switches;
- with --strings: the rendering features the exe names (the screen-space effects QVFR must check first).
"""
import argparse
import collections
import hashlib
import math
import os
import re
import struct
import sys

import pefile

GRAPHICS = {
    "d3d11.dll": "D3D11", "d3d12.dll": "D3D12", "dxgi.dll": "DXGI", "vulkan-1.dll": "Vulkan",
    "opengl32.dll": "OpenGL", "d3d9.dll": "D3D9 (often only D3DPERF markers)",
}
VR = {
    "openvr_api.dll": "OpenVR (needs OpenComposite to reach OpenXR)",
    "openxr_loader.dll": "OpenXR (Khronos loader DLL)",
    "libovrrt64_1.dll": "Oculus LibOVR", "libovr.dll": "Oculus LibOVR",
}
XR_EXTENSIONS_OF_NOTE = {
    "XR_VARJO_quad_views": "quad views (runtime-composed QVFR)",
    "XR_VARJO_foveated_rendering": "Varjo foveated rendering",
    "XR_EXT_eye_gaze_interaction": "eye gaze (the game reads gaze itself)",
    "XR_FB_eye_tracking_social": "Meta eye tracking",
    "XR_KHR_visibility_mask": "visibility (hidden-area) mask",
    "XR_KHR_D3D11_enable": "D3D11 binding", "XR_KHR_D3D12_enable": "D3D12 binding",
    "XR_KHR_vulkan_enable": "Vulkan binding", "XR_KHR_vulkan_enable2": "Vulkan binding (2)",
    "XR_KHR_composition_layer_depth": "depth submission", "XR_FB_foveation": "FB foveation",
    "XR_MSFT_secondary_view_configuration": "secondary views",
}
ENGINES = [
    (rb"(?i)UnrealEngine|FEngineLoop|\+\+UE[45]\+", "Unreal Engine"),
    (rb"UnityPlayer|UnityEngine", "Unity"),
    (rb"ISIMotor|gMotor", "ISI gMotor (rFactor 2 family)"),
    (rb"(?i)CryEngine|CrySystem", "CryEngine"),
    (rb"(?i)\bstingray\b", "Stingray"),
    (rb"(?i)Frostbite", "Frostbite"),
    (rb"(?i)\bGodot\b", "Godot"),
]
# Rendering features worth a QVFR check: anything that works in screen space, on whole targets, or picks detail by
# the view's pixel density (the artefact catalogue in the integration playbook).
FEATURES = re.compile(
    rb"(?i)(ssao|hbao|gtao|cacao|ambient.?occlusion|ssr\b|screen.?space.?reflect|bloom|lens.?flare|auto.?exposure|"
    rb"eye.?adapt|histogram|tone.?map|vignette|depth.?of.?field|\bdof\b|motion.?blur|taa\b|temporal.?aa|"
    rb"dlss|fsr\b|fidelityfx|xess|lod.?bias|mip.?bias|anisotrop|shadow.?cascade|cascade|hidden.?area|"
    rb"visibility.?mask|stereo|single.?pass|instanc.*stereo|multiview|eye.?texture|left.?eye|right.?eye|"
    rb"render.?scale|resolution.?scale|supersampl|msaa|foveat|vrs\b|variable.?rate)")


def entropy(data):
    if not data:
        return 0.0
    counts = collections.Counter(data)
    n = len(data)
    return -sum(c / n * math.log2(c / n) for c in counts.values())


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def file_version(pe):
    try:
        pe.parse_data_directories(directories=[pefile.DIRECTORY_ENTRY["IMAGE_DIRECTORY_ENTRY_RESOURCE"]])
        for fi in getattr(pe, "FileInfo", []) or []:
            for entry in fi:
                for st in getattr(entry, "StringTable", []) or []:
                    vals = {k.decode(errors="replace"): v.decode(errors="replace") for k, v in st.entries.items()}
                    return vals.get("FileVersion") or vals.get("ProductVersion")
    except Exception:
        pass
    return None


def ascii_strings(blob, minimum=5):
    return (m.group() for m in re.finditer(rb"[\x20-\x7e]{%d,}" % minimum, blob))


def dll_names_in(strings):
    names = set()
    for s in strings:
        for m in re.finditer(rb"(?i)([a-z0-9_\-.]+\.dll)\b", s):
            names.add(m.group(1).decode().lower())
    return names


# ---- Steam appinfo.vdf (binary KeyValues, versions 0x27/0x28/0x29) ----

def read_appinfo(path, app_id):
    data = open(path, "rb").read()
    magic, _universe = struct.unpack_from("<II", data, 0)
    version = magic & 0xFF
    pos = 8
    table = None
    if version >= 0x29:
        (table_off,) = struct.unpack_from("<q", data, pos)
        pos += 8
        count = struct.unpack_from("<I", data, table_off)[0]
        table, p = [], table_off + 4
        for _ in range(count):
            end = data.index(b"\0", p)
            table.append(data[p:end].decode("utf-8", "replace"))
            p = end + 1
    while pos + 8 <= len(data):
        appid, size = struct.unpack_from("<II", data, pos)
        if appid == 0:
            break
        body = pos + 8
        if appid == app_id:
            # info_state, last_updated, pics token, sha1, change number, binary sha1 (v28+)
            kv_start = body + 4 + 4 + 8 + 20 + 4 + (20 if version >= 0x28 else 0)
            kv, _ = parse_kv(data, kv_start, table)
            return kv
        pos = body + size
    return None


def parse_kv(data, pos, table):
    out = {}
    while True:
        t = data[pos]
        pos += 1
        if t == 0x08:
            return out, pos
        if table is not None:
            (idx,) = struct.unpack_from("<I", data, pos)
            pos += 4
            key = table[idx]
        else:
            end = data.index(b"\0", pos)
            key = data[pos:end].decode("utf-8", "replace")
            pos = end + 1
        if t == 0x00:
            out[key], pos = parse_kv(data, pos, table)
        elif t == 0x01:
            end = data.index(b"\0", pos)
            out[key] = data[pos:end].decode("utf-8", "replace")
            pos = end + 1
        elif t in (0x02, 0x03, 0x04, 0x06):
            out[key] = struct.unpack_from("<i" if t == 0x02 else "<f" if t == 0x03 else "<I", data, pos)[0]
            pos += 4
        elif t in (0x07, 0x0A):
            out[key] = struct.unpack_from("<Q", data, pos)[0]
            pos += 8
        elif t == 0x05:
            end = data.index(b"\0\0", pos)
            out[key] = data[pos:end + 1].decode("utf-16-le", "replace")
            pos = end + 2
        else:
            raise ValueError(f"KeyValues type 0x{t:02x} at {pos}")


def find_steam_app_id(game_dir):
    p = os.path.join(game_dir, "steam_appid.txt")
    if os.path.isfile(p):
        txt = open(p, encoding="ascii", errors="ignore").read().strip()
        if txt.isdigit():
            return int(txt)
    # steamapps/appmanifest_<id>.acf whose installdir is this folder
    lib = os.path.dirname(os.path.dirname(os.path.abspath(game_dir)))
    folder = os.path.basename(os.path.abspath(game_dir)).lower()
    for name in os.listdir(lib) if os.path.isdir(lib) else []:
        if name.startswith("appmanifest_") and name.endswith(".acf"):
            txt = open(os.path.join(lib, name), encoding="utf-8", errors="ignore").read()
            m = re.search(r'"installdir"\s+"([^"]+)"', txt)
            if m and m.group(1).lower() == folder:
                return int(name[len("appmanifest_"):-4])
    return None


def dll_version(path):
    try:
        pe = pefile.PE(path, fast_load=True)
        return file_version(pe) or "?"
    except Exception:
        return "?"


def game_root(exe):
    """The Steam folder the exe sits in (steamapps/common/<root>/...), else the exe's folder."""
    parts = os.path.abspath(exe).split(os.sep)
    low = [p.lower() for p in parts]
    if "common" in low:
        i = low.index("common")
        if i + 1 < len(parts) - 1:
            return os.sep.join(parts[: i + 2])
    return os.path.dirname(os.path.abspath(exe))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("exe")
    ap.add_argument("--steam", default=r"C:\Program Files (x86)\Steam")
    ap.add_argument("--strings", action="store_true", help="list the rendering features the exe names")
    args = ap.parse_args()
    exe = args.exe
    root = game_root(exe)
    pe = pefile.PE(exe, fast_load=True)
    pe.parse_data_directories(directories=[pefile.DIRECTORY_ENTRY["IMAGE_DIRECTORY_ENTRY_IMPORT"]])

    print(f"## Triage: {os.path.basename(exe)}\n")
    print("### The build\n")
    print(f"- Path: `{os.path.abspath(exe)}`")
    print(f"- Size: {os.path.getsize(exe):,} bytes; SHA-256 `{sha256(exe)}`")
    print(f"- PE timestamp: 0x{pe.FILE_HEADER.TimeDateStamp:08x}; image base 0x{pe.OPTIONAL_HEADER.ImageBase:x}; "
          f"file version {file_version(pe) or '-'}")

    print("\n### The code\n")
    print("| Section | RVA | Size | Entropy |")
    print("| --- | --- | --- | --- |")
    packed = []
    for s in pe.sections:
        name = s.Name.rstrip(b"\0").decode(errors="replace")
        data = s.get_data()
        e = entropy(data[: 8 << 20])
        print(f"| {name} | 0x{s.VirtualAddress:x} | 0x{s.Misc_VirtualSize:x} | {e:.2f} |")
        if s.Characteristics & 0x20000000 and e > 7.8 and name != ".bind":  # executable, near random; not the stub
            packed.append(name)
    names = {s.Name.rstrip(b"\0") for s in pe.sections}
    if b".bind" in names:
        print("\n- **Steam DRM (SteamStub)**: a `.bind` section holds the stub (the entry point is in it).")
    if packed:
        print(f"- **Encrypted or packed code** in {', '.join(packed)} (entropy near 8): static analysis of the file shows "
              "no code.")
    else:
        print("- The code section reads as plain code: the exe can go into Ghidra as it is.")
    pdata = next((s for s in pe.sections if s.Name.startswith(b".pdata")), None)
    if pdata:
        print(f"- .pdata: {pdata.SizeOfRawData // 12:,} function entries (x64 unwind table; function bounds without symbols)")

    blobs = [s.get_data() for s in pe.sections if s.Name.startswith((b".rdata", b".data"))]
    strings = [s for b in blobs for s in ascii_strings(b)]
    rtti = sum(1 for s in strings if s.startswith(b".?AV") or s.startswith(b".?AU"))
    print(f"- RTTI type names: {rtti:,}" + (" (class names survive: vtables can be named)" if rtti > 100 else ""))

    imports = collections.OrderedDict()
    for entry in getattr(pe, "DIRECTORY_ENTRY_IMPORT", []):
        dll = entry.dll.decode(errors="replace").lower()
        imports[dll] = [i.name.decode(errors="replace") if i.name else f"#{i.ordinal}" for i in entry.imports]
    loaded = dll_names_in(strings)

    print("\n### Graphics and VR\n")
    for dll, what in {**GRAPHICS, **VR}.items():
        how = "imported" if dll in imports else "named in a string (loaded at run time)" if dll in loaded else None
        if how:
            print(f"- {what}: `{dll}` {how}")
    xr_api = [s for s in strings if re.fullmatch(rb"xr[A-Z][A-Za-z0-9]+", s)]
    static_loader = any(s in (b"xrCreateApiLayerInstance", b"OpenXR-Loader") for s in strings) or any(
        b"Khronos\\OpenXR" in s for s in strings)
    if static_loader and "openxr_loader.dll" not in imports:
        print("- **OpenXR loader linked into the exe** (Khronos loader strings): API layers load from "
              "XR_ENABLE_API_LAYERS / XR_API_LAYER_PATH as with the DLL loader; elevated, it reads HKLM only.")
    if xr_api:
        print(f"- OpenXR functions named: {len(set(xr_api))}")
    loader_env = {"XR_API_LAYER_PATH", "XR_ENABLE_API_LAYERS", "XR_LOADER_DEBUG", "XR_RUNTIME_JSON"}
    exts = sorted({s.decode() for s in strings if re.fullmatch(rb"XR_[A-Z0-9]+_[A-Za-z0-9_]+", s)} - loader_env)
    if exts:
        print("- OpenXR extensions named:")
        for e in exts:
            note = XR_EXTENSIONS_OF_NOTE.get(e)
            print(f"  - `{e}`" + (f": {note}" if note else ""))
    for vr_name in ("openvr_api.dll",):
        if vr_name in imports:
            ifaces = sorted({s.decode() for s in strings if re.fullmatch(rb"IVR[A-Za-z]+_\d{3}", s)})
            if ifaces:
                print(f"- OpenVR interfaces: {', '.join(ifaces)}")
    d3dx = [n for d, fns in imports.items() if d.startswith("d3dx") for n in fns if "Perspective" in n or "LookAt" in n]
    if d3dx:
        print(f"- Camera math imported from D3DX: {', '.join(sorted(set(d3dx)))} (cross-reference these for the eye "
              "projections)")

    print("\n### Anti-cheat and DRM\n")
    ac = []
    if os.path.isdir(os.path.join(root, "EasyAntiCheat")) or any("anticheat" in n.lower() for d in imports.values()
                                                                    for n in d):
        ac.append("EasyAntiCheat (EOS anti-cheat)")
    if os.path.isdir(os.path.join(root, "BattlEye")) or any("battleye" in d for d in loaded):
        ac.append("BattlEye")
    if os.path.isfile(os.path.join(root, "start_protected_game.exe")):
        ac.append("`start_protected_game.exe` bootstrapper (Steam starts the game protected)")
    if ac:
        print("- **" + "; ".join(ac) + "**. Sim-QVFR patches game code, so it must only run in a launch where the "
              "anti-cheat isn't running (the game exe started directly, offline sessions). Never in a protected "
              "launch; never work around the anti-cheat.")
    else:
        print("- No anti-cheat found.")

    print("\n### Engine\n")
    blob_all = b"\n".join(strings)
    found = [name for pat, name in ENGINES if re.search(pat, blob_all)]
    print(f"- Fingerprint: {', '.join(found) if found else 'unknown'}")
    paths = collections.Counter()
    for s in strings:
        m = re.match(rb"([A-Za-z]:\\(?:[^\\]+\\){2,6})", s)
        if m and (s.endswith(b".cpp") or s.endswith(b".h") or b".cpp" in s):
            paths[m.group(1).decode(errors="replace")] += 1
    if paths:
        print("- Source paths in the exe (the code's own layout):")
        for p, n in paths.most_common(8):
            print(f"  - `{p}` ({n})")

    print("\n### Upscalers in the game folder\n")
    ups = []
    for dirpath, _dirs, files in os.walk(root):
        for f in files:
            fl = f.lower()
            if fl.startswith(("nvngx_dlss", "amd_fidelityfx", "ffx_", "libxess", "sl.")) and fl.endswith(".dll"):
                ups.append((os.path.relpath(os.path.join(dirpath, f), root), dll_version(os.path.join(dirpath, f))))
        if dirpath.count(os.sep) - root.count(os.sep) > 5:
            _dirs[:] = []
    for rel, ver in ups:
        print(f"- `{rel}` {ver}")
    if not ups:
        print("- None (no DLSS: Foveated DLSS doesn't apply)")

    print("\n### Steam launch options\n")
    app_id = find_steam_app_id(root)
    appinfo = os.path.join(args.steam, "appcache", "appinfo.vdf")
    if app_id and os.path.isfile(appinfo):
        try:
            kv = read_appinfo(appinfo, app_id)
        except Exception as e:  # format change: say so rather than guess
            kv = None
            print(f"- appinfo.vdf unreadable ({e})")
        launch = ((kv or {}).get("appinfo", kv or {}).get("config", {}) or {}).get("launch", {})
        print(f"- App id {app_id}")
        for key, opt in sorted(launch.items(), key=lambda kv_: int(kv_[0]) if kv_[0].isdigit() else 0):
            desc = opt.get("description", "")
            print(f"  - `{opt.get('executable', '?')} {opt.get('arguments', '')}`".rstrip() +
                  f" type={opt.get('type', '-')}" + (f" ({desc})" if desc else ""))
    else:
        print("- No Steam app id found")

    if args.strings:
        print("\n### Rendering features the exe names\n")
        feats = collections.OrderedDict()
        for s in strings:
            if len(s) <= 80 and FEATURES.search(s):
                feats.setdefault(s.decode(errors="replace"), None)
        for f in list(feats)[:400]:
            print(f"- `{f}`")


if __name__ == "__main__":
    sys.stdout.reconfigure(encoding="utf-8")
    main()
