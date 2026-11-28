# starfield-phone

Hand-held phone mod for Starfield 1.16.244 (Pip-Boy style): an SFSE plugin (`plugin/`, CommonLibSF + xmake) that
shows a phone AnimObject in the first-person left hand, renders a CPU-drawn UI into its screen texture through
D3D12 hooks, and has Settings, Camera and Quests apps.

## Read first

Working notes from earlier sessions live in the Claude Code memory of the game folder:

`%USERPROFILE%\.claude\projects\C--Program-Files--x86--Steam-steamapps-common-Starfield\memory\`

Start with `MEMORY.md` there, then:
- `phone-mod-project.md`: build/deploy/test loop, ini keys, status, next steps
- `starfield-engine-layouts.md`: verified engine offsets and Address Library IDs (AnimObjects, scene graph,
  quests, forms map, model preload, screen texture)
- `commonlibsf-pitfalls.md`: CommonLibSF parts that are wrong or zero for 1.16.244 (some hang the game)
- `starfield-modding-workflow.md`: game quirks and how testing is done
- `ghidra-starfield-setup.md`: the analyzed Ghidra project and headless recipe

Repo docs: `docs/p0-findings.md` (findings with evidence), `tools/re/README.md` (reverse-engineering helpers).

## Essentials

- Build: push to `main`; GitHub Actions builds `PhoneTest.dll` (no local MSVC). Keep the repo public, or Actions
  is blocked by the account's budget.
- Deploy: game closed -> copy `PhoneTest.dll`/`.pdb` to `<Starfield>\Data\SFSE\Plugins\`. Assets:
  `py -I tools/make_test_assets.py <Starfield folder> <extracted animobjectdataslate.nif>` writes `data/`, copy it
  into `<Starfield>\Data\`. New loose files need a game restart; changed NIFs need a new file name.
- `data/` is derived from vanilla and ScreenSFSE assets: never commit it.
- The user tests in game and sends screenshots and `Documents\My Games\Starfield\SFSE\Logs\PhoneTest.log` lines.
- Don't bind keys to F5/F9 (quicksave/quickload). Answer the user in Chinese; code, commits and repo text in English.
