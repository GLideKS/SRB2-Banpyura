"""Collect every get_number() expression the game can evaluate, from the PC reference build.

For every map in zones.pk3/srb2.pk3 the PC build (-warp MAPxx -ps2ref-scan) loads the map and evaluates
the string arguments of all linedefs and things; the startup SOC loading and the demo runs add the rest.
Result: tools/ps2/soc_strings.tsv (header expression_hex<TAB>value), the input of gen_soc_numbers.py.
usage: collect_soc_strings.py [--jobs N]
"""
import argparse
import concurrent.futures as cf
import glob
import os
import re
import shutil
import subprocess
import sys
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
EXE = ROOT / 'build/pc-golden/bin/Release/srb2win_ps2.exe'
SCAN = ROOT / 'build/scan'
OUT = ROOT / 'tools/ps2/soc_strings.tsv'


def maps():
    names = []
    for pk3 in ('zones.pk3', 'srb2.pk3'):
        for n in zipfile.ZipFile(ROOT / 'srb2-assets' / pk3).namelist():
            m = re.search(r'(?:^|/)(MAP[0-9A-Z]{2})\.wad$', n, re.I)
            if m:
                names.append(m.group(1).upper())
    return sorted(set(names))


def scan(name):
    out = SCAN / name
    shutil.rmtree(out, ignore_errors=True)
    out.mkdir(parents=True)
    home = out / 'home'
    (home / 'srb2').mkdir(parents=True)
    (home / 'srb2/reference.cfg').write_text('fpscap "35"\nfullscreen "Off"\nshowfps "Off"\nshowping "Off"\n')
    env = dict(os.environ, SRB2WADDIR=str(ROOT / 'srb2-assets'))
    cmd = [str(EXE), '-ps2ref', str(out), '-home', str(home), '-config', 'reference.cfg', '-nolog', '-noendtxt',
           '-win', '-width', '320', '-height', '200', '-skipintro', '-warp', name, '-ps2ref-scan']
    try:
        p = subprocess.run(cmd, cwd=out, env=env, capture_output=True, timeout=180)
        ok = (out / 'complete.txt').exists()
    except subprocess.TimeoutExpired:
        ok = False
    return name, ok


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--jobs', type=int, default=6)
    a = ap.parse_args()
    shutil.rmtree(SCAN, ignore_errors=True)
    SCAN.mkdir(parents=True)
    names = maps()
    print(len(names), 'maps')
    failed = []
    with cf.ThreadPoolExecutor(a.jobs) as ex:
        for name, ok in ex.map(scan, names):
            if not ok:
                failed.append(name)
    print('scan failed (no complete.txt):', failed)
    table = {}
    files = glob.glob(str(SCAN / '*/soc.tsv')) + glob.glob(str(ROOT / 'golden/phase0-v2/run1/DEMO_*/soc.tsv'))
    for f in files:
        for line in Path(f).read_text().splitlines()[1:]:
            h, v = line.split('\t')
            if table.setdefault(h, v) != v:
                sys.exit(f'conflicting value for {bytes.fromhex(h)!r}')
    OUT.write_text('expression_hex\tvalue\n' + ''.join(f'{h}\t{v}\n' for h, v in sorted(table.items())))
    print(f'wrote {OUT}: {len(table)} distinct expressions from {len(files)} logs')


if __name__ == '__main__':
    main()
