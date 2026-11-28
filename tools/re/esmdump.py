import struct, zlib, sys, re

path, types, pat = sys.argv[1], set(sys.argv[2].split(',')), re.compile(sys.argv[3], re.I)
data = open(path, 'rb').read()

def subrecs(buf):
    i, big = 0, None
    while i + 6 <= len(buf):
        t = buf[i:i+4].decode('latin1'); sz = struct.unpack_from('<H', buf, i+4)[0]; i += 6
        if t == 'XXXX':
            big = struct.unpack_from('<I', buf, i)[0]; i += sz; continue
        if big is not None: sz, big = big, None
        yield t, buf[i:i+sz]; i += sz

def show(v):
    if 2 < len(v) < 300 and all(32 <= b < 127 for b in v.rstrip(b'\0')) and v.rstrip(b'\0'):
        return repr(v.rstrip(b'\0').decode())
    return v[:48].hex() + ('..' if len(v) > 48 else '') + f' ({len(v)}b)'

def walk(off, end):
    while off < end:
        t = data[off:off+4].decode('latin1'); sz = struct.unpack_from('<I', data, off+4)[0]
        if t == 'GRUP':
            walk(off + 24, off + sz); off += sz; continue
        flags, fid = struct.unpack_from('<II', data, off+8)
        body = data[off+24: off+24+sz]
        if t in types:
            if flags & 0x40000: body = zlib.decompress(body[4:])
            subs = list(subrecs(body))
            text = ' '.join(show(v) for _, v in subs)
            if pat.search(text):
                print(f'== {t} {fid:08X}')
                for st, v in subs: print(f'   {st}: {show(v)}')
        off += 24 + sz

walk(0, len(data))
