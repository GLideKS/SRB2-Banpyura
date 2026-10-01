"""Host test of the engine's pack reader: builds src/w_pack.c (unchanged) with MSVC together with tools/ps2/pack_hosttest.c
and liblz4, decodes every lump of the packs through it and compares with the pk3 (names, order, size, crc32, hash, longname).

usage: test_pack_reader.py [--lz4-src DIR]   (DIR with lz4.c/lz4.h; default build/scratch/lz4src/lz4-4.4.5/lz4libs,
                                              from `pip download lz4 --no-binary :all: --no-deps` + unpack)
"""
import argparse
import hashlib
import struct
import subprocess
import sys
import time
import zipfile
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools/ps2'))
from verify_pack import PAIRS, derive  # noqa: E402  (name derivation: independent of the C code)

VCVARS = r'C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat'


def build(lz4, out):
    out.mkdir(parents=True, exist_ok=True)
    exe = out / 'pack_hosttest.exe'
    bat = out / 'build.bat'
    cmd = ['cl', '/nologo', '/O2', '/W3', '/WX', '/wd4244', '/wd4267', '/wd4018', '/wd4146', '/wd4996',
           '/D_CRT_SECURE_NO_WARNINGS', '/DPS2_PROFILE', '/DNOHW', '/DNDEBUG',
           '/I' + str(ROOT / 'src'), '/I' + str(ROOT / 'build/pc-golden/src'), '/I' + str(lz4),
           '/Fe:pack_hosttest.exe', str(ROOT / 'tools/ps2/pack_hosttest.c'), str(ROOT / 'src/w_pack.c'), str(lz4 / 'lz4.c')]
    bat.write_text(f'@echo off\ncall "{VCVARS}" >nul 2>&1\nif errorlevel 1 exit /b 1\n'
                   + subprocess.list2cmdline(cmd) + '\n', encoding='utf-8')
    p = subprocess.run(['cmd', '/c', str(bat)], cwd=out, capture_output=True, text=True, encoding='oem', errors='replace')
    (out / 'build.log').write_text(p.stdout + p.stderr, encoding='utf-8')
    if p.returncode:
        print(p.stdout, p.stderr)
        raise SystemExit('host test build failed')
    return exe


def sha256(path):
    with path.open('rb') as f:
        return hashlib.file_digest(f, 'sha256').hexdigest()


def validation_tests(exe, out, report):
    """Disposable fixtures only: check metadata and index rejection without touching existing packs."""
    fixtures = out / 'validation'
    fixtures.mkdir(exist_ok=True)
    pool = b'test.bin\0test\0'
    payload = struct.pack('<2I', 0x80010000, 0x80010000) + b'A' * 65536 + b'B' * 65536
    file_size = (6144 + len(payload) + 2047) // 2048 * 2048
    valid = bytearray(file_size)
    struct.pack_into('<4s11I16x', valid, 0, b'SRP2', 1, 64, 1, 1, 2048, 4096, len(pool), 6144, file_size, 65536, 0)
    struct.pack_into('<6I', valid, 2048, 6144, len(payload), 131072, 0, 9, 1)
    valid[4096:4096 + len(pool)] = pool
    valid[6144:6144 + len(payload)] = payload
    path = fixtures / 'valid.bin'
    path.write_bytes(valid)
    p = subprocess.run([str(exe), str(path), str(fixtures / 'valid.tsv')], capture_output=True, text=True)
    (fixtures / 'valid.log').write_text(p.stdout + p.stderr, encoding='utf-8')
    bad = int(p.returncode != 0)
    if p.returncode == 0:
        row = (fixtures / 'valid.tsv').read_text().splitlines()[1].split('\t')
        bad += int(row[-1] != f'{zlib.crc32(payload[8:]):08x}')
    report('Synthetic two-raw-block LZ4 fixture: ' + p.stderr.strip() + f' (exit {p.returncode})')

    # More than 64 blocks exercises the heap-index branch, including unaligned partial destinations.
    large_data = b'A' * 65536 + b'B' * (64 * 65536)
    large_payload = struct.pack('<65I', *([0x80010000] * 65)) + large_data
    large_size = (6144 + len(large_payload) + 2047) // 2048 * 2048
    large = bytearray(valid[:6144]) + bytearray(large_size - 6144)
    struct.pack_into('<I', large, 36, large_size)
    struct.pack_into('<2I', large, 2048 + 4, len(large_payload), len(large_data))
    large[6144:6144 + len(large_payload)] = large_payload
    path = fixtures / 'heap-index.bin'
    path.write_bytes(large)
    p = subprocess.run([str(exe), str(path), str(fixtures / 'heap-index.tsv')], capture_output=True, text=True)
    (fixtures / 'heap-index.log').write_text(p.stdout + p.stderr, encoding='utf-8')
    bad += int(p.returncode != 0)
    if p.returncode == 0:
        row = (fixtures / 'heap-index.tsv').read_text().splitlines()[1].split('\t')
        bad += int(row[-1] != f'{zlib.crc32(large_data):08x}')
    report('Synthetic 65-raw-block LZ4 fixture: ' + p.stderr.strip() + f' (exit {p.returncode})')
    cases = [
        ('table-overlaps-header', 'header', 20, 0),
        ('pool-overlaps-table', 'header', 24, 2048),
        ('data-overlaps-pool', 'header', 32, 4096),
        ('pool-outside-file', 'header', 28, file_size),
        ('unknown-flags', 'header', 12, 2),
        ('bad-name-offset', 'header', 2048 + 12, len(pool)),
        ('wrong-longname', 'header', 2048 + 16, 0),
        ('unknown-codec', 'header', 2048 + 20, 2),
        ('raw-size-mismatch', 'header', 2048 + 20, 0),
        ('unaligned-payload', 'header', 2048, 6145),
        ('payload-outside-file', 'header', 2048 + 4, file_size),
        ('index-exceeds-disksize', 'header', 2048 + 4, 4),
        ('zero-block-size', 'index', 6144, 0),
        ('oversized-block', 'index', 6144, 65537),
        ('raw-block-size-mismatch', 'index', 6144, 0x8000FFFF),
        ('index-total-too-short', 'index', 6148, 65535),
        ('index-total-too-long', 'index', 2048 + 4, len(payload) - 1),
        ('invalid-unrequested-block', 'index', 6148, 0),
        ('invalid-lz4-body', 'read', 6144, 65536),
    ]
    for name, mode, offset, value in cases:
        data = bytearray(valid)
        struct.pack_into('<I', data, offset, value)
        path = fixtures / (name + '.bin')
        path.write_bytes(data)
        p = subprocess.run([str(exe), '--reject-' + mode, str(path)], capture_output=True, text=True)
        (fixtures / (name + '.log')).write_text(p.stdout + p.stderr, encoding='utf-8')
        bad += int(p.returncode != 0)
        report(f'Validation {name}: exit {p.returncode}')
    for name, data in [('unterminated-pool', valid[:]), ('truncated-file', valid[:-2048])]:
        if name == 'unterminated-pool':
            data[4096 + len(pool) - 1] = 65
        path = fixtures / (name + '.bin')
        path.write_bytes(data)
        p = subprocess.run([str(exe), '--reject-header', str(path)], capture_output=True, text=True)
        (fixtures / (name + '.log')).write_text(p.stdout + p.stderr, encoding='utf-8')
        bad += int(p.returncode != 0)
        report(f'Validation {name}: exit {p.returncode}')
    report(f'Validation: {len(cases) + 2} rejection cases, 2 independently CRC-checked baselines, {bad} failures')
    return bad


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--lz4-src', default=str(ROOT / 'build/scratch/lz4src/lz4-4.4.5/lz4libs'))
    ap.add_argument('--pak', default=str(ROOT / 'build/pak'))
    ap.add_argument('--src', default=str(ROOT / 'srb2-assets'))
    ap.add_argument('--out', default=str(ROOT / 'build/agent-pack-g1'), help='isolated executable, tables and logs')
    a = ap.parse_args()
    out = Path(a.out).resolve()
    out.mkdir(parents=True, exist_ok=True)
    messages = []

    def report(message):
        print(message, flush=True)
        messages.append(message)
        (out / 'test.log').write_text('\n'.join(messages) + '\n', encoding='utf-8')

    inputs = [Path(root) / name for pk3, pak in PAIRS for root, name in [(a.src, pk3), (a.pak, pak)]]
    before = {str(p): sha256(p) for p in inputs}
    (out / 'input-sha256.txt').write_text(''.join(f'{h}  {p}\n' for p, h in before.items()), encoding='utf-8')
    exe = build(Path(a.lz4_src).resolve(), out)
    report('Host build: /DPS2_PROFILE /W3 /WX; see build.log (selected legacy-header warnings disabled)')
    total_bad = 0
    for pk3, pak in PAIRS:
        tsv = out / (pak + '.tsv')
        t0 = time.perf_counter()
        p = subprocess.run([str(exe), str(Path(a.pak) / pak), str(tsv)], capture_output=True, text=True)
        (out / (pak + '.log')).write_text(p.stdout + p.stderr, encoding='utf-8')
        report(f'{pak}: {p.stderr.strip()} (exit {p.returncode}, {time.perf_counter() - t0:.2f}s)')
        bad = 0 if p.returncode == 0 else 1
        if not tsv.exists():
            report(f'  Missing output table: {tsv}')
            total_bad += 1
            continue
        rows = [l.rstrip('\n').split('\t') for l in tsv.read_text(errors='replace').splitlines()]
        if not rows:
            report(f'  Empty output table: {tsv}')
            total_bad += 1
            continue
        head, rows = rows[0], rows[1:]
        expected_nonmusic = 0 if pak == 'MUSIC.PAK' else 1
        if head != ['nonmusic', str(expected_nonmusic), 'verify', str(1 - expected_nonmusic)]:
            report(f'  Incorrect music classification: {head}')
            bad += 1
        with zipfile.ZipFile(Path(a.src) / pk3) as zf:
            infos = zf.infolist()
        if len(rows) != len(infos):
            report(f'  entry count differs: {len(rows)} vs {len(infos)}')
            bad += 1
        for i, (r, zi) in enumerate(zip(rows, infos)):
            if len(r) != 8:
                report(f'  Malformed row {i}: {r}')
                bad += 1
                continue
            idx, name, h, longname, fullname, size, comp, crc = r
            en, eh, el = derive(zi.filename.encode())
            exp = (str(i), zi.filename, en.decode(), '%08x' % eh, el.decode(), str(zi.file_size), '%08x' % zi.CRC)
            got = (idx, fullname, name, h, longname, size, crc)
            if exp != got:
                bad += 1
                if bad < 10:
                    report(f'  lump {idx}: expected {exp}, got {got}')
        report(f'  {len(rows)} lumps: index/names/hash/longname/size/crc32 vs pk3 -> {bad} differences')
        total_bad += bad
    total_bad += validation_tests(exe, out, report)
    changed = [str(p) for p in inputs if sha256(p) != before[str(p)]]
    total_bad += len(changed)
    report(f'Input preservation: {len(inputs)} packs/archives hashed before and after, {len(changed)} changed')
    report(f'TOTAL differences/failures: {total_bad}')
    return 1 if total_bad else 0


if __name__ == '__main__':
    sys.exit(main())
