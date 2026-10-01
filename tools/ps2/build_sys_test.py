"""Build (and with --run, run in PCSX2 and judge) the PS2 system-layer test ELF.

usage: build_sys_test.py [--run] [--host-padmap]
  (default)       build SYS_TEST.ELF + data files into <out>/sys-test
  --run           then run it under tools/ps2/run_pcsx2.py three times (default, -iopreset, -test-error)
                  and check the results/timing from the logs
  --host-padmap   build and run the pad mapping self-test on the host with MSVC (cl)
Output directory: $SRB2_PS2_OUT (same variable as build.py) or build/ps2, then sys-test/.
The test links the real src/ps2/{ps2_boot,i_system,i_joy,i_sound,ps2_padmap}.c; engine functions they
call are stubbed in sys_test.c.
"""
import argparse
import concurrent.futures as cf
import os
import random
import re
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import build  # noqa: E402  (tools/ps2/build.py: toolchain paths and engine flags)

ROOT = build.ROOT
OUT = build.OUT / 'sys-test'
SDK = build.SDK

SRCS = ['tools/ps2/sys_test.c', 'src/ps2/ps2_boot.c', 'src/ps2/i_system.c', 'src/ps2/i_joy.c', 'src/ps2/i_sound.c',
        'src/ps2/ps2_padmap.c', 'src/ps2/ps2_padmap_test.c', 'src/m_argv.c', 'src/string.c']
# libps2_drivers carries the embedded IRX images; it must precede the libraries it was built against
LIBS = ['-lps2_drivers', '-lpad', '-lpoweroff', '-lfileXio', '-lcdvd', '-ldebug', '-lpatches', '-lm']


def fnv1a(data):
    h = 2166136261
    for b in data:
        h = ((h ^ b) * 16777619) & 0xffffffff
    return h


def make_data():
    OUT.mkdir(parents=True, exist_ok=True)
    blob = random.Random(20261001).randbytes(12 * 1024 * 1024 + 4099)  # > 8 MiB, not a block multiple
    (OUT / 'sys_test_big.bin').write_bytes(blob)
    (OUT / 'sys_test_big.sum').write_text('%08x\n' % fnv1a(blob))
    (OUT / 'srb2.pk3').write_bytes(b'PK')  # only has to exist: I_LocateWad probes for it
    # ps2args: one argument per line; comments and blank lines are dropped, whitespace trimmed
    (OUT / 'ps2args').write_text('-fromfile\n# a comment\n\n+x\n   last  \n')


def compile_all():
    objdir = OUT / 'obj'
    objdir.mkdir(parents=True, exist_ok=True)
    cmd0 = [str(build.CC)] + build.CFLAGS + ['-Werror'] + build.INCS

    def one(src):
        obj = objdir / (src.replace('/', '__').replace('.c', '.o'))
        extra = ['-Wno-error'] if src in ('src/m_argv.c', 'src/string.c') else []  # engine files, not ours
        p = subprocess.run(cmd0 + extra + ['-c', str(ROOT / src), '-o', str(obj)], env=build.ENV, capture_output=True, text=True, cwd=ROOT)
        return src, obj, p.returncode, p.stdout + p.stderr

    objs, bad = [], False
    with cf.ThreadPoolExecutor(os.cpu_count() or 4) as ex:
        for src, obj, rc, out in ex.map(one, SRCS):
            if out.strip():
                print('=== %s (rc=%d)\n%s' % (src, rc, out))
            bad |= rc != 0
            objs.append(obj)
    return None if bad else objs


def link(objs):
    elf = OUT / 'SYS_TEST.ELF'
    cmd = [str(build.CC)] + build.LDFLAGS + [str(o) for o in objs] + ['-o', str(elf), '-Wl,-Map=' + str(OUT / 'SYS_TEST.map')] + LIBS
    p = subprocess.run(cmd, env=build.ENV, capture_output=True, text=True, cwd=ROOT)
    (OUT / 'link.log').write_text(p.stdout + p.stderr, encoding='utf-8')
    if p.returncode:
        print((p.stdout + p.stderr)[-6000:])
        return None
    return elf


def pcsx2(name, args, timeout=150):
    log = OUT / ('pcsx2-%s.log' % name)
    cmd = [sys.executable, str(ROOT / 'tools/ps2/run_pcsx2.py'), '--elf', str(OUT / 'SYS_TEST.ELF'), '--log', str(log),
           '--timeout', str(timeout)]
    if args:
        cmd.append('--args=' + args)
    p = subprocess.run(cmd, capture_output=True, text=True)
    return p.returncode, log.read_text(errors='replace') if log.exists() else '', p.stdout.strip().splitlines()[-1:]


STAMP = re.compile(r'^\[\s*(\d+),(\d+)\]')


def stamp(line):
    m = STAMP.match(line)
    return int(m.group(1)) + int(m.group(2)) / 10000.0 if m else None  # "[   sec,1e-4 s]"


def st_lines(text):
    return [l.split('] ', 1)[-1] for l in text.splitlines() if 'ST ' in l or 'STUB ' in l or 'PS2BOOT' in l or 'I_Error' in l]


def judge(name, code, text, expect_error):
    ok = True
    lines = st_lines(text)
    fails = [l for l in lines if 'ST FAIL' in l]
    print('--- run %-9s pcsx2 exit code %s' % (name, code))
    for l in lines:
        print('   ', l)
    if code != 0:
        print('FAIL: PCSX2 did not exit by itself (poweroff) within the timeout'); ok = False
    if fails:
        print('FAIL: %d test failures' % len(fails)); ok = False
    if 'ST COMPLETE failures=0' not in text:
        print('FAIL: no "ST COMPLETE failures=0"'); ok = False
    seq = ['STUB M_SaveConfig', 'STUB G_SaveGameData', 'STUB D_QuitNetGame', 'STUB M_FreePlayerSetupColors', 'STUB I_ShutdownGraphics', 'STUB W_Shutdown', 'PS2BOOT exit']
    pos = -1
    tail = text[text.rfind('ST COMPLETE'):]
    for s in seq:
        i = tail.find(s, pos + 1)
        if i < 0:
            print('FAIL: shutdown sequence missing "%s"' % s); ok = False
            break
        pos = i
    if expect_error:
        if 'I_Error(): test error 42: fatal path' not in text:
            print('FAIL: I_Error text not printed'); ok = False
        if 'PS2BOOT exit code=-1 poweroff=1' not in text:
            print('FAIL: I_Error did not end with poweroff'); ok = False
    else:
        if 'PS2BOOT exit code=0 poweroff=1' not in text:
            print('FAIL: I_Quit did not end with poweroff'); ok = False
    return ok


def wall_check(text, secs=10.0):
    t0 = t1 = None
    for l in text.splitlines():
        if 'ST wall_begin' in l:
            t0 = stamp(l)
        elif 'ST wall_end' in l:
            t1 = stamp(l)
    if t0 is None or t1 is None:
        print('FAIL: wall markers missing'); return False
    wall = t1 - t0
    print('host wall clock (PCSX2 log timestamps) for %d tics: %.4f s, expected %.4f s: %+.2f %%' % (secs * 35, wall, secs, (wall / secs - 1) * 100))
    return abs(wall / secs - 1) <= 0.01


def vblank_check(text):
    ok = True
    for m in re.finditer(r'ST tic35 window=(\d+) precise_ms=([\d.]+) cop0_ms=([\d.]+) vblanks=(\d+)', text):
        w, p, c, v = int(m.group(1)), float(m.group(2)), float(m.group(3)), int(m.group(4))
        print('window %d: bus timer %.3f ms, COP0 Count %.3f ms (%+.3f %%), vblanks %d' % (w, p, c, (c / p - 1) * 100, v))
        ok &= abs(p / 1000.0 - 1) <= 0.01 and abs(c / p - 1) <= 0.001
    m = re.search(r'ST tic35 total tics=(\d+) precise_ms=([\d.]+) cop0_ms=([\d.]+) vblanks=(\d+) vblank_hz=([\d.]+)', text)
    if not m:
        print('FAIL: no total line'); return False
    tics, pms, cms, vb, hz = int(m.group(1)), float(m.group(2)), float(m.group(3)), int(m.group(4)), float(m.group(5))
    print('total: %d tics in %.3f ms of bus timer (%.3f ms COP0): %+.3f %% against 35.000 Hz; %d vblanks = %.3f Hz (NTSC 59.940 / PAL 50.000)'
          % (tics, pms, cms, (pms / (tics / 35.0 * 1000.0) - 1) * 100, vb, hz))
    ok &= abs(pms / (tics / 35.0 * 1000.0) - 1) <= 0.01
    ok &= abs(hz / 59.94 - 1) <= 0.01 or abs(hz / 50.0 - 1) <= 0.01
    return ok


def run_all():
    ok = True
    runs = [('default', '-alpha beta', False), ('iopreset', '-iopreset -alpha beta', False), ('error', '-alpha beta -test-error', True)]
    texts = {}
    for name, args, err in runs:
        code, text, _ = pcsx2(name, args)
        texts[name] = text
        ok &= judge(name, code, text, err)
    print('--- timing (run default)')
    ok &= wall_check(texts['default'])
    ok &= vblank_check(texts['default'])
    print('RESULT', 'PASS' if ok else 'FAIL')
    return ok


def host_padmap():
    vc = r'C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat'
    d = ROOT / 'build' / 'agent-sys' / 'host'
    d.mkdir(parents=True, exist_ok=True)
    bat = d / 'build_host.bat'
    bat.write_text('@echo off\r\ncall "%s" >nul 2>&1\r\ncd /d %s\r\ncl /nologo /W4 /WX /DPS2PAD_HOST_TEST /I%s %s %s /Fe:padmap_host.exe /Fo:.\\\r\nif errorlevel 1 exit /b 1\r\n.\\padmap_host.exe\r\n'
                   % (vc, d, ROOT / 'src/ps2', ROOT / 'src/ps2/ps2_padmap.c', ROOT / 'src/ps2/ps2_padmap_test.c'))
    p = subprocess.run(['cmd.exe', '/c', str(bat)], capture_output=True)
    print(p.stdout.decode('cp866', errors='replace'))
    return p.returncode == 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--run', action='store_true')
    ap.add_argument('--host-padmap', action='store_true')
    a = ap.parse_args()
    if a.host_padmap:
        return 0 if host_padmap() else 1
    build.gen_config()
    make_data()
    objs = compile_all()
    if not objs:
        return 1
    elf = link(objs)
    if not elf:
        return 1
    print('built', elf, elf.stat().st_size, 'bytes')
    if a.run:
        return 0 if run_all() else 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
