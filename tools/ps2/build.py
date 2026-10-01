"""Build the PS2 port (EE GCC, isolated PATH, incremental, parallel).

usage: build.py [--syntax] [--keep-going] [--jobs N] [--target ELFNAME] [files...]
  --syntax   run -fsyntax-only on the source list (no objects, no link)
  files...   restrict to these source files (paths relative to repo root)
Source list: tools/ps2/sources.txt (one path per line, '#' comments).
Output: build/ps2/obj/*.o, build/ps2/SRB2.ELF, build/ps2/build.log
"""
import argparse
import concurrent.futures as cf
import os
import re
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
DEV = Path('D:/ps2dev')
SDK = DEV / 'ps2sdk'
OUT = Path(os.environ.get('SRB2_PS2_OUT', ROOT / 'build/ps2'))  # per-agent builds: set SRB2_PS2_OUT
OBJ = OUT / 'obj'
GEN = OUT / 'gen'
CC = DEV / 'ee/bin/mips64r5900el-ps2-elf-gcc.exe'

ENV = dict(os.environ, PS2DEV=str(DEV), PS2SDK=str(SDK))
ENV['PATH'] = ';'.join(str(p) for p in [DEV/'ee/bin', DEV/'iop/bin', DEV/'bin',
                                         Path('C:/Windows/System32'), Path('C:/Windows')])

DEFS = ['-D_EE', '-DPS2', '-DPS2_PROFILE', '-DNOHW', '-DNOMD5', '-DNO_PNG_LUMPS',
        '-DNOMUMBLE', '-DNO_IPV6', '-DNOUPNP', '-DCMAKECONFIG', '-D_LARGEFILE64_SOURCE',
        '-DNOEXECINFO', '-DUNIXCOMMON', '-DHAVE_ZLIB']  # HAVE_ZLIB: bring-up scaffold for pk3, removed in phase 2
WARN = ['-Wall', '-Wextra', '-Wno-trigraphs', '-Wno-unused-parameter', '-fwrapv']
CFLAGS = ['-G0', '-O2', '-std=gnu23', '-ffunction-sections', '-fdata-sections', '-MMD', '-MP'] + DEFS + WARN
INCS = ['-I' + str(ROOT/'src'), '-I' + str(ROOT/'src/ps2'), '-I' + str(GEN),
        '-I' + str(SDK/'ee/include'), '-I' + str(SDK/'common/include'),
        '-I' + str(DEV/'gsKit/include'), '-I' + str(SDK/'ports/include')]
LDFLAGS = ['-T' + str(SDK/'ee/startup/linkfile'), '-L' + str(SDK/'ee/lib'), '-L' + str(DEV/'gsKit/lib'),
           '-L' + str(SDK/'ports/lib'), '-Wl,-zmax-page-size=128', '-Wl,--defsym,_stack_size=0x80000',
           '-Wl,--gc-sections']
LIBS = ['-lz', '-lgskit', '-ldmakit', '-laudsrv', '-lpad', '-lpoweroff', '-lfileXio', '-lcdvd',
        '-ldebug', '-lpatches', '-lm']


def gen_config():
    """Equivalent of cmake/Comptime.cmake for the CMake-less PS2 build."""
    GEN.mkdir(parents=True, exist_ok=True)
    text = (ROOT/'src/config.h.in').read_text()
    def git(*a):
        return subprocess.run(['git', *a], cwd=ROOT, capture_output=True, text=True).stdout.strip()
    rep = {'${SRB2_COMP_REVISION}': git('rev-parse', '--short', 'HEAD'),
           '${SRB2_COMP_BRANCH}': git('rev-parse', '--abbrev-ref', 'HEAD'),
           '${SRB2_COMP_NOTE}': git('log', '-1', '--format=%s').replace('"', "'"),
           '${CMAKE_BUILD_TYPE}': 'Release'}
    for k, v in rep.items():
        text = text.replace(k, v)
    text = text.replace('#cmakedefine SRB2_COMP_UNCOMMITTED', '/* clean */')
    text = text.replace('#cmakedefine01 SRB2_COMP_OPTIMIZED', '#define SRB2_COMP_OPTIMIZED 1')
    p = GEN/'config.h'
    if not p.exists() or p.read_text() != text:
        p.write_text(text)


def sources(selected):
    lines = [l.split('#')[0].strip() for l in (ROOT/'tools/ps2/sources.txt').read_text().splitlines()]
    srcs = [l for l in lines if l and (ROOT/l).exists()]  # files owned by other workers may not exist yet
    if selected:
        srcs = [s for s in srcs if s in selected]
    return srcs


def obj_for(src):
    return OBJ / (src.replace('/', '__').replace('.c', '.o'))


def stale(src, obj):
    if not obj.exists():
        return True
    t = obj.stat().st_mtime
    if (ROOT/src).stat().st_mtime > t:
        return True
    dep = obj.with_suffix('.d')
    if not dep.exists():
        return True
    txt = dep.read_text(errors='replace').replace('\\\n', ' ')
    for f in re.split(r'(?<!\\)\s+', txt.split(':', 1)[1].strip()):
        f = f.replace('\\ ', ' ')
        if f and os.path.exists(f) and os.path.getmtime(f) > t:
            return True
    return False


def compile_one(src, syntax):
    obj = obj_for(src)
    cmd = [str(CC)] + CFLAGS + INCS
    if syntax:
        cmd += ['-fsyntax-only', '-MF', str(OBJ/'syntax.d'), str(ROOT/src)]
    else:
        cmd += ['-c', str(ROOT/src), '-o', str(obj)]
    p = subprocess.run(cmd, env=ENV, capture_output=True, text=True, cwd=ROOT)
    return src, p.returncode, p.stdout + p.stderr


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--syntax', action='store_true')
    ap.add_argument('--keep-going', action='store_true')
    ap.add_argument('--jobs', type=int, default=os.cpu_count() or 4)
    ap.add_argument('--target', default='SRB2.ELF')
    ap.add_argument('--list-undefined', action='store_true')
    ap.add_argument('files', nargs='*')
    a = ap.parse_args()
    OBJ.mkdir(parents=True, exist_ok=True)
    gen_config()
    flags = ' '.join(CFLAGS + INCS)
    stamp = OBJ / 'flags.txt'
    if not stamp.exists() or stamp.read_text() != flags:
        for o in OBJ.glob('*.o'):
            o.unlink()
        stamp.write_text(flags)
    srcs = sources(set(a.files))
    todo = srcs if a.syntax else [s for s in srcs if stale(s, obj_for(s))]
    log = []
    failed = []
    t0 = time.time()
    with cf.ThreadPoolExecutor(a.jobs) as ex:
        for src, rc, out in ex.map(lambda s: compile_one(s, a.syntax), todo):
            if out.strip():
                log.append(f'=== {src} (rc={rc})\n{out}')
            if rc:
                failed.append(src)
    (OUT/'build.log').write_text('\n'.join(log), encoding='utf-8')
    print(f'compiled {len(todo)}/{len(srcs)} files in {time.time()-t0:.1f}s, failed {len(failed)}, log: {OUT/"build.log"}')
    for f in failed:
        print('  FAIL', f)
    if failed or a.syntax:
        return 1 if failed else 0
    elf = OUT / a.target
    cmd = [str(CC)] + LDFLAGS + (['-Wl,--warn-unresolved-symbols'] if a.list_undefined else []) + [str(obj_for(s)) for s in srcs] + ['-o', str(elf), '-Wl,-Map=' + str(OUT/'SRB2.map')] + LIBS
    p = subprocess.run(cmd, env=ENV, capture_output=True, text=True, cwd=ROOT)
    (OUT/'link.log').write_text(p.stdout + p.stderr, encoding='utf-8')
    if p.returncode:
        print((p.stdout + p.stderr)[-6000:])
        return 1
    print('linked', elf, elf.stat().st_size, 'bytes')
    return 0


if __name__ == '__main__':
    sys.exit(main())
