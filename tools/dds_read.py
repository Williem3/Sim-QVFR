"""Reads an uncompressed DDS file (as RenderDoc saves textures) into a float numpy array, height x width x channels.

Formats: DXGI R11G11B10_FLOAT, R16G16B16A16_FLOAT/TYPELESS, R32G32B32A32_FLOAT, R8G8B8A8_UNORM(_SRGB)/TYPELESS and
B8G8R8A8 (8-bit ones as 0-1, not linearised), legacy headers for those (FourCC 113, 116, RGBA masks). First mip and
slice only.

    python dds_read.py <file.dds>      prints the size, format and per-channel min / mean / max
    from dds_read import read_dds      (tools/ on sys.path) for other scripts
"""
import struct
import sys

import numpy as np

R11G11B10_FLOAT = 26
R16G16B16A16_TYPELESS, R16G16B16A16_FLOAT = 9, 10
R32G32B32A32_FLOAT = 2
R8G8B8A8 = (27, 28, 29)
B8G8R8A8 = (87, 90, 91)


def _small_float(bits, mantissa_bits):
    """Unsigned 5-bit-exponent floats (R11G11B10's channels) to float32."""
    e = (bits >> mantissa_bits) & 0x1F
    m = (bits & ((1 << mantissa_bits) - 1)).astype(np.float64) / (1 << mantissa_bits)
    v = np.where(e == 0, m * 2.0 ** -14, (1 + m) * np.exp2(e.astype(np.float64) - 15))
    return np.where(e == 31, np.inf, v).astype(np.float32)


def read_dds(path):
    with open(path, "rb") as f:
        data = f.read()
    if data[:4] != b"DDS ":
        raise ValueError(f"{path}: not a DDS file")
    height, width = struct.unpack_from("<II", data, 12)
    # DDS_PIXELFORMAT at 76: size, flags (80), FourCC (84), bit count (88), RGBA masks (92).
    pf_flags = struct.unpack_from("<I", data, 80)[0]
    fourcc = data[84:88]
    bitcount = struct.unpack_from("<I", data, 88)[0]
    masks = struct.unpack_from("<4I", data, 92)
    offset = 128
    fmt = None
    if fourcc == b"DX10":
        fmt = struct.unpack_from("<I", data, 128)[0]
        offset += 20
    elif fourcc == struct.pack("<I", 113):
        fmt = R16G16B16A16_FLOAT
    elif fourcc == struct.pack("<I", 116):
        fmt = R32G32B32A32_FLOAT
    elif bitcount == 32 and masks[0] == 0xFF:
        fmt = 28
    elif bitcount == 32 and masks[0] == 0xFF0000:
        fmt = 87
    raw = memoryview(data)[offset:]
    n = width * height
    if fmt == R11G11B10_FLOAT:
        w = np.frombuffer(raw, dtype="<u4", count=n).reshape(height, width)
        img = np.stack([_small_float(w, 6), _small_float(w >> 11, 6), _small_float(w >> 22, 5)], axis=-1)
    elif fmt in (R16G16B16A16_FLOAT, R16G16B16A16_TYPELESS):
        img = np.frombuffer(raw, dtype="<f2", count=n * 4).reshape(height, width, 4).astype(np.float32)
    elif fmt == R32G32B32A32_FLOAT:
        img = np.frombuffer(raw, dtype="<f4", count=n * 4).reshape(height, width, 4).copy()
    elif fmt in R8G8B8A8 or fmt in B8G8R8A8:
        img = np.frombuffer(raw, dtype=np.uint8, count=n * 4).reshape(height, width, 4).astype(np.float32) / 255
        if fmt in B8G8R8A8:
            img = img[..., [2, 1, 0, 3]]
    else:
        raise ValueError(f"{path}: format {fmt} (flags {pf_flags:#x}, fourcc {fourcc!r}) not handled")
    return img, fmt


if __name__ == "__main__":
    img, fmt = read_dds(sys.argv[1])
    print(f"{sys.argv[1]}: {img.shape[1]}x{img.shape[0]} format {fmt}")
    for c in range(img.shape[2]):
        ch = img[..., c]
        print(f"  channel {c}: min {ch.min():.5g} mean {ch.mean():.5g} max {ch.max():.5g}")
