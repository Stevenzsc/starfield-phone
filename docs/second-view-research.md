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
