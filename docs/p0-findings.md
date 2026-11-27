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

## Test plugin

`plugin/` builds `PhoneTest.dll`: F7/F8 slate open/close, F9/F10 digipick raise/lower, log in
`Documents\My Games\Starfield\SFSE\Logs\PhoneTest.log`.
