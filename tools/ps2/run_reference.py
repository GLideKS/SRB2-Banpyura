"""Run complete original attract demos twice in isolated homes, compare bytes."""
import argparse
import csv
import hashlib
import json
import os
from pathlib import Path
import subprocess
import shutil
import zipfile

ROOT=Path(__file__).resolve().parents[2]
ap=argparse.ArgumentParser()
ap.add_argument('--output',type=Path,default=ROOT/'golden/phase0-v1')
ap.add_argument('--demos',nargs='+',default=[f'DEMO_{i:03}' for i in range(1,5)])
ap.add_argument('--runs',type=int,default=2)
args=ap.parse_args()
base=args.output.resolve(); base.mkdir(parents=True,exist_ok=True)
exe=ROOT/'build/pc-golden/bin/Release/srb2win_ps2.exe'
def digest(p): return hashlib.file_digest(p.open('rb'),'sha256').hexdigest()
manifest={'upstream':'0e09462308610005f640ed84b21ec8a4ef116a4b','executable_sha256':digest(exe),'assets':{},'demos':{}}
for name in ['srb2','zones','characters','music']:
    manifest['assets'][name+'.pk3']=digest(ROOT/'srb2-assets'/f'{name}.pk3')
with zipfile.ZipFile(ROOT/'srb2-assets/srb2.pk3') as z:
    for name in args.demos:
        matches=[n for n in z.namelist() if Path(n).stem.upper()==name]
        if len(matches)!=1: raise RuntimeError((name,matches))
        demo=base/(name+'.lmp'); demo.write_bytes(z.read(matches[0]))
        manifest['demos'][name]={'pk3_entry':matches[0],'sha256':digest(demo)}
(base/'manifest.json').write_text(json.dumps(manifest,indent=2))
env=dict(os.environ)
env['SRB2WADDIR']=str(ROOT/'srb2-assets')
startup=subprocess.STARTUPINFO(); startup.dwFlags|=subprocess.STARTF_USESHOWWINDOW; startup.wShowWindow=0
results={}
for run in range(1,args.runs+1):
    for name in args.demos:
        out=base/f'run{run}'/name
        out.mkdir(parents=True,exist_ok=False)
        home=out/'home'; home.mkdir()
        gamehome=home/'srb2'; gamehome.mkdir()
        cfg=gamehome/'reference.cfg'
        shutil.copy2(base/(name+'.lmp'),gamehome/(name+'.lmp'))
        cfg.write_text('fpscap "35"\nfullscreen "Off"\nshowfps "Off"\nshowping "Off"\n',encoding='ascii')
        cmd=[str(exe),'-ps2ref',str(out),'-home',str(home),'-config','reference.cfg','-nolog','-noendtxt',
             '-win','-width','320','-height','200','-timedemo',name+'.lmp']
        (out/'command.json').write_text(json.dumps(cmd,indent=2))
        print('RUN',run,name,flush=True)
        with (out/'stdout.log').open('wb') as log:
            p=subprocess.Popen(cmd,cwd=out,env=env,stdout=log,stderr=subprocess.STDOUT,startupinfo=startup)
            try: code=p.wait(timeout=240)
            except subprocess.TimeoutExpired:
                p.terminate(); p.wait(); raise RuntimeError(f'{name} timeout; see {out}/stdout.log')
        if code or not (out/'complete.txt').exists():
            raise RuntimeError(f'{name} failed ({code}); see {out}/stdout.log')
        rows=list(csv.DictReader((out/'tics.csv').open()))
        frames=list(csv.DictReader((out/'frames.csv').open()))
        if not rows or not frames: raise RuntimeError(f'{name}: empty tic/frame log')
        results[f'run{run}/{name}']={'tics':len(rows),'frames':len(frames),'end_leveltic':rows[-1]['leveltic']}
        print('DONE',name,results[f'run{run}/{name}'],flush=True)
if args.runs>=2:
    for name in args.demos:
        a=base/'run1'/name; b=base/'run2'/name
        wanted=['tics.csv','frames.csv','soc.tsv','sfx.csv','complete.txt']+[p.name for p in a.glob('*.idx')]+[p.name for p in a.glob('*.pcm')]
        differences=[f for f in wanted if not (b/f).exists() or (a/f).read_bytes()!=(b/f).read_bytes()]
        if {p.name for p in a.glob('*.pcm')}!={p.name for p in b.glob('*.pcm')}: differences.append('PCM file set')
        results[name]={'compared_files':len(wanted),'differences':differences}
    (base/'comparison.json').write_text(json.dumps(results,indent=2))
    print(json.dumps(results,indent=2))
    if any(results[n]['differences'] for n in args.demos): raise SystemExit(1)
