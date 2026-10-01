"""Compare a fresh -ps2ref run directory with a golden run, byte for byte.

usage: compare_golden.py <golden run dir> <candidate run dir>
Each dir holds DEMO_00n/ subdirectories. Ignored: command.json, stdout.log, home/
(host paths) and memory-*.csv (PU_STATIC contains the output path string, so the
byte count changes with path length; compared separately by tag, tag 1 excluded).
"""
import csv
import hashlib
import sys
from pathlib import Path

SKIP = {'command.json', 'stdout.log'}


def files(d):
    return sorted(p.name for p in d.iterdir() if p.is_file() and p.name not in SKIP)


def memory(p):
    return {r['tag']: int(r['bytes']) for r in csv.DictReader(p.open())}


def main(golden, cand):
    bad = n = 0
    for demo in sorted(p.name for p in golden.iterdir() if p.is_dir()):
        a, b = golden / demo, cand / demo
        if files(a) != files(b):
            print(demo, 'file set differs'); bad += 1; continue
        for f in files(a):
            n += 1
            if f.startswith('memory-'):
                ma, mb = memory(a / f), memory(b / f)
                ma.pop('1', None); mb.pop('1', None)
                if ma != mb:
                    print('DIFF', demo, f); bad += 1
            elif (a / f).read_bytes() != (b / f).read_bytes():
                print('DIFF', demo, f); bad += 1
        sha = hashlib.sha256((a / 'tics.csv').read_bytes()).hexdigest()
        print(demo, 'tics.csv sha256', sha)
    print('files compared', n, 'differences', bad)
    return 1 if bad else 0


if __name__ == '__main__':
    raise SystemExit(main(Path(sys.argv[1]), Path(sys.argv[2])))
