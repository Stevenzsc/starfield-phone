"""Build the P0 screen-texture test assets into <repo>/data (copy that folder into Starfield\\Data).

  Textures\\PhoneTest\\PhoneScreenTest.dds   504x888 R8G8B8A8_UNORM, 1 mip, magenta + grid.
                                             The size is unique: the plugin finds the texture by it.
  Materials\\PhoneTest\\PhoneScreenTest.mat  copy of vanilla Dataslatescreen_01.mat using that texture.
  Meshes\\AnimObjects\\PhoneTest\\AnimObjectPhoneTest.nif
                                             vanilla AnimObjectDataSlate.nif with all four materials
                                             pointed at PhoneScreenTest.mat.

usage: py -I tools/make_test_assets.py <Starfield folder> <animobjectdataslate.nif extracted from Meshes01.ba2>
"""
import os
import re
import struct
import sys

W, H = 504, 888
DXGI_R8G8B8A8_UNORM = 28

MAT_REL = "Materials\\PhoneTest\\PhoneScreenTest.mat"
TEX_REL = "Textures\\PhoneTest\\PhoneScreenTest.dds"
NIF_REL = "Meshes\\AnimObjects\\PhoneTest\\AnimObjectPhoneTest.nif"


def write(path, data):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb") as f:
        f.write(data)
    print(f"wrote {path} ({len(data)} bytes)")


def make_dds():
    # DDS_HEADER (124 bytes) + DDS_HEADER_DXT10 (20 bytes)
    flags = 0x1 | 0x2 | 0x4 | 0x1000 | 0x8  # caps, height, width, pixelformat, pitch
    pitch = W * 4
    pf = struct.pack("<II4sIIIII", 32, 0x4, b"DX10", 0, 0, 0, 0, 0)
    hdr = struct.pack("<IIIIIII", 124, flags, H, W, pitch, 0, 1) + b"\0" * 44 + pf
    hdr += struct.pack("<IIIII", 0x1000, 0, 0, 0, 0)
    dx10 = struct.pack("<IIIII", DXGI_R8G8B8A8_UNORM, 3, 0, 1, 0)  # TEXTURE2D, arraySize 1
    px = bytearray(W * H * 4)
    for y in range(H):
        for x in range(W):
            i = (y * W + x) * 4
            grid = (x % 63 == 0) or (y % 63 == 0)
            px[i:i + 4] = bytes((255, 255, 255, 255)) if grid else bytes((255, 0, 255, 255))
    return b"DDS " + hdr + dx10 + bytes(px)


def make_mat(game):
    src = os.path.join(game, "Data", "Materials", "Items", "DataSlate", "Dataslatescreen_01.mat")
    text = open(src, encoding="utf-8").read()
    text = text.replace("MATERIALS\\\\Items\\\\Dataslate\\\\Dataslatescreen_01.mat", MAT_REL.replace("\\", "\\\\"))
    text = text.replace("Data\\\\Textures\\\\Items\\\\DataSlate\\\\DataSlateScreen_color.DDS", ("Data\\" + TEX_REL).replace("\\", "\\\\"))
    text = text.replace("Dataslatescreen_01", "PhoneScreenTest")
    # this material's own objects all use the res:F237F6xx:... IDs; give them a new, unique prefix
    text, n = re.subn(r"res:F237F6([0-9A-F]{2}):0005C4D1:A2C9E9B0", r"res:7E5A11\1:0005C4D1:A2C9E9B0", text)
    print(f"material: {n} object IDs renamed")
    assert "PhoneScreenTest.dds" in text and "PhoneTest\\\\PhoneScreenTest.mat" in text
    return text.encode("utf-8")


def make_nif(src_nif):
    data = bytearray(open(src_nif, "rb").read())
    # header strings are <uint32 len><chars>; nothing points at their byte offsets, so lengths may change
    olds = [
        b"Materials\\Items\\DataSlate\\Dataslatescreen_01.mat",
        b"Materials\\Items\\DataSlate\\Dataslatesides_01.mat",
        b"Materials\\Items\\DataSlate\\Dataslateback_01.mat",
        b"Materials\\Items\\Dataslate\\DataSlate_TextGen01.mat",
    ]
    new = MAT_REL.encode()
    out = bytes(data)
    for old in olds:
        needle = struct.pack("<I", len(old)) + old
        count = out.count(needle)
        if count != 1:
            raise SystemExit(f"nif: expected one header string {old!r}, found {count}")
        out = out.replace(needle, struct.pack("<I", len(new)) + new)
    return out


def main():
    game, src_nif = sys.argv[1], sys.argv[2]
    root = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "data")
    write(os.path.join(root, TEX_REL), make_dds())
    write(os.path.join(root, MAT_REL), make_mat(game))
    write(os.path.join(root, NIF_REL), make_nif(src_nif))


if __name__ == "__main__":
    main()
