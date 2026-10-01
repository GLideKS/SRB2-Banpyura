"""Build the PS2 video-layer hardware test: tools/ps2/video_test.c + the real src/ps2/i_video.c and src/ps2/ps2_gs.c.

usage: SRB2_PS2_OUT=<dir> python tools/ps2/build_video_test.py
Output: <dir>/VIDEO_TEST.ELF (+ vt/*.o, vt/build.log). Same flags and tool environment as tools/ps2/build.py;
any compiler output (warnings included) from the three files fails the build.
"""
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import build as B  # noqa: E402  (constants of the engine build; main() is not run on import)

OUTDIR = B.OUT / 'vt'
SOURCES = ['tools/ps2/video_test.c', 'src/ps2/i_video.c', 'src/ps2/ps2_gs.c']
LIBS = ['-lgskit', '-ldmakit', '-ldebug', '-lpatches', '-lm']


def main():
    OUTDIR.mkdir(parents=True, exist_ok=True)
    B.gen_config()
    objs, log, bad = [], [], False
    for src in SOURCES:
        obj = OUTDIR / (Path(src).stem + '.o')
        cmd = [str(B.CC)] + B.CFLAGS + B.INCS + ['-c', str(B.ROOT / src), '-o', str(obj)]
        p = subprocess.run(cmd, env=B.ENV, capture_output=True, text=True, cwd=B.ROOT)
        out = (p.stdout + p.stderr).strip()
        log.append(f'=== {src} (rc={p.returncode})\n{out}')
        if p.returncode or out:
            bad = True
        objs.append(str(obj))
    (OUTDIR / 'build.log').write_text('\n'.join(log), encoding='utf-8')
    if bad:
        print('\n'.join(log))
        return 1
    elf = B.OUT / 'VIDEO_TEST.ELF'
    cmd = [str(B.CC)] + B.LDFLAGS + objs + ['-o', str(elf), '-Wl,-Map=' + str(OUTDIR / 'video_test.map')] + LIBS
    p = subprocess.run(cmd, env=B.ENV, capture_output=True, text=True, cwd=B.ROOT)
    (OUTDIR / 'link.log').write_text(p.stdout + p.stderr, encoding='utf-8')
    if p.returncode:
        print(p.stdout + p.stderr)
        return 1
    print('built', elf, elf.stat().st_size, 'bytes; compile output: none')
    return 0


if __name__ == '__main__':
    sys.exit(main())
