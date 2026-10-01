"""Build the standalone phase-0 probe using an isolated 32-bit tool PATH."""
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[2]
DEV = Path('D:/ps2dev')
SDK = DEV / 'ps2sdk'
OUT = ROOT / 'build/ps2-hello'
OUT.mkdir(parents=True, exist_ok=True)
env = dict(os.environ, PS2DEV=str(DEV), PS2SDK=str(SDK))
env['PATH'] = ';'.join(str(p) for p in [DEV/'ee/bin', DEV/'iop/bin', DEV/'bin', Path('C:/Windows/System32'), Path('C:/Windows')])
cc = DEV/'ee/bin/mips64r5900el-ps2-elf-gcc.exe'
cmd = [str(cc), '-D_EE', '-G0', '-O2', '-std=gnu11', '-Wall', '-Wextra', '-Werror',
       '-I'+str(SDK/'ee/include'), '-I'+str(SDK/'common/include'), '-I'+str(DEV/'gsKit/include'),
       '-T'+str(SDK/'ee/startup/linkfile'), '-L'+str(SDK/'ee/lib'), '-L'+str(DEV/'gsKit/lib'),
       '-Wl,-zmax-page-size=128', '-Wl,--defsym,_stack_size=0x80000',
       '-Wl,-Map='+str(OUT/'HELLO.map'), str(ROOT/'tools/ps2/hello.c'), str(ROOT/'tools/ps2/division_probe.c'), '-o', str(OUT/'HELLO.ELF'),
       '-lgskit', '-ldmakit', '-laudsrv', '-lpad', '-lpoweroff', '-lfileXio', '-ldebug', '-lpatches', '-lm']
with (OUT/'build.log').open('w') as log:
    log.write(json.dumps(cmd)+'\n'); log.flush()
    p = subprocess.run(cmd, env=env, stdout=log, stderr=subprocess.STDOUT)
if p.returncode:
    print((OUT/'build.log').read_text()); raise SystemExit(p.returncode)
for name in ['iomanX','fileXio','audsrv','padman','sio2man','poweroff','cdfs']:
    shutil.copy2(SDK/'iop/irx'/f'{name}.irx', OUT)
(OUT/'sentinel.bin').write_bytes(b'S'+bytes(2046)+b'B')
for tool, args in [('size', ['-A']), ('readelf',['-h']), ('gcc',['--version'])]:
    p=subprocess.run([str(cc).replace('gcc.exe',tool+'.exe')]+args+([] if tool=='gcc' else [str(OUT/'HELLO.ELF')]),env=env,capture_output=True,text=True)
    (OUT/f'{tool}.log').write_text(p.stdout+p.stderr)
print('Built', OUT/'HELLO.ELF', (OUT/'HELLO.ELF').stat().st_size, 'bytes', hashlib.sha256((OUT/'HELLO.ELF').read_bytes()).hexdigest())
for symbol in ['ps2_probe_sdiv','ps2_probe_udiv','__divdi3','__udivdi3']:
    p=subprocess.run([str(cc).replace('gcc.exe','objdump.exe'),'-d','--disassemble='+symbol,str(OUT/'HELLO.ELF')],env=env,capture_output=True,text=True,check=True)
    (OUT/(symbol+'.asm')).write_text(p.stdout)
