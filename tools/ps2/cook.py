"""Cook the SRB2 pk3 archives into SRP2 packs (format: docs/PACK_FORMAT.md).

usage: cook.py [--src DIR] [--out DIR] [--jobs N] [--only NAME ...]
  --src   directory with srb2.pk3 zones.pk3 characters.pk3 music.pk3 (default srb2-assets)
  --out   output directory (default build/pak): SRB2.PAK ZONES.PAK CHARS.PAK MUSIC.PAK

One pack per pk3, same entry order as the zip central directory (this is the order the engine's
ResGetLumpsZip walks, so wadnum / lumpnum / folder logic stay as they were). Lump data is stored raw or as
LZ4HC (64 KiB blocks for lumps > 64 KiB). Needs `pip install lz4`.
Verify with tools/ps2/verify_pack.py.
"""
import argparse
import hashlib
import os
import struct
import sys
import time
import zipfile
from concurrent.futures import ProcessPoolExecutor
from pathlib import Path

import lz4.block

ROOT = Path(__file__).resolve().parents[2]

PACKS = [('srb2.pk3', 'SRB2.PAK'), ('zones.pk3', 'ZONES.PAK'), ('characters.pk3', 'CHARS.PAK'), ('music.pk3', 'MUSIC.PAK')]

MAGIC = b'SRP2'
VERSION = 1
BLOCK = 65536          # decoded block size of big lumps
SECTOR = 2048
ALIGN_SMALL = 64
MIN_PACK_SIZE = 256    # smaller lumps are always raw
MAX_RATIO = 0.90       # LZ4 only if the stored form is <= 90% of the raw size
CM_RAW, CM_LZ4 = 0, 1
HEADER = struct.Struct('<4s11I16x')   # magic, version, header_size, flags, numlumps, table_off, pool_off, pool_size, data_off, file_size, block, 0
ENTRY = struct.Struct('<6I')          # position, disksize, size, fullname_off, longname_off, codec
FLAG_NONMUSIC = 1                     # W_VerifyNMUSlumps would say "has other lumps" (lump is "important")
assert HEADER.size == 64 and ENTRY.size == 24


def align(v, a):
    return (v + a - 1) // a * a


# ---- the engine's view of the zip (ResGetLumpsZip), used to prove the order and to derive names -------------

def engine_walk(path):
    """Walk the central directory exactly like ResGetLumpsZip: returns [(fullname bytes, compression, csize, size)]."""
    data = Path(path).read_bytes()
    start = max(0, len(data) - (22 + 65536))
    p = data.find(b'PK\x05\x06', start)          # ResFindSignature: first match in the tail window
    if p < 0:
        raise SystemExit(f'{path}: missing central directory')
    (_sig, _disk, _cdisk, _de, entries, _cdsize, cdoff, _clen) = struct.unpack_from('<4sHHHHIIH', data, p)
    out = []
    q = cdoff
    for _ in range(entries):
        (sig, _v, _vn, _fl, comp, _mt, _md, _crc, csize, size, nlen, xlen, clen, _ds, _ai, _ae, _off) = struct.unpack_from('<4s6HIIIHHHHHII', data, q)
        if sig != b'PK\x01\x02':
            raise SystemExit(f'{path}: central directory is corrupt')
        name = data[q + 46:q + 46 + nlen]
        out.append((name, comp, csize, size))
        q += 46 + nlen + xlen + clen
    return out


def engine_names(full):
    """name[8], hash, longname exactly as ResGetLumpsZip computes them from the full name (bytes)."""
    i = full.rfind(b'/')
    trim = full[i + 1:] if i >= 0 else full
    d = trim.rfind(b'.')
    long_ = trim if d < 0 else trim[:d]           # dotpos - trimname characters of trimname
    name = long_[:8]
    h = 5381
    for c in name:
        if c == 0:
            break
        h = ((h * 33) & 0xFFFFFFFF) ^ (c + 32 if 65 <= c <= 90 else c)
    return name, h, long_


# W_VerifyNMUSlumps (music.pk3 may only hold these) -----------------------------------------------------
NMUS = [b'D_', b'O_', b'DS', b'ENDOOM', b'PLAYPAL', b'PAL', b'COLORMAP', b'CLM', b'TRANS', b'CONSBACK', b'SAVE',
        b'BLACXLVL', b'GAMEDONE', b'CONT', b'STNONEX', b'ULTIMATE', b'SLCT', b'LSSTATIC', b'BLANKLV', b'CRFNT',
        b'NTFNT', b'NTFNO', b'LTFNT', b'TTL', b'STCFN', b'TNYFN', b'STLIVE', b'CROSHAI', b'INTERSC', b'SPECTILE',
        b'STT', b'YB_', b'RESULT', b'RACE', b'SRB2BACK', b'M_', b'LT', b'HOMING', b'HOMITM', b'CHARFG', b'CHARBG',
        b'RECATK', b'RECCLOCK', b'NTSATK', b'NTSSONC', b'SLID', b'CONT', b'MINICAPS', b'BLUESTAT', b'BYELSTAT',
        b'ORNGSTAT', b'REDSTAT', b'YELSTAT', b'NBRACKET', b'NGHTLINK', b'NGT', b'NARROW', b'NREDAR', b'NSS', b'NBON',
        b'NRNG', b'NHUD', b'CAPS', b'DRILL', b'GRADE', b'MINUS5', b'NGRTIMER', b'MUSICDEF', b'SHADERS', b'SH_']
FOLDER_BLACKLIST = [b'Lua/', b'SOC/', b'Sprites/', b'LongSprites/', b'Textures/', b'Patches/', b'Flats/', b'Fades/']


def _strncasecmp0(a, b, n):
    """strncasecmp(a, b, n) == 0 for C strings (a may be NUL padded)."""
    a = a.split(b'\0', 1)[0]
    for k in range(n):
        ca = a[k] if k < len(a) else 0
        cb = b[k] if k < len(b) else 0
        la = ca + 32 if 65 <= ca <= 90 else ca
        lb = cb + 32 if 65 <= cb <= 90 else cb
        if la != lb:
            return False
        if la == 0:
            return True
    return True


def music_only(names):
    """True if W_VerifyPK3(checklist=NMUSlist, status=false) accepts every entry (W_VerifyNMUSlumps() == 1)."""
    for full in names:
        i = full.rfind(b'/')
        trim = full[i + 1:] if i >= 0 else full
        if not trim:
            continue                              # directories are ignored
        name, _, _ = engine_names(full)
        if not any(_strncasecmp0(name, e, len(e)) for e in NMUS):
            return False
        if any(_strncasecmp0(full, e, len(e)) for e in FOLDER_BLACKLIST):
            return False
    return True


# ---- compression ----------------------------------------------------------------------------------------------

def lz4_block(b):
    return lz4.block.compress(b, mode='high_compression', compression=12, store_size=False)


def lz4_lump(data):
    """Stored LZ4 form of a lump: one raw block if len <= BLOCK, else u32 index[nblocks] (bit31 = block stored raw) + blocks."""
    if len(data) <= BLOCK:
        return lz4_block(data)
    idx, body = [], []
    for i in range(0, len(data), BLOCK):
        b = data[i:i + BLOCK]
        c = lz4_block(b)
        if len(c) >= len(b):
            idx.append(len(b) | 0x80000000)
            body.append(b)
        else:
            idx.append(len(c))
            body.append(c)
    return struct.pack(f'<{len(idx)}I', *idx) + b''.join(body)


def encode(args):
    """Worker: read entry `i` of the pk3 (zipfile checks the CRC), choose a codec. Returns (i, codec, payload, size, sha256)."""
    path, i = args
    global _zf
    if _zf is None or _zf[0] != path:
        z = zipfile.ZipFile(path)
        _zf = (path, z, z.infolist())
    zi = _zf[2][i]
    data = b'' if zi.is_dir() else _zf[1].read(zi)
    sha = hashlib.sha256(data).digest()
    if len(data) < MIN_PACK_SIZE or data[:4] == b'OggS' or data[:4] == b'\x89PNG':
        return i, CM_RAW, data, len(data), sha        # tiny, Ogg (music is kept byte for byte) or PNG (already deflated)
    packed = lz4_lump(data)
    if len(packed) <= len(data) * MAX_RATIO:
        return i, CM_LZ4, packed, len(data), sha
    return i, CM_RAW, data, len(data), sha


_zf = None


# ---- pack writer ----------------------------------------------------------------------------------------------

def cook(src, dst, jobs):
    zf = zipfile.ZipFile(src)
    infos = zf.infolist()
    walk = engine_walk(src)
    # prove that infolist() order == the order ResGetLumpsZip walks, and that the engine can read every entry
    if len(walk) != len(infos):
        raise SystemExit(f'{src}: engine sees {len(walk)} entries, zipfile {len(infos)}')
    for k, ((name, comp, csize, size), zi) in enumerate(zip(walk, infos)):
        if b'\0' in name or b'\n' in name or not name.isascii():
            raise SystemExit(f'{src}: entry {k} {name!r}: NUL/newline/non-ASCII names are not supported')
        if name.decode('ascii') != zi.filename or size != zi.file_size or csize != zi.compress_size:
            raise SystemExit(f'{src}: entry {k} differs between engine walk and zipfile ({name!r} vs {zi.filename!r})')
        if comp not in (0, 8):
            raise SystemExit(f'{src}: entry {k} {name!r} uses compression {comp} (engine: unsupported)')
    zf.close()
    n = len(walk)
    if n > 0xFFFF:
        raise SystemExit(f'{src}: {n} entries do not fit in UINT16')
    names = [w[0] for w in walk]

    t0 = time.time()
    with ProcessPoolExecutor(jobs) as ex:
        results = list(ex.map(encode, [(str(src), i) for i in range(n)], chunksize=8))
    t_enc = time.time() - t0

    # string pool: fullname\0 [longname\0]; a longname equal to the tail of its fullname shares it
    pool = bytearray()
    seen = {}
    offs = []
    for full in names:
        _, _, long_ = engine_names(full)
        if full in seen:
            fo = seen[full]
        else:
            fo = seen[full] = len(pool)
            pool += full + b'\0'
        trim = full[full.rfind(b'/') + 1:]
        if long_ == trim:
            lo = fo + len(full) - len(trim)       # no extension: longname is the tail of fullname (shares its NUL)
        elif long_ in seen:
            lo = seen[long_]
        else:
            lo = seen[long_] = len(pool)
            pool += long_ + b'\0'
        offs.append((fo, lo))
    pool_size = len(pool)

    table_off = SECTOR
    pool_off = align(table_off + ENTRY.size * n, SECTOR)
    data_off = align(pool_off + pool_size, SECTOR)
    pos = data_off
    entries = []
    layout = []
    for (i, codec, payload, size, sha), (fo, lo) in zip(results, offs):
        if len(payload) == 0:
            entries.append(ENTRY.pack(0, 0, 0, fo, lo, CM_RAW))
            layout.append(None)
            continue
        pos = align(pos, SECTOR if size >= BLOCK else ALIGN_SMALL)
        entries.append(ENTRY.pack(pos, len(payload), size, fo, lo, codec))
        layout.append(pos)
        pos += len(payload)
    file_size = align(pos, SECTOR)
    flags = 0 if music_only(names) else FLAG_NONMUSIC

    with open(dst, 'wb') as f:
        f.write(HEADER.pack(MAGIC, VERSION, HEADER.size, flags, n, table_off, pool_off, pool_size, data_off, file_size, BLOCK, 0))
        f.seek(table_off)
        f.write(b''.join(entries))
        f.seek(pool_off)
        f.write(pool)
        for (i, codec, payload, size, sha), p in zip(results, layout):
            if p is not None:
                f.seek(p)
                f.write(payload)
        f.truncate(file_size)

    # statistics
    st = {}
    for (i, codec, payload, size, sha) in results:
        s = st.setdefault(codec, [0, 0, 0])
        s[0] += 1
        s[1] += size
        s[2] += len(payload)
    return dict(n=n, stats=st, file_size=file_size, pool=pool_size, flags=flags, t=t_enc, srcsize=Path(src).stat().st_size,
                png=sum(1 for r in results if r[2][:4] == b'\x89PNG'),
                folders=sum(1 for nm in names if nm.endswith(b'/')))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--src', default=str(ROOT / 'srb2-assets'))
    ap.add_argument('--out', default=str(ROOT / 'build/pak'))
    ap.add_argument('--jobs', type=int, default=os.cpu_count() or 4)
    ap.add_argument('--only', nargs='*', default=[], help='pk3 names to cook (default all four)')
    a = ap.parse_args()
    out = Path(a.out)
    out.mkdir(parents=True, exist_ok=True)
    tot = [0, 0, 0]
    codec_name = {CM_RAW: 'raw', CM_LZ4: 'lz4'}
    for pk3, pak in PACKS:
        if a.only and pk3 not in a.only:
            continue
        r = cook(Path(a.src) / pk3, out / pak, a.jobs)
        print(f'{pk3} -> {pak}: {r["n"]} entries ({r["folders"]} folders), pk3 {r["srcsize"]:,} B, pack {r["file_size"]:,} B, '
              f'pool {r["pool"]:,} B, nonmusic={r["flags"] & 1}, encode {r["t"]:.1f}s')
        for c, (cnt, raw, disk) in sorted(r['stats'].items()):
            print(f'    {codec_name[c]:4} {cnt:6} lumps  {raw:>13,} B -> {disk:>13,} B' + (f'  ({disk / raw * 100:.1f}%)' if raw else ''))
        tot[0] += r['srcsize']
        tot[1] += r['file_size']
    print(f'total: pk3 {tot[0]:,} B -> packs {tot[1]:,} B')


if __name__ == '__main__':
    sys.exit(main())
