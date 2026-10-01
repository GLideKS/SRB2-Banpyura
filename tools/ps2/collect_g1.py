"""Archive existing G1 evidence without rebuilding, running, or changing inputs.

The numerical checks are independent of the strict profile/arena verdict.
Do not interpret numerical_checks_passed as permission to advance to G2.
"""
import csv
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT / 'docs/GATES/g1'
DEV = Path('D:/ps2dev')


def sha(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def copy(source, name):
    target = OUT / name
    target.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(source, target)


def main():
    if not OUT.parent.is_dir():
        raise SystemExit('Gate parent directory missing')
    OUT.mkdir(exist_ok=False)
    title = ROOT / 'build/g1-final-title'
    idle = ROOT / 'build/g1-final'
    build = ROOT / 'build/g1-final-title-build'
    golden = ROOT / 'golden/phase1/title'
    report = {'gate_verdict': 'RED', 'remaining': [
        'PNG/zlib and ZIP/UDMF runtime code still retained',
        'zone remains per-block malloc, not D4 arena/frame-stamped LRU',
        'remote/addon entry points not fully excluded',
        'soft-double remains in hot paths; no full PS2 CPU phase profile'],
        'title': {}, 'idle': {}, 'elf': {}, 'memory': {}}
    for run, name in [(title, 'title'), (idle, 'idle')]:
        copy(run / 'boot.txt', f'{name}-boot.log')
        copy(run / 'pcsx2.log', f'{name}-pcsx2.log')
        copy(run / 'refout/complete.txt', f'{name}-complete.txt')
        copy(run / '.srb2/reference.cfg', f'{name}-reference.cfg')
        text = (run / 'pcsx2.log').read_text(errors='replace')
        report[name]['poweroff_seen'] = 'sceCdPowerOff called' in text
        report[name]['elf_sha256'] = sha(run / 'SRB2.ELF')
    for name in ['frames.csv', 'title-000035.idx', 'title-000070.idx', 'title-000105.idx']:
        a, b = golden / name, title / 'refout' / name
        ab, bb = a.read_bytes(), b.read_bytes()
        report['title'][name] = {'golden_sha256': sha(a), 'ps2_sha256': sha(b),
            'differences': sum(x != y for x, y in zip(ab, bb)) + abs(len(ab) - len(bb))}
    copy(title / 'refout/frames.csv', 'title-frames.csv')
    boot = (idle / 'boot.txt').read_text(errors='replace')
    report['idle']['progress'] = [dict((key, int(value)) for key, value in
        re.findall(r'(\w+)=(\d+)', line)) for line in boot.splitlines() if line.startswith('G1 idle ')]
    report['idle']['gs_submit'] = [dict((key, int(value)) for key, value in
        re.findall(r'(\w+)=(\d+)', line)) for line in boot.splitlines() if line.startswith('G1 gs_submit ')]
    for path in sorted((idle / 'refout').glob('memory-*.csv')):
        with path.open(newline='') as stream:
            report['memory'][path.name] = {row['tag']: int(row['bytes']) for row in csv.DictReader(stream)}
        copy(path, 'memory/' + path.name)
    env = dict(os.environ)
    env['PATH'] = ';'.join(str(p) for p in [DEV/'ee/bin', DEV/'bin', Path('C:/Windows/System32'), Path('C:/Windows')])
    for tool, args, name in [('size', ['-A'], 'size.log'), ('readelf', ['-lW'], 'readelf.log'),
                             ('nm', ['--defined-only'], 'symbols.log')]:
        text = subprocess.check_output([str(DEV/f'ee/bin/mips64r5900el-ps2-elf-{tool}.exe'),
            *args, str(title/'SRB2.ELF')], env=env, text=True)
        (OUT/name).write_text(text, encoding='utf-8')
        if tool == 'size':
            report['elf']['sections'] = {name: int(size) for name, size in re.findall(r'^(\.\S+)\s+(\d+)\s+\d+', text, re.M)}
        if tool == 'nm':
            functions = [line.split()[-1] for line in text.splitlines() if re.match(r'^[0-9a-f]+ [Tt] ', line)]
            report['elf']['png_prefix_functions'] = [name for name in functions if name.startswith('png_')]
            report['elf']['forbidden_examples'] = [name for name in functions if name in
                ('inflate', 'deflate', 'M_SavePNG', 'Picture_PNGConvert', 'TextmapParse', 'P_WriteTextmap', 'ResFindSignature')]
    report['elf']['bytes'] = (title/'SRB2.ELF').stat().st_size
    report['elf']['sha256'] = sha(title/'SRB2.ELF')
    for name in ('build.log', 'link.log'):
        copy(build / name, name)
    copy(build/'obj/flags.txt', 'flags.txt')
    report['build_diagnostics_empty'] = all(not (build/name).read_text().strip() for name in ('build.log', 'link.log'))
    host = ROOT/'build/host-profile-g1/equivalence-final/report.json'
    hostdata = json.loads(host.read_text())
    report['host_profile'] = {'passed': hostdata['passed'], 'inputs_changed': hostdata['inputs_changed'],
        'demos': {name: {key: data[key] for key in ('exit_code', 'tics.csv_rows', 'idx_files', 'soc.tsv_rows', 'sfx.csv_rows')}
                  | {'compared_files': data['comparison']['compared_files'], 'differences': data['comparison']['differences']}
                  for name, data in hostdata['demos'].items()}}
    copy(host, 'host-profile-report.json')
    for path in sorted((ROOT/'build/host-profile-g1').glob('*20261001-150235.*')):
        copy(path, 'host-profile/' + path.name)
    for path in sorted((ROOT/'build/agent-equivalence-g1').rglob('*.log')):
        copy(path, 'equivalence/' + path.relative_to(ROOT/'build/agent-equivalence-g1').as_posix())
    for name in ('blend-results.json', 'texture-results.json'):
        copy(ROOT/'build/agent-equivalence-g1'/name, 'equivalence/'+name)
    for path in sorted((ROOT/'build/agent-zone-g1').rglob('*.log')):
        copy(path, 'zone/' + path.relative_to(ROOT/'build/agent-zone-g1').as_posix())
    for name in ('test.log', 'build.log', 'input-sha256.txt'):
        copy(ROOT/'build/agent-pack-g1'/name, 'pack/'+name)
    report['inputs'] = {str(p): sha(p) for p in [
        *sorted((ROOT/'build/pak').glob('*.PAK')),
        *sorted((ROOT/'srb2-assets').glob('*.pk3')),
        Path('D:/PCSX2/pcsx2-qt.exe'), Path('D:/PCSX2-test/pcsx2-qt.exe'),
        Path('D:/PCSX2-test/inis/PCSX2.ini')]}
    report['numerical_checks_passed'] = (report['build_diagnostics_empty'] and report['elf']['bytes'] <= 7000000
        and all(report['title'][name]['differences'] == 0 for name in ('frames.csv', 'title-000035.idx', 'title-000070.idx', 'title-000105.idx'))
        and report['idle']['progress'][-1]['seconds'] >= 600
        and report['idle']['poweroff_seen'] and report['host_profile']['passed'])
    report['source_sha256'] = {p.relative_to(ROOT).as_posix(): sha(p) for folder in ('src', 'tools/ps2')
        for p in sorted((ROOT/folder).rglob('*')) if p.suffix in ('.c', '.h', '.py', '.ps1', '.txt')}
    (OUT/'report.json').write_text(json.dumps(report, indent=2), encoding='utf-8')
    print('ELF bytes:', report['elf']['bytes'])
    print('Idle last:', report['idle']['progress'][-1])
    print('Numeric checks passed:', report['numerical_checks_passed'], '; G1 remains RED')
    print('Evidence:', OUT)


if __name__ == '__main__':
    main()
