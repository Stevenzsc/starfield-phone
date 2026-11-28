import struct, zlib, sys, re, os
# usage: ba2x.py <archive> <regex> <outdir>
arc, pat, out = sys.argv[1], re.compile(sys.argv[2], re.I), sys.argv[3]
f = open(arc, 'rb')
magic, ver, typ, cnt, nameoff = struct.unpack('<4sI4sIQ', f.read(24))
assert typ == b'GNRL', typ
comp = 0
if ver >= 2: f.read(8)
if ver == 3: comp = struct.unpack('<I', f.read(4))[0]
entries = [struct.unpack('<IIIIQIII', f.read(36)) for _ in range(cnt)]
f.seek(nameoff); names = []
for _ in range(cnt):
    l = struct.unpack('<H', f.read(2))[0]; names.append(f.read(l).decode('utf8'))
n = 0
for (h, ext, dh, fl, off, packed, size, al), name in zip(entries, names):
    if not pat.search(name): continue
    f.seek(off); raw = f.read(packed if packed else size)
    if packed:
        if comp == 3:
            import lz4.block; raw = lz4.block.decompress(raw, uncompressed_size=size)
        else: raw = zlib.decompress(raw)
    p = os.path.join(out, name.replace('/', os.sep)); os.makedirs(os.path.dirname(p), exist_ok=True)
    open(p, 'wb').write(raw); n += 1
print('ver', ver, 'comp', comp, 'extracted', n)
