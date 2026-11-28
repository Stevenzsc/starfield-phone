# Second render view (camera viewfinder without the phone)

Goal: show the world on the phone screen from the player's eye without the 1st-person hand and phone.
Starfield.exe 1.16.244, addresses are absolute (base 0x140000000). Found with Ghidra (tools/re).

## Why a capture of the main frame cannot work

- 1st-person geometry is drawn into the same `DeferredGBuffer` as the world; `FirstPersonDepth` is only a
  separate depth pass. No pass produces "world only" lit colour.
- `Game.ShowFirstPersonGeometry(bool)` (Papyrus native `FUN_142009d70`) only sets a hidden flag on the
  1st-person 3D, for the whole frame.

## How CreationRenderer renders several views

The renderer keeps entity tables (ECS style). Handles are `index (24 bits) | generation << 24`.

- View table `DAT_145975168` (at most 64 rows). Columns are table pointers at +0xe8 .. +0x120:
  - create a view: `FUN_1407c9be0(uint* handle)` (handle must be 0xFFFFFF first); destroy: `FUN_1407c9d10`
  - +0x120 (0x1C bytes), set by `FUN_140841310(&view, data)`: { u32 scene handle (= scene object +0x150),
    u32 camera handle (= NiCamera +0x130), u32 0, float x3, u32 1 }
  - +0x108: feature flags (u64 bits) through `FUN_14298c1f0(view)`; console toggles use it
    (0x2 decals, 0x40 lens flare, 0x1000 TAA, 0x2000 motion blur, 0x8000 SAO, 0x80 unknown)
  - +0xf8: shared exposure state (`FUN_140841200`), +0xf0: 0x3C bytes (`FUN_140d77e70`)
- Output table `DAT_145975188` ("render graph output"):
  - create: `FUN_1407ca380(uint* handle)`; destroy: `FUN_1407ca4c0`
  - +0x128: { u8 pipeline type, BSFixedString name } via `FUN_140342ce0(&out, data)`
    (0x55 main world, 0x5A second main output, 0x56 FaceCustomization, 0x57 ResourceMapUpscaler)
  - +0x120: list of view handles via `FUN_142914b90(column, index, BSTArray<u32>)`
  - +0xf0: render target handle (row of the texture table `DAT_145975140`)
  - +0xe8: 0x30 bytes of resolution / format settings (`FUN_140841410`); +0x108: u8 (`FUN_140841500`)
- The frame root graph (`FUN_142b793e0` / `FUN_142b78aa0`) builds one `RenderViewSubGraph_[n]` per view
  in an output's view list.

## Existing users to copy from

- World setup `FUN_14083e9a0`: world holder `DAT_145fb08e8` (0x4F0 bytes): views at +0x3D0 / +0x3D4,
  outputs at +0x3D8 / +0x3DC, +0x4E0 picks the active view, +0x4E1 valid. Scene object at holder[0xE]
  (+0x70), "World Camera" NiCamera at holder[0x10] (+0x80). Output types set in `FUN_14083fc30`.
- Character creator face preview `FUN_140d8ead0` ("FaceCustomization"): view + output + view list.
- Starmap scanner `FUN_1403420f0` ("ResourceMapUpscaler", created by `FUN_14168a070`): view, output, its
  own render target row in `DAT_145975140`, view list, target bound to output +0xF0.

## Open questions

1. Camera: create an NiCamera (ctor `FUN_142be2be0`, 0x220 bytes; init `FUN_142be2e70`, `FUN_142be3080`,
   `FUN_142be3350`) under the world camera so it follows the eye, and check that +0x130 gets a handle.
2. Hiding the hand / phone in that view only: a near plane past the phone, or a per-view flag (unknown).
3. Render target: create a texture row like `FUN_1403420f0` does and find its ID3D12Resource for the copy
   into the phone screen texture.
4. Cost: a full 0x55 pipeline at phone resolution.

## In-game results (probe, 2026-11-28)

- Handles: world scene 01000000, env (+0x64) 01000000, main camera 01000001 / view 01000001,
  second camera 01000002 / view 01000002, outputs 01000003 (type 0x55, view list [01000001]) and
  01000004 (type 0x5A, empty).
- View binding row (0x1C): { u32 scene, u32 camera, u8 flags.., float FOV 85, float near 0.05, float far 6000, u32 }.
- Own camera + view through `FUN_142550860` works (camera handle 01000006, view 01000003, no crash), but putting the
  view into output 01000004 shows nothing: that output is only submitted when there is no main view.

## How outputs are submitted

- `FUN_140840180` (per frame): view list = [active main view] -> written into output 01000003 every frame ->
  `FUN_1429123a0(frame, holder+0x3E0)` appends the 0x80-byte output descriptor to the frame.
- Output descriptor (0x80): BSTSmallArray of { void* target, texture*, u32 slot hash } (inline 4 x 0x18), +0x70
  target object (0 for offscreen), +0x78 output handle. `FUN_142912210(desc, hash, &texture)` binds a texture to a
  slot; the main output uses slot 0x9681900, output 01000004 uses 0x97C1900, the scanner 0x971AF00.
- `FUN_140840B40(desc)` queues a descriptor (64-slot ring) for the renderer: the starmap scanner renders its
  texture this way. Calling it once per frame should render our own output.
- Render texture: `FUN_1428FC910(&BSFixedString name, &desc[0x48], &texture)`; scanner desc: +0 0x80, +4 width,
  +8 height, +0xC 1, +0x10 2, +0x14 1, +0x18 0x2401, +0x2C 0x101006, +0x30 0x05040302, +0x38 1. Ready when
  `(texture+0x14 & 0x70000000)` reaches the loaded state.
- `Game.ShowFirstPersonGeometry` = `FUN_142009D70` (ID 117377): the cull bit on NiAVObject +0x118 does not hide
  1st-person geometry from the renderer; this native does (player +0x112A bit 4).
