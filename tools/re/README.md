# Reverse-engineering helpers

Run Python with `py -I` (isolated) on files from the game or archives.

| Tool | Use |
|---|---|
| `addrlib.py <versionlib-1-16-244-0.bin> id:<n> \| rva:<hex>` | Address Library: ID <-> RVA (`Data\SFSE\Plugins\versionlib-*.bin`) |
| `ba2x.py <archive.ba2> <regex> <outdir>` | extract files from a GNRL BA2 (zlib) |
| `esmdump.py <esm> <TYPES,...> <regex>` | dump records of given types whose subrecords match |
| `esmform.py <esm> <hexID,...>` | dump given records' subrecords |
| `esmrefs.py <esm> <hexID,...>` | records whose data contains a form ID |
| `ghidra/run.cmd <Script.java> <out> <args>` | headless Ghidra on the analyzed project |

Ghidra scripts: `DumpFuncs` (decompile addresses), `DumpCallers` (refs to addresses), `RangeRefs` (refs into a
range), `StrXrefs` (refs to exact C strings), `ScalarRefs` (instructions using an immediate, e.g. a chunk ID like
`QOBJ` = 4A424F51), `DisFunc`/`Disasm` (disassembly), `VSlot` (function at vtable slots), `DumpSyms` (vtable dump).

Useful recipes: Papyrus natives are registered with their name strings (StrXrefs on the native name, then the
LEA R9 before the name is the implementation). ESM record loaders switch on 4-byte chunk IDs (ScalarRefs).
