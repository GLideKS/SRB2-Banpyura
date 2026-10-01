"""Verify SRP2 packs against the pk3 archives they were cooked from (independent reader, shares no code with cook.py).

usage: verify_pack.py [--src DIR] [--pak DIR]
For every pack: header/table sanity, entry count and ORDER and names equal the zip central directory, the SHA-256 of EVERY
decoded lump equals the SHA-256 of the lump read from the pk3, name/hash/longname derived like ResGetLumpsZip,
alignment rules, no overlaps. Exit code 0 only if there are 0 discrepancies.
"""
import argparse
import hashlib
import struct
import sys
import zipfile
from pathlib import Path

import lz4.block

ROOT = Path(__file__).resolve().parents[2]
PAIRS = [('srb2.pk3', 'SRB2.PAK'), ('zones.pk3', 'ZONES.PAK'), ('characters.pk3', 'CHARS.PAK'), ('music.pk3', 'MUSIC.PAK')]


def cstr(pool, off):
    end = pool.index(b'\0', off)
    return pool[off:end]


def derive(full):
    # ResGetLumpsZip: trimname after the last '/', dotpos = last '.' of trimname (or the end), name = first min(8, ..) chars
    slash = full.rfind(b'/')
    trim = full[slash + 1:] if slash >= 0 else full
    dot = trim.rfind(b'.')
    length = dot if dot >= 0 else len(trim)
    longname = trim[:length]
    name = longname[:8]
    x = 5381
    for ch in name:
        x = ((x * 33) & 0xFFFFFFFF) ^ (ch | 0x20 if 65 <= ch <= 90 else ch)
    return name, x, longname


def decode(f, pos, disksize, size, codec, block):
    f.seek(pos)
    raw = f.read(disksize)
    if len(raw) != disksize:
        raise ValueError('short read')
    if codec == 0:
        if disksize != size:
            raise ValueError('raw lump with disksize != size')
        return raw
    if codec != 1:
        raise ValueError(f'unknown codec {codec}')
    if size <= block:
        return lz4.block.decompress(raw, uncompressed_size=size)
    nb = (size + block - 1) // block
    idx = struct.unpack_from(f'<{nb}I', raw)
    p = 4 * nb
    out = bytearray()
    for b in range(nb):
        cs = idx[b] & 0x7FFFFFFF
        bs = min(block, size - b * block)
        blk = raw[p:p + cs]
        p += cs
        out += blk if idx[b] >> 31 else lz4.block.decompress(blk, uncompressed_size=bs)
        if (idx[b] >> 31) and cs != bs:
            raise ValueError('raw block of wrong size')
    if p != len(raw):
        raise ValueError('block sizes do not add up to disksize')
    return bytes(out)


def verify(pk3, pak):
    bad = []

    def err(msg):
        bad.append(msg)
        if len(bad) <= 20:
            print('   ', msg)

    zf = zipfile.ZipFile(pk3)
    infos = zf.infolist()
    f = open(pak, 'rb')
    hdr = f.read(64)
    magic, version, hsize, flags, n, table_off, pool_off, pool_size, data_off, file_size, block, rsv = struct.unpack('<4s11I', hdr[:48])
    if magic != b'SRP2' or version != 1 or hsize != 64 or block != 65536:
        err(f'bad header {magic} {version} {hsize} {block}')
        return len(bad), 0, {}
    if file_size != Path(pak).stat().st_size:
        err('file_size field != actual size')
    if n != len(infos):
        err(f'entry count {n} != zip {len(infos)}')
    f.seek(table_off)
    table = [struct.unpack('<6I', f.read(24)) for _ in range(n)]
    f.seek(pool_off)
    pool = f.read(pool_size)
    if not pool or pool[-1] != 0:
        err('pool not NUL terminated')
    prev_end = data_off
    stats = {0: 0, 1: 0}
    for i, (zi, (pos, disksize, size, fo, lo, codec)) in enumerate(zip(infos, table)):
        full = cstr(pool, fo)
        if full.decode('ascii') != zi.filename:
            err(f'[{i}] name {full!r} != zip {zi.filename!r}')
        name, h, longname = derive(full)
        if cstr(pool, lo) != longname:
            err(f'[{i}] longname {cstr(pool, lo)!r} != derived {longname!r}')
        if size != zi.file_size:
            err(f'[{i}] size {size} != zip {zi.file_size}')
        if zi.is_dir() and not (size == 0 and full.endswith(b'/')):
            err(f'[{i}] directory not stored as an empty entry')
        if size == 0:
            if disksize != 0:
                err(f'[{i}] empty lump with data')
            data = b''
        else:
            a = 2048 if size >= block else 64
            if pos % a:
                err(f'[{i}] position {pos} not aligned to {a}')
            if pos < prev_end:
                err(f'[{i}] overlaps previous data ({pos} < {prev_end})')
            if pos + disksize > file_size:
                err(f'[{i}] data beyond end of file')
            prev_end = pos + disksize
            try:
                data = decode(f, pos, disksize, size, codec, block)
            except Exception as e:
                err(f'[{i}] {full!r}: decode failed: {e}')
                continue
        if len(data) != size:
            err(f'[{i}] decoded {len(data)} bytes, expected {size}')
        ref = b'' if zi.is_dir() else zf.read(zi)
        if hashlib.sha256(data).digest() != hashlib.sha256(ref).digest():
            err(f'[{i}] {full!r}: sha256 mismatch')
        stats[codec] = stats.get(codec, 0) + 1
    return len(bad), n, stats


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--src', default=str(ROOT / 'srb2-assets'))
    ap.add_argument('--pak', default=str(ROOT / 'build/pak'))
    a = ap.parse_args()
    total = 0
    for pk3, pak in PAIRS:
        print(f'{pak} vs {pk3} ...', flush=True)
        nbad, n, stats = verify(Path(a.src) / pk3, Path(a.pak) / pak)
        total += nbad
        print(f'  {n} lumps compared (sha256 of every decoded lump), raw {stats.get(0, 0)}, lz4 {stats.get(1, 0)}: '
              f'{nbad} discrepancies')
    print('TOTAL discrepancies:', total)
    return 1 if total else 0


if __name__ == '__main__':
    sys.exit(main())
