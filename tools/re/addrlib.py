import struct, sys
# usage: addrlib.py <versionlib.bin> id:<n>|rva:<hex> ...
data = open(sys.argv[1], 'rb').read()
offs = struct.unpack_from(f'<{(len(data) - 0x60) // 4}I', data, 0x60)
rev = {}
for i, o in enumerate(offs):
    if o: rev.setdefault(o, i)
for q in sys.argv[2:]:
    k, v = q.split(':')
    if k == 'id':
        print(f'id {v} -> rva 0x{offs[int(v)]:X}  va 0x{0x140000000 + offs[int(v)]:X}')
    else:
        r = int(v, 16); print(f'rva 0x{r:X} -> id {rev.get(r)}')
