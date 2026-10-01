"""Run the PS2 build in PCSX2 with the PS2REF hooks and compare against the PC golden data.

usage: run_ps2_ref.py --mode demo --demo DEMO_001 [--no-build] [--timeout SEC] [--out DIR]
       run_ps2_ref.py --mode title [--no-build]
  demo : plays DEMO_00n (-timedemo) and compares tics.csv, frames.csv and every frame-*.idx with
         golden/phase0-v2/run1/DEMO_00n byte for byte (memory-*.csv ignored).
  title: dumps title-screen frames 35/70/105 (-ps2ref-title 105 -skipintro) and compares them with
         golden/phase1/title (made by tools/ps2/make_title_ref.py on the PC build).
Exit code 0 only if the PS2 run finished (complete.txt) and every compared file is identical.
Work dir: $SRB2_PS2_RUN (default build/ps2-ref-run; ELF + hardlinked pk3 + .srb2 home), build objects in <dir>-build.
Several agents work at once: each MUST set its own SRB2_PS2_RUN. The engine log is boot.txt in the work dir.
Pass extra engine arguments after --: run_ps2_ref.py --mode demo --demo DEMO_001 -- -memtrace
"""
import argparse
import csv
import os
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
RUN = Path(os.environ.get('SRB2_PS2_RUN', ROOT / 'build/ps2-ref-run'))  # per-agent: set SRB2_PS2_RUN=build/<unique name>
BUILD = RUN.parent / (RUN.name + '-build')
GOLDEN = ROOT / 'golden/phase0-v2/run1'
TITLE = ROOT / 'golden/phase1/title'


def build():
    env = dict(os.environ, SRB2_PS2_OUT=str(BUILD))
    p = subprocess.run([sys.executable, str(ROOT / 'tools/ps2/build.py'), '--ps2ref'], env=env)
    if p.returncode:
        raise SystemExit('build failed')
    shutil.copy2(BUILD / 'SRB2.ELF', RUN / 'SRB2.ELF')


def prepare(demo):
    RUN.mkdir(parents=True, exist_ok=True)
    for name in ['srb2', 'zones', 'characters', 'music']:
        dst = RUN / f'{name}.pk3'
        if not dst.exists():
            try:
                os.link(ROOT / 'srb2-assets' / f'{name}.pk3', dst)
            except OSError:
                shutil.copy2(ROOT / 'srb2-assets' / f'{name}.pk3', dst)
    for pak in (ROOT / 'build/pak').glob('*.PAK') if (ROOT / 'build/pak').exists() else []:
        shutil.copy2(pak, RUN / pak.name)  # cooked packs, if built (phase 2)
    (RUN / '.srb2').mkdir(exist_ok=True)
    (RUN / '.srb2/reference.cfg').write_text('fpscap "35"\nfullscreen "Off"\nshowfps "Off"\nshowping "Off"\n')
    if demo:
        shutil.copy2(ROOT / 'golden/phase0-v2' / f'{demo}.lmp', RUN / '.srb2' / f'{demo}.lmp')
    out = RUN / 'refout'
    shutil.rmtree(out, ignore_errors=True)
    out.mkdir()
    return out


def compare(golden, got, patterns):
    bad = []
    n = 0
    for pat in patterns:
        names = sorted(p.name for p in golden.glob(pat))
        if not names:
            bad.append(f'no golden files for {pat}')
        for name in names:
            n += 1
            a, b = golden / name, got / name
            if not b.exists():
                bad.append(f'missing {name}')
            elif a.read_bytes() != b.read_bytes():
                bad.append(f'differs {name}')
    return n, bad


def first_tic_mismatch(golden, got):
    try:
        ga = list(csv.reader((golden / 'tics.csv').open()))
        gb = list(csv.reader((got / 'tics.csv').open()))
    except OSError:
        return None
    for i, (x, y) in enumerate(zip(ga, gb)):
        if x != y:
            return i, x, y
    if len(ga) != len(gb):
        return min(len(ga), len(gb)), None, None
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--mode', choices=['demo', 'title'], required=True)
    ap.add_argument('--demo', default='DEMO_001')
    ap.add_argument('--no-build', action='store_true')
    ap.add_argument('--timeout', type=float, default=900)
    ap.add_argument('extra', nargs='*')
    a = ap.parse_args()
    RUN.mkdir(parents=True, exist_ok=True)
    out = prepare(a.demo if a.mode == 'demo' else None)
    if not a.no_build:
        build()
    args = ['-logfile', 'boot.txt', '-ps2ref', 'host:/refout', '-config', 'reference.cfg', '-nolog', '-noendtxt']
    if a.mode == 'demo':
        args += ['-timedemo', a.demo + '.lmp']
    else:
        args += ['-skipintro', '-ps2ref-title', '105']
    args += a.extra
    log = RUN / 'pcsx2.log'
    cmd = [sys.executable, str(ROOT / 'tools/ps2/run_pcsx2.py'), '--elf', str(RUN / 'SRB2.ELF'), '--log', str(log),
           '--args=' + ' '.join(args), '--timeout', str(a.timeout), '--until-file', str(out / 'complete.txt'),
           '--until', 'complete']
    print(' '.join(cmd))
    subprocess.run(cmd)
    boot = RUN / 'boot.txt'
    done = (out / 'complete.txt').exists()
    print('finished:', done)
    if not done:
        tail = boot.read_text(errors='replace').splitlines()[-12:] if boot.exists() else ['(no boot.txt)']
        print('\n'.join(tail))
        return 1
    if a.mode == 'demo':
        gold = GOLDEN / a.demo
        n, bad = compare(gold, out, ['tics.csv', 'frames.csv', 'frame-*.idx'])
        mm = first_tic_mismatch(gold, out)
        if mm:
            print('first differing tic row', mm)
    else:
        n, bad = compare(TITLE, out, ['frames.csv', 'title-*.idx'])
    print(f'compared {n} files, differences: {len(bad)}')
    for b in bad[:20]:
        print('  ', b)
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
