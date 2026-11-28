"""Build the phone test assets into <repo>/data (copy that folder into Starfield\\Data).

  Textures\\PhoneTest\\PhoneScreenTest.dds   756x1332 R8G8B8A8_UNORM, 1 mip, magenta + grid.
                                             The size is unique: the plugin finds the texture by it.
  Materials\\PhoneTest\\PhoneScreenTest.mat  copy of vanilla Dataslatescreen_01.mat using that texture (lit; unused).
  Materials\\PhoneTest\\PhoneScreenEmissive.mat
                                             emissive screen material: ScreenSFSE's ColorEmissive screen material
                                             (read from the local "ScreenSFSE - Main.ba2") pointed at that texture.
  Meshes\\AnimObjects\\PhoneTest\\AnimObjectPhone_<variant>.nif
                                             vanilla AnimObjectDataSlate.nif; its screen faces use PhoneScreenEmissive.mat,
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

W, H = 756, 1332
DXGI_R8G8B8A8_UNORM = 28

MAT_REL = "Materials\\PhoneTest\\PhoneScreenTest.mat"
EMISSIVE_MAT_REL = "Materials\\PhoneTest\\PhoneScreenEmissive.mat"
SCREEN_MAT_REL = EMISSIVE_MAT_REL  # what the NIF's screen faces use
TEX_REL = "Textures\\PhoneTest\\PhoneScreenTest.dds"
NIF_DIR = "AnimObjects\\PhoneTest"

# name: (rotation degrees about x, y, z applied in that order, translation x, y, z in NIF units = metres)
# Round 1 (in game): only ry+90 turns the screen face toward the camera (ry-90 shows the back).
# Round 2: +0.1 x moved it up and right (behind the fist); -x and +y (0.1) left the view; y-/z were not seen.
# Round 3: small steps per axis looked like rotations (perspective + tilted bone), so placement was tuned live with
# the plugin's numpad tuner on top of r3_base and saved to tools/placement.txt.
# Round 4 (in game): the baked placement matched only when written TRANSPOSED: the game's in-memory NiMatrix3 is
# the transpose of the rotation stored in the NIF file. "v1" is that placement.
# The plugin draws N white squares on the screen for line N of PhoneTest.ini (1 = vanilla slate).
RY = (0, 90, 0)
VARIANTS = {
    "r3f_base": (RY, (0, 0, 0)),  # base the tuner offset is relative to (re-tune from here); emissive screen
}
ROOT_NAME = b"PhoneAnimObjectRoot"


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


def ba2_read(archive, name):
    """One file from a Starfield GNRL BA2 (v2/v3, zlib)."""
    import zlib
    with open(archive, "rb") as f:
        magic, ver, typ, cnt, nameoff = struct.unpack("<4sI4sIQ", f.read(24))
        if typ != b"GNRL":
            raise SystemExit(f"{archive}: not a GNRL archive")
        if ver >= 2:
            f.read(8)
        if ver == 3 and struct.unpack("<I", f.read(4))[0] != 0:
            raise SystemExit(f"{archive}: only zlib-compressed archives are supported")
        entries = [struct.unpack("<IIIIQIII", f.read(36)) for _ in range(cnt)]
        f.seek(nameoff)
        for e in entries:
            n = f.read(struct.unpack("<H", f.read(2))[0]).decode("utf-8")
            if n.lower() == name.lower():
                off, packed, size = e[4], e[5], e[6]
                f.seek(off)
                raw = f.read(packed or size)
                return zlib.decompress(raw) if packed else raw
    raise SystemExit(f"{archive}: {name} not found")


def make_emissive_mat(game):
    """ScreenSFSE's emissive screen material (ColorEmissive shader model, proven on its in-world screens) with our
    texture. Built from the user's own install, never committed (third-party asset)."""
    text = ba2_read(os.path.join(game, "Data", "ScreenSFSE - Main.ba2"),
                    "materials/screensfse/screensfse_smallch1.mat").decode("utf-8")
    esc = lambda p: p.replace("\\", "\\\\")
    text = text.replace(esc(r"Data\Textures\ScreenSFSE\SmallCh1.dds"), esc("Data\\" + TEX_REL))
    text = text.replace(esc(r"Materials\ScreenSFSE\ScreenSFSE_SmallCh1.mat"), esc(EMISSIVE_MAT_REL))
    text = text.replace("ScreenSFSE_SmallCh1", "PhoneScreenEmissive")
    # this material's own objects: new IDs so they can't clash with ScreenSFSE's
    text, n = re.subn(r"res:([0-9A-F]{8}):5A57A268:C017515A",
                      lambda m: "res:%08X:5A57A268:C017515A" % (int(m.group(1), 16) ^ 0x7E5A1100), text)
    if "PhoneScreenTest.dds" not in text or "ScreenSFSE" in text or n != 10:
        raise SystemExit(f"emissive material: unexpected template (ids {n})")
    print(f"emissive material: {n} IDs renamed")
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


def make_nif(src_nif, rot_deg, trans, post=None):
    out = open(src_nif, "rb").read()
    # header strings are <uint32 len><chars>; nothing points at their byte offsets, so lengths may change.
    # Screen face -> our material; sides, back and the generated-text part stay vanilla. (Up to v2 the TextGen part
    # used our material too: it showed a mirrored slice of the UI on the strip by the hinge.)
    new = SCREEN_MAT_REL.encode()
    for old in (b"Materials\\Items\\DataSlate\\Dataslatescreen_01.mat",):
        needle = struct.pack("<I", len(old)) + old
        if out.count(needle) != 1:
            raise SystemExit(f"nif: expected one header string {old!r}")
        out = out.replace(needle, struct.pack("<I", len(new)) + new)
    # Unique root name, so the plugin's live tuner finds OUR object and not an NPC's vanilla slate.
    old = b"AnimObjectDataSlate"
    needle = struct.pack("<I", len(old)) + old
    if out.count(needle) != 1:
        raise SystemExit("nif: expected one header string 'AnimObjectDataSlate'")
    out = out.replace(needle, struct.pack("<I", len(ROOT_NAME)) + ROOT_NAME)

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
        if post:
            # the numpad tuner's offset D, applied after the base: child' = D * child (in the game's memory
            # convention). If memory matrices are the transpose of the file's, the file rotation is rot * D^T.
            drot, dt, transposed = post
            if transposed:
                nr = [[sum(nr[i][k] * drot[j][k] for k in range(3)) for j in range(3)] for i in range(3)]
            else:
                nr = [[sum(drot[i][k] * nr[k][j] for k in range(3)) for j in range(3)] for i in range(3)]
            nt = [sum(drot[i][k] * nt[k] for k in range(3)) + dt[i] for i in range(3)]
        struct.pack_into("<3f", data, t_off, *nt)
        struct.pack_into("<9f", data, t_off + 12, *[v for row in nr for v in row])
        geoms += 1
    if geoms != 4:
        raise SystemExit(f"nif: expected 4 BSGeometry blocks, found {geoms}")
    return bytes(data)


def placement_post():
    """tools/placement.txt = the numpad tuner's saved offset D (copy of PhoneTest_offset.txt), tuned on top of
    r3_base (rotation RY, no translation). Returns (rot rows, trans) or None."""
    path = os.path.join(os.path.dirname(os.path.abspath(__file__)), "placement.txt")
    if not os.path.exists(path):
        return None
    vals = {}
    for line in open(path, encoding="utf-8"):
        parts = line.split()
        if parts and parts[0] in ("rot", "trans"):
            vals[parts[0]] = [float(v) for v in parts[1:]]
    d = vals["rot"]
    return [d[0:3], d[3:6], d[6:9]], vals["trans"]


def main():
    game, src_nif = sys.argv[1], sys.argv[2]
    root = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "data")
    write(os.path.join(root, TEX_REL), make_dds())
    write(os.path.join(root, MAT_REL), make_mat(game))
    write(os.path.join(root, EMISSIVE_MAT_REL), make_emissive_mat(game))
    ini = ["; PhoneTest model list (F11 cycles; re-read on every press). Generated by tools/make_test_assets.py",
           "model=AnimObjects\\AnimObjectDataSlate.nif"]
    jobs = []
    placed = placement_post()
    if placed:
        # tuned placement first (one F11 press reaches it); transposed = verified convention (round 4)
        # v3 = v1 placement, emissive screen, vanilla TextGen part (new name: the game caches NIFs by path)
        jobs.append(("v3", RY, (0, 0, 0), (placed[0], placed[1], True)))
    jobs += [(name, rot, trans, None) for name, (rot, trans) in VARIANTS.items()]
    for name, rot, trans, post in jobs:
        # file names change with the transform method: the game caches a loaded NIF by path for the session
        rel = f"{NIF_DIR}\\AnimObjectPhone_g_{name}.nif"
        write(os.path.join(root, "Meshes", rel), make_nif(src_nif, rot, trans, post))
        ini.append(f"model={rel}")
    # how the upright home screen maps onto the texture; change in game and re-open (F8, F7) to try values
    # screen_aspect = visible screen height / width (the vanilla slate screen is roughly square); tune until the
    # rounded app icons look square
    # v3: only the screen mesh shows the UI, and its UVs are rotated 180 degrees (v2 showed the TextGen layer)
    ini += ["screen_rotate=180", "screen_mirror=0", "screen_aspect=0.88"]
    # safe area: fractions of the upright screen hidden by the slate frame (v3: left and top are covered)
    ini += ["screen_inset_left=0.14", "screen_inset_top=0.06", "screen_inset_right=0.02", "screen_inset_bottom=0.02"]
    # dev_keys=1 enables F11 (cycle models) and the numpad placement tuner
    ini += ["dev_keys=0"]
    write(os.path.join(root, "SFSE", "Plugins", "PhoneTest.ini"), ("\n".join(ini) + "\n").encode())


if __name__ == "__main__":
    main()
