"""Build the phone test assets into <repo>/data (copy that folder into Starfield\\Data).

  Textures\\PhoneTest\\PhoneScreenTest.dds   504x888 R8G8B8A8_UNORM, 1 mip, magenta + grid.
                                             The size is unique: the plugin finds the texture by it.
  Materials\\PhoneTest\\PhoneScreenTest.mat  copy of vanilla Dataslatescreen_01.mat using that texture.
  Meshes\\AnimObjects\\PhoneTest\\AnimObjectPhone_<variant>.nif
                                             vanilla AnimObjectDataSlate.nif; its screen faces use PhoneScreenTest.mat,
                                             sides and back stay vanilla; the root node gets the variant's rotation
                                             and translation (relative to the L_AnimObject1 bone).
  Data\\SFSE\\Plugins\\PhoneTest.ini       model list for the plugin's F11 key (one model= line per variant).

usage: py -I tools/make_test_assets.py <Starfield folder> <animobjectdataslate.nif extracted from Meshes01.ba2>
"""
import math
import os
import re
import struct
import sys

W, H = 504, 888
DXGI_R8G8B8A8_UNORM = 28

MAT_REL = "Materials\\PhoneTest\\PhoneScreenTest.mat"
TEX_REL = "Textures\\PhoneTest\\PhoneScreenTest.dds"
NIF_DIR = "AnimObjects\\PhoneTest"

# name: (rotation degrees about x, y, z applied in that order, translation x, y, z in NIF units)
VARIANTS = {
    "base": ((0, 0, 0), (0, 0, 0)),
    "rx+90": ((90, 0, 0), (0, 0, 0)),
    "rx-90": ((-90, 0, 0), (0, 0, 0)),
    "ry+90": ((0, 90, 0), (0, 0, 0)),
    "ry-90": ((0, -90, 0), (0, 0, 0)),
    "rz+90": ((0, 0, 90), (0, 0, 0)),
    "rz-90": ((0, 0, -90), (0, 0, 0)),
    "rx180": ((180, 0, 0), (0, 0, 0)),
}


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


def rotation(rx, ry, rz):
    """Row-major 3x3 for R = Rz * Ry * Rx (degrees)."""
    def m(a, b):
        return [[sum(a[i][k] * b[k][j] for k in range(3)) for j in range(3)] for i in range(3)]
    cx, sx = math.cos(math.radians(rx)), math.sin(math.radians(rx))
    cy, sy = math.cos(math.radians(ry)), math.sin(math.radians(ry))
    cz, sz = math.cos(math.radians(rz)), math.sin(math.radians(rz))
    Rx = [[1, 0, 0], [0, cx, -sx], [0, sx, cx]]
    Ry = [[cy, 0, sy], [0, 1, 0], [-sy, 0, cy]]
    Rz = [[cz, -sz, 0], [sz, cz, 0], [0, 0, 1]]
    return m(Rz, m(Ry, Rx))


def blocks(data):
    """[(type name, byte offset)] of every block. Header layout of Starfield NIFs (20.2.0.7, BS 172/173)."""
    num_blocks = struct.unpack_from("<I", data, data.index(b"\n") + 1 + 4 + 1 + 4)[0]
    types_at = data.index(b"\x06\x00\x00\x00NiNode") - 2  # uint16 block-type count precedes the first type name
    num_types = struct.unpack_from("<H", data, types_at)[0]
    p = types_at + 2
    names = []
    for _ in range(num_types):
        n = struct.unpack_from("<I", data, p)[0]
        names.append(data[p + 4:p + 4 + n].decode())
        p += 4 + n
    index = struct.unpack_from(f"<{num_blocks}H", data, p)
    p += 2 * num_blocks
    sizes = struct.unpack_from(f"<{num_blocks}I", data, p)
    # footer after the blocks: uint32 root count + one int32 ref per root
    num_roots, first_root = struct.unpack_from("<Ii", data, len(data) - 8)
    if num_roots != 1 or first_root != 0:
        raise SystemExit(f"nif: unexpected footer roots={num_roots} first={first_root}")
    off = len(data) - 8 - sum(sizes)
    out = []
    for i in range(num_blocks):
        out.append((names[index[i]], off))
        off += sizes[i]
    return out


def transform_offset(data, block_off):
    """Offset of the NiAVObject translation (3 floats), followed by rotation (9 floats) and scale."""
    n_extra = struct.unpack_from("<I", data, block_off + 4)[0]
    return block_off + 4 + 4 + 4 * n_extra + 4 + 4  # name, extra count, extra refs, controller, flags


def make_nif(src_nif, rot_deg, trans):
    out = open(src_nif, "rb").read()
    # header strings are <uint32 len><chars>; nothing points at their byte offsets, so lengths may change.
    # Front face (screen + generated text) -> our material; sides and back stay vanilla.
    new = MAT_REL.encode()
    for old in (b"Materials\\Items\\DataSlate\\Dataslatescreen_01.mat", b"Materials\\Items\\Dataslate\\DataSlate_TextGen01.mat"):
        needle = struct.pack("<I", len(old)) + old
        if out.count(needle) != 1:
            raise SystemExit(f"nif: expected one header string {old!r}")
        out = out.replace(needle, struct.pack("<I", len(new)) + new)

    # The engine overwrites the ROOT node's transform when it attaches an AnimObject (verified in game: rotating
    # the root changed nothing), so apply the variant to every geometry child instead:
    #   child' = V * child  ->  rot' = R * rot, trans' = R * trans + T
    data = bytearray(out)
    bl = blocks(data)
    if bl[0][0] != "NiNode":
        raise SystemExit(f"nif: block 0 is {bl[0][0]}, expected NiNode")
    R = rotation(*rot_deg)
    geoms = 0
    for name, off in bl:
        if name != "BSGeometry":
            continue
        t_off = transform_offset(data, off)
        t = struct.unpack_from("<3f", data, t_off)
        r = struct.unpack_from("<9f", data, t_off + 12)
        rm = [list(r[0:3]), list(r[3:6]), list(r[6:9])]
        nr = [[sum(R[i][k] * rm[k][j] for k in range(3)) for j in range(3)] for i in range(3)]
        nt = [sum(R[i][k] * t[k] for k in range(3)) + trans[i] for i in range(3)]
        struct.pack_into("<3f", data, t_off, *nt)
        struct.pack_into("<9f", data, t_off + 12, *[v for row in nr for v in row])
        geoms += 1
    if geoms != 4:
        raise SystemExit(f"nif: expected 4 BSGeometry blocks, found {geoms}")
    return bytes(data)


def main():
    game, src_nif = sys.argv[1], sys.argv[2]
    root = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "data")
    write(os.path.join(root, TEX_REL), make_dds())
    write(os.path.join(root, MAT_REL), make_mat(game))
    ini = ["; PhoneTest model list (F11 cycles; re-read on every press). Generated by tools/make_test_assets.py",
           "model=AnimObjects\\AnimObjectDataSlate.nif"]
    for name, (rot, trans) in VARIANTS.items():
        # file names change with the transform method: the game caches a loaded NIF by path for the session
        rel = f"{NIF_DIR}\\AnimObjectPhone_g_{name}.nif"
        write(os.path.join(root, "Meshes", rel), make_nif(src_nif, rot, trans))
        ini.append(f"model={rel}")
    write(os.path.join(root, "SFSE", "Plugins", "PhoneTest.ini"), ("\n".join(ini) + "\n").encode())


if __name__ == "__main__":
    main()
