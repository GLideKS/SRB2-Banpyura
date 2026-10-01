"""Run an ELF in PCSX2 under a machine-wide lock (only one emulator at a time).

usage: run_pcsx2.py --elf FILE --log FILE [--args "..."] [--timeout SEC]
                    [--unlimited] [--wait-for-exit | --until "TEXT"]
 - host: = the directory of the ELF (PCSX2 HLE host filesystem).
 - Exit code 0 only if PCSX2 exited by itself (poweroff/exit) within the timeout,
   or, with --until, the text appeared in the log (PCSX2 is then killed).
 - Prints the last lines of the log that start with the marker '--marker' (default none).
Other agents may be using the emulator: the lock file is waited for (max --lock-wait).
"""
import argparse
import os
import subprocess
import sys
import time
from pathlib import Path

LOCK = Path('C:/Users/loban/AppData/Local/Temp/pcsx2-run.lock')
PCSX2 = os.environ.get('SRB2_PCSX2', 'D:/PCSX2-test/pcsx2-qt.exe')  # private copy of D:/PCSX2 with ExtraMemory=false (real 32 MB)


def acquire(wait):
    end = time.time() + wait
    while True:
        try:
            fd = os.open(str(LOCK), os.O_CREAT | os.O_EXCL | os.O_WRONLY)
            os.write(fd, str(os.getpid()).encode())
            os.close(fd)
            return
        except FileExistsError:
            try:  # stale lock (owner gone or older than 15 min)
                if time.time() - LOCK.stat().st_mtime > 900:
                    LOCK.unlink()
                    continue
            except OSError:
                pass
            if time.time() > end:
                raise SystemExit('could not get the PCSX2 lock')
            time.sleep(1)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--elf', required=True)
    ap.add_argument('--log', required=True)
    ap.add_argument('--args', default='')
    ap.add_argument('--timeout', type=float, default=120)
    ap.add_argument('--lock-wait', type=float, default=900)
    ap.add_argument('--unlimited', action='store_true', help='BROKEN in PCSX2 2.6.3 here: the run hangs with an empty log; do not use')
    ap.add_argument('--until', default='')
    ap.add_argument('--until-file', default='', help='file (e.g. engine -logfile in the ELF dir) searched for --until instead of the PCSX2 log')
    ap.add_argument('--marker', default='')
    a = ap.parse_args()
    cmd = [PCSX2, '-portable', '-batch', '-nogui', '-fastboot', '-elf', str(Path(a.elf).resolve()),
           '-logfile', str(Path(a.log).resolve())]
    if a.unlimited:
        cmd.append('-unlimited')
    if a.args:
        cmd += ['-gameargs', a.args]
    Path(a.log).parent.mkdir(parents=True, exist_ok=True)
    acquire(a.lock_wait)
    for stale_file in (a.log, a.until_file):
        try:
            if stale_file:
                Path(stale_file).unlink()  # --until must not match a stale log
        except OSError:
            pass
    code = 1
    try:
        si = subprocess.STARTUPINFO()
        si.dwFlags |= subprocess.STARTF_USESHOWWINDOW
        si.wShowWindow = 0
        p = subprocess.Popen(cmd, startupinfo=si, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        end = time.time() + a.timeout
        seen = False
        while time.time() < end:
            if p.poll() is not None:
                code = p.returncode
                break
            if a.until:
                try:
                    if a.until in Path(a.until_file or a.log).read_text(errors='replace'):
                        seen = True
                        break
                except OSError:
                    pass
            time.sleep(0.5)
        if p.poll() is None:
            # polite close first so PCSX2 flushes its log file
            subprocess.run(['taskkill', '/PID', str(p.pid)], capture_output=True)
            try:
                p.wait(8)
            except subprocess.TimeoutExpired:
                pass
        if p.poll() is None:
            p.terminate()
            try:
                p.wait(10)
            except subprocess.TimeoutExpired:
                p.kill()
        if seen:
            code = 0
        elif time.time() >= end:
            code = 2  # a forced close is not successful completion
        elif p.poll() is not None:
            code = p.returncode
    finally:
        try:
            LOCK.unlink()
        except OSError:
            pass
    text = Path(a.log).read_text(errors='replace') if Path(a.log).exists() else ''
    if a.marker:
        print('\n'.join(l for l in text.splitlines() if a.marker in l))
    print('pcsx2 exit code', code, '(2 = timeout)')
    return code


if __name__ == '__main__':
    sys.exit(main())
