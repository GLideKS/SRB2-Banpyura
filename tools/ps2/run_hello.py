"""Real emulator run, optional ISO and WASAPI loopback, strict exit/log checks."""
import argparse, io, json, subprocess, sys, time, wave
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2]
OUT=ROOT/'build/ps2-hello'
ap=argparse.ArgumentParser(); ap.add_argument('--name',default='pcsx2-verified'); ap.add_argument('--disc',action='store_true'); ap.add_argument('--audio',action='store_true'); ap.add_argument('--badclut',action='store_true')
args=ap.parse_args()
startup=subprocess.STARTUPINFO(); startup.dwFlags|=subprocess.STARTF_USESHOWWINDOW; startup.wShowWindow=0
cmd=['D:\\PCSX2\\pcsx2-qt.exe','-portable','-batch','-nogui','-fastboot','-elf',str(OUT/'HELLO.ELF'),'-logfile',str(OUT/(args.name+'.log'))]
if args.disc:
    import pycdlib
    iso=pycdlib.PyCdlib(); iso.new(interchange_level=1,vol_ident='SRB2_P0')
    data=(OUT/'sentinel.bin').read_bytes(); iso.add_fp(io.BytesIO(data),len(data),iso_path='/SENTINEL.BIN;1')
    data=b'BOOT2 = cdrom0:\\HELLO.ELF;1\r\nVER = 1.00\r\nVMODE = NTSC\r\n'; iso.add_fp(io.BytesIO(data),len(data),iso_path='/SYSTEM.CNF;1')
    iso.add_file(str(OUT/'HELLO.ELF'),iso_path='/HELLO.ELF;1')
    iso.write(str(OUT/'probe.iso')); iso.close()
    cmd+=['-gameargs','--disc'+(' --badclut' if args.badclut else ''),str(OUT/'probe.iso')]
elif args.badclut:
    cmd+=['-gameargs','--badclut']
(OUT/(args.name+'-command.json')).write_text(json.dumps(cmd,indent=2))
def launch():
    return subprocess.Popen(cmd,startupinfo=startup,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
if args.audio:
    sys.path.insert(0,str(ROOT/'build/probe-python'))
    import soundcard as sc
    import numpy as np
    speaker=sc.default_speaker(); mic=sc.get_microphone(speaker.id,include_loopback=True)
    blocks=[]
    with mic.recorder(samplerate=48000,channels=2,blocksize=1024) as rec:
        blocks.append(rec.record(numframes=48000//2))
        p=launch(); deadline=time.monotonic()+25
        while p.poll() is None and time.monotonic()<deadline:
            blocks.append(rec.record(numframes=48000//4))
        blocks.append(rec.record(numframes=48000//2))
    samples=np.concatenate(blocks)
    with wave.open(str(OUT/(args.name+'-loopback.wav')),'wb') as wav:
        wav.setnchannels(2); wav.setsampwidth(2); wav.setframerate(48000)
        wav.writeframes((np.clip(samples,-1,1)*32767).astype('<i2').tobytes())
    window=24000; best=[]
    for channel,target in [(0,441),(1,220.5)]:
        candidates=[]
        for at in range(0,len(samples)-window+1,6000):
            chunk=samples[at:at+window,channel]
            spectrum=abs(np.fft.rfft(chunk*np.hanning(window)))
            freq=np.fft.rfftfreq(window,1/48000)
            mask=abs(freq-target)<6
            candidates.append((float(spectrum[mask].max()),float(np.sqrt(np.mean(chunk*chunk))),float(freq[mask][spectrum[mask].argmax()])))
        best.append(max(candidates))
    result={'speaker':speaker.name,'sample_rate':48000,'frames':len(samples),'channels':{'left':best[0],'right':best[1]},'meaning':'Each triple: target-band FFT amplitude, window RMS, peak Hz'}
    (OUT/(args.name+'-audio.json')).write_text(json.dumps(result,indent=2),encoding='utf-8')
    print(json.dumps(result,ensure_ascii=True))
else:
    p=launch()
try: code=p.wait(timeout=30)
except subprocess.TimeoutExpired:
    p.terminate(); p.wait(); raise RuntimeError('PCSX2 did not exit after poweroff')
log=(OUT/(args.name+'.log')).read_text(encoding='utf-8',errors='replace')
print('\n'.join(line for line in log.splitlines() if 'P0 ' in line or 'sceCdPowerOff' in line))
expected='P0 COMPLETE failures=1' if args.badclut else 'P0 COMPLETE failures=0'
if code or expected not in log or 'sceCdPowerOff called' not in log: raise SystemExit(1)
if args.badclut and 'mismatch=32000 pixels=64000' not in log: raise RuntimeError('CLUT negative control did not reject the expected 128 colors')
if args.audio and any(v[0]<1 or v[1]<1e-4 for v in best): raise RuntimeError('Loopback did not measure both test tones')
