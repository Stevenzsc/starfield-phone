import struct, zlib, sys
# usage: esmform.py <esm> <hexFormID>,...   -> dump those records' subrecords
path = sys.argv[1]
want = {int(x, 16) for x in sys.argv[2].split(',')}
data = open(path, 'rb').read()

def show(v):
    t = v.rstrip(b'\0')
    if t and len(v) < 400 and all(32 <= b < 127 for b in t): return repr(t.decode())
    return v[:64].hex() + ('..' if len(v) > 64 else '') + f' ({len(v)}b)'

def walk(off, end):
    while off < end:
        t = data[off:off+4]; sz = struct.unpack_from('<I', data, off+4)[0]
        if t == b'GRUP':
            walk(off + 24, off + sz); off += sz; continue
        flags, fid = struct.unpack_from('<II', data, off+8)
        if fid in want:
            body = data[off+24:off+24+sz]
            if flags & 0x40000: body = zlib.decompress(body[4:])
            print(f'== {t.decode()} {fid:08X}')
            i = 0
            while i + 6 <= len(body):
                st = body[i:i+4].decode('latin1'); n = struct.unpack_from('<H', body, i+4)[0]
                print(f'   {st}: {show(body[i+6:i+6+n])}'); i += 6 + n
        off += 24 + sz

walk(0, len(data))
