import struct, zlib, sys
# usage: esmrefs.py <esm> <hexFormID>[,<hexFormID>...]  -> records whose body contains the formID bytes
path = sys.argv[1]
needles = {int(x, 16): struct.pack('<I', int(x, 16)) for x in sys.argv[2].split(',')}
data = open(path, 'rb').read()

def edid(body):
    i = 0
    while i + 6 <= len(body):
        t = body[i:i+4]; sz = struct.unpack_from('<H', body, i+4)[0]
        if t == b'EDID': return body[i+6:i+6+sz].rstrip(b'\0').decode('latin1')
        i += 6 + sz
    return ''

def walk(off, end):
    while off < end:
        t = data[off:off+4]; sz = struct.unpack_from('<I', data, off+4)[0]
        if t == b'GRUP':
            walk(off + 24, off + sz); off += sz; continue
        flags, fid = struct.unpack_from('<II', data, off+8)
        body = data[off+24:off+24+sz]
        if flags & 0x40000:
            try: body = zlib.decompress(body[4:])
            except Exception: body = b''
        for n, b in needles.items():
            if n != fid and b in body:
                print(f'{n:08X} <- {t.decode()} {fid:08X} {edid(body)}')
        off += 24 + sz

walk(0, len(data))
