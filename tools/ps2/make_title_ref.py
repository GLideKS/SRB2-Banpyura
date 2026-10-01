"""Make golden/phase1/title: indexed title-screen frames 35/70/105 from the PC reference build, twice (must match).

usage: make_title_ref.py   (needs build/pc-golden/bin/Release/srb2win_ps2.exe, see tools/ps2/build_reference.ps1)
"""
import os
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
EXE = ROOT / 'build/pc-golden/bin/Release/srb2win_ps2.exe'
DEST = ROOT / 'golden/phase1/title'


def run(out):
    shutil.rmtree(out, ignore_errors=True)
    out.mkdir(parents=True)
    home = out / 'home'
    (home / 'srb2').mkdir(parents=True)
    (home / 'srb2/reference.cfg').write_text('fpscap "35"\nfullscreen "Off"\nshowfps "Off"\nshowping "Off"\n')
    env = dict(os.environ, SRB2WADDIR=str(ROOT / 'srb2-assets'))
    cmd = [str(EXE), '-ps2ref', str(out), '-home', str(home), '-config', 'reference.cfg', '-nolog', '-noendtxt',
           '-win', '-width', '320', '-height', '200', '-skipintro', '-ps2ref-title', '105']
    subprocess.run(cmd, cwd=out, env=env, capture_output=True, timeout=300, check=True)


def main():
    a, b = ROOT / 'build/title-ref/a', ROOT / 'build/title-ref/b'
    run(a)
    run(b)
    names = ['frames.csv'] + [f'title-{n:06}.idx' for n in (35, 70, 105)]
    for n in names:
        if (a / n).read_bytes() != (b / n).read_bytes():
            raise SystemExit(f'PC title frames are not repeatable: {n}')
    shutil.rmtree(DEST, ignore_errors=True)
    DEST.mkdir(parents=True)
    for n in names:
        shutil.copy2(a / n, DEST / n)
    print('golden/phase1/title written:', names)


if __name__ == '__main__':
    sys.exit(main())
