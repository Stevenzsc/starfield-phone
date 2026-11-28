# P0 findings: first-person hand-held objects (Starfield 1.16.244)

## Data (Starfield.esm, Starfield - Animations.ba2)

| Device | Action (AACT) | Idle (IDLE, graph `Human_1stPersonRoot.agx`) | Graph event | AnimObject (ANIO) |
|---|---|---|---|---|
| Data slate | `ActionDataSlateOpen` 0001901F / `ActionDataSlateClose` 000222CD | `Human_1stPersonRoot_DataSlateOpen` 0026FCCA / `..._DataSlateClose` 0026FCC9 | `IdleDataSlateEquip` / `DataSlateClose` | `AnimObjectDataSlate` 0005AFB8 |
| Digipick | `ActionDigipickRaise` 003A15E3 / `ActionDigipickLower` 003A15E4 | `DigipickRaise` 003A1975 / `DigipickLower` 003A1976 | `IdleDigiPicEquip` / `DigiPicClose` | `AnimObjectDigiPic` 0002C24C |

- `meshes/actors/human/_1stperson/animations/common/equip_dataslate.afx` loads the object:
  `AnimObjLoad` (frame 0) and `AnimObjDraw` (frame 1), payload `AnimObjectDataSlate`.
- `human_1stpersonroot.agx` has a Switch node `DataSlate` that routes `IdleDataSlateEquip` and `IdleDigiPicEquip`.
- Default objects: `SecurityMenuInitiateAction_DO` (DFOB 003A15E2) and `SecurityMenuCloseAction_DO` (DFOB 003A1974) point at the digipick actions.

## Engine (Ghidra)

- SecurityMenu open (ID 92372, +0x1614DF0) reads the default object (form pointer at +0x48), checks `form+0x2E == 7` (AACT),
  builds a `BGSActionData` on the stack and calls the function pointer at ID 936442 (+0x5FAFE70, value = +0x19AA9A0, ID 101974).
  Lowering uses the same code in +0x161BC30.
- `BGSActionData` layout: +00 vtbl (ID 393262), +08 source actor (player, ID 922868 global), +10 target 0, +18 action form,
  +20 u32 0, +28 second vtbl (ID 393260), +30/+38 empty `BSFixedString`, +40..+60 zero.
- Console `player.playidle 26FCCA`, `player.sae IdleDataSlateEquip` and `player.playidle 3A1975` showed nothing in first person.

## In-game results (2026-11-27)

- F7 (`ActionDataSlateOpen` through PerformAction) shows the data slate in the left hand in 1st person; F8 puts it away.
- Swapping `AnimObjectDataSlate`'s model path at runtime works: the engine reads the path on every open.
  `animobjecttablet.nif` showed in the left hand, but only on the SECOND open: a model that is not loaded yet is
  missing on the first open (the `.afx` draws it at frame 1, before the async load finishes). Preload the phone
  model before opening.
- The 1st-person attach bone comes from the NIF's `Prn` string. `L_AnimObject1` works (data slate, tablet).
  Not checked in 1st person: `R_AnimObject1` (id card, tech reader) and `AnimObjectA` (digipick, Grendel).
- A truly separate ANIO needs its own `.afx` payload, so its own animation suffix: a new branch in
  `human_1stpersonroot.agx`.

## Screen texture writer (2026-11-27, verified in game)

- Assets from `tools/make_test_assets.py` (copy `data/` into `Starfield\Data`): a 756x1332 RGBA8 DDS, a material copied
  from the vanilla `Dataslatescreen_01.mat` (loose JSON; its own `res:F237F6xx` object IDs renamed), and the vanilla
  slate NIF with its four material strings pointed at it. Loose `.mat` and NIF files load with `bInvalidateOlderFiles=1`.
- The game creates the texture with `CreatePlacedResource`, format 27 (`R8G8B8A8_TYPELESS`), 1 mip, a few ms after the
  open action. It creates a NEW texture on every open (match #1, #2...); the writer follows the newest one.
- Hooks on the dummy device's vtable catch the game's device and queue (Streamline `sl.interposer.dll` present).
- With `ALLOW_SIMULTANEOUS_ACCESS` added at creation, a plain `CopyTextureRegion` before the game's command lists,
  15 times a second, needs no barriers: 332 writes, the pattern animates on the slate in the left hand, no crash.
- Open points: whole slate uses the screen material (only the test); textures stay AddRef'd until replaced;
  the material is lit, not emissive, so it is dark in shadow.

## Placement (2026-11-27, verified in game)

- The live numpad tuner (plugin/src/tuner.cpp) works once it uses the engine's own layout: GetObjectByName is vtable
  +0x1F0, NiNode children are at +0x138 (data) / +0x142 (uint16 count). CommonLibSF's NiNode::children and
  GetObjectByName slot are wrong for 1.16.244 (a scene dump through them crashed; the search found nothing).
- The 1st-person object is found from the top of the player's third-person 3D tree; it hangs on `L_AnimObject1`.
  Every open attaches a NEW object and the old one can stay under the bone, so the tuner follows the last child
  with the root name. The attach-call hook (ID 37953 +0x6B0) never fires for it.
- The engine's AnimObject attach (FUN_14034D400) zeroes only the root's local translation; children keep their
  NIF transforms in the bone's frame.
- In-memory NiMatrix3 is the TRANSPOSE of the rotation stored in the NIF file (a baked offset only matched when
  written transposed). The tuned placement is in tools/placement.txt and baked as AnimObjectPhone_g_v1.nif.

## Test plugin

`plugin/` builds `PhoneTest.dll`: F7/F8 slate open/close, F11 cycles the slate's model, log in
`Documents\My Games\Starfield\SFSE\Logs\PhoneTest.log`.

## Player, ship and location natives (phone apps: Character, Ship, Star map, Messages)

Found with `tools/re/ghidra/NativeFind.java` (LEA of the native name next to the registration call). All ignore
their VM / stack-ID arguments, so `stats.cpp` calls them directly with `nullptr, 0`.

| What | ID | Signature |
|---|---|---|
| ObjectReference.GetValue | 118329 | float (vm, stack, ref, AVIF*) -> ActorValueOwner at ref+0x70, slot 1 |
| ObjectReference.GetBaseValue | 118296 | float (vm, stack, ref, AVIF*) -> slot 3 |
| ObjectReference.GetCurrentPlanet | 118491 | PNDT* (vm, stack, ref) |
| ObjectReference.GetCurrentLocation | 63412 | thunk to TESObjectREFR::GetCurrentLocation (ref) |
| Actor.GetLevel | 116823 | u16 (vm, stack, actor): base NPC (+0x98) +0xE8; crashes if the base is null |
| Actor.GetGoldAmount | 116821 | int (vm, stack, actor): credits |
| Game.GetPlayerHomeSpaceShip | 102957 | FUN_141A6BD50 (player, ref** out); out holds a reference, release with ID 38742 |
| TESObjectREFR::GetSpaceship | 119881 | ref* (ref, true): ship the ref is aboard, no reference added |
| Game.GetXPForLevel | 69497 | float (level) |
| Debug.Notification | 117311 | void (vm, stack, unused, BSFixedString*) (not used yet) |

AVIF IDs: Health 2D4, Oxygen 2D5, CarryWeight 2DC, Experience 2C9, ShieldHealth 24A05F, ShieldMaxHealth 5BFA8,
SpaceshipGravJumpFuel 854F, SpaceshipGravJumpMaxDistance 8551, SpaceshipMaxAvailablePower 1018,
SpaceshipCrew 40CE0, SpaceshipPassengerSlots 1E7DE. No AV holds the current inventory weight.

Activities (QTYP QuestTypeActivities 475F8) are pointer quests whose FULL is a developer placeholder
("FFNeonZ01 wrapper", Chinese "霓虹城Z01的封装"); the phone lists them by objective like the mission menu.
