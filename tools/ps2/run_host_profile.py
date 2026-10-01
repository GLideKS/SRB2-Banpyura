"""Run all four native PS2_PROFILE demos using existing packs and golden clocks.

Outputs commands, logs, counts, hashes and byte comparisons under the isolated
host build. Golden files and packs are read-only inputs. A fresh output directory
is required, so reruns cannot silently mix old and new evidence.
"""
import argparse
import csv
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[2]
DEMOS = [f"DEMO_{i:03}" for i in range(1, 5)]


def digest(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def rows(path, delimiter=","):
    with path.open(newline="") as stream:
        return list(csv.DictReader(stream, delimiter=delimiter))


def compare(golden, candidate):
    names = ["tics.csv", "frames.csv", "soc.tsv", "sfx.csv", "complete.txt"]
    names += sorted(p.name for p in golden.glob("*.idx"))
    names += sorted(p.name for p in golden.glob("*.pcm"))
    differences = []
    hashes = {}
    for name in names:
        a, b = golden / name, candidate / name
        if not a.exists() or not b.exists():
            differences.append({"file": name, "reason": "missing file"})
            continue
        hashes[name] = {"golden": digest(a), "candidate": digest(b)}
        if hashes[name]["golden"] != hashes[name]["candidate"]:
            detail = {"file": name, "reason": "bytes differ"}
            if name == "tics.csv":
                ga, ca = rows(a), rows(b)
                detail["golden_rows"], detail["candidate_rows"] = len(ga), len(ca)
                for index, (left, right) in enumerate(zip(ga, ca), 1):
                    if left != right:
                        detail["first_row"] = index
                        detail["fields"] = {k: [left[k], right.get(k)] for k in left if left[k] != right.get(k)}
                        break
            differences.append(detail)
    for pattern in ("*.idx", "*.pcm"):
        if {p.name for p in golden.glob(pattern)} != {p.name for p in candidate.glob(pattern)}:
            differences.append({"file": pattern, "reason": "file set differs"})
    return {"compared_files": len(hashes), "differences": differences, "sha256": hashes}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", type=Path, default=ROOT / "build/host-profile-g1/bin/Release/srb2-host-profile-g1.exe")
    parser.add_argument("--golden", type=Path, default=ROOT / "golden/phase0-v2/run1")
    parser.add_argument("--output", type=Path, default=ROOT / "build/host-profile-g1/equivalence")
    parser.add_argument("--packs", type=Path, default=ROOT / "build/pak")
    parser.add_argument("--dependencies", type=Path, default=Path("D:/AI-projects/SRB2B-plus/build/deps/vcpkg_installed"))
    parser.add_argument("--timeout", type=int, default=240)
    args = parser.parse_args()
    exe, golden, base, packs = (p.resolve() for p in (args.exe, args.golden, args.output, args.packs))
    inputs = [exe] + [packs / name for name in ("SRB2.PAK", "ZONES.PAK", "CHARS.PAK", "MUSIC.PAK")]
    for demo in DEMOS:
        inputs += [golden.parent / (demo + ".lmp"), golden / demo / "command.json",
                   golden / demo / "home/srb2/reference.cfg"]
        inputs += [p for p in (golden / demo).iterdir() if p.is_file()]
    before = {str(p): digest(p) for p in inputs}
    base.mkdir(parents=True, exist_ok=False)
    report = {"executable": str(exe), "golden": str(golden), "packs": str(packs),
              "input_sha256": before, "demos": {}}
    (base / "report.json").write_text(json.dumps(report, indent=2))
    env = dict(os.environ, SRB2WADDIR=str(packs))
    env["PATH"] = str(args.dependencies / "x64-windows/bin") + os.pathsep + env.get("PATH", "")
    startup = None
    if os.name == "nt":
        startup = subprocess.STARTUPINFO()
        startup.dwFlags |= subprocess.STARTF_USESHOWWINDOW
        startup.wShowWindow = 0
    failed = False
    for demo in DEMOS:
        out = base / demo
        gamehome = out / "home/srb2"
        gamehome.mkdir(parents=True)
        reference = golden / demo
        shutil.copy2(reference / "home/srb2/reference.cfg", gamehome / "reference.cfg")
        shutil.copy2(golden.parent / (demo + ".lmp"), gamehome / (demo + ".lmp"))
        # Replay the exact recorded arguments, replacing only executable and
        # output/home paths. This retains -ps2ref's controlled clock and timedemo.
        command = json.loads((reference / "command.json").read_text())
        command[0] = str(exe)
        command[command.index("-ps2ref") + 1] = str(out)
        command[command.index("-home") + 1] = str(out / "home")
        (out / "command.json").write_text(json.dumps(command, indent=2))
        result = {"command": command, "cwd": str(packs),
                  "config": (gamehome / "reference.cfg").read_text()}
        report["demos"][demo] = result
        (base / "report.json").write_text(json.dumps(report, indent=2))
        print("RUN", demo, flush=True)
        with (out / "stdout.log").open("wb") as log:
            try:
                process = subprocess.Popen(command, cwd=packs, env=env, stdout=log,
                                           stderr=subprocess.STDOUT, startupinfo=startup)
                try:
                    result["exit_code"] = process.wait(timeout=args.timeout)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait()
                    result["error"] = "timeout"
                    result["exit_code"] = process.returncode
            except OSError as error:
                result["error"] = str(error)
        if result.get("exit_code") != 0 or not (out / "complete.txt").exists():
            result.setdefault("error", "run failed or completion marker missing")
            failed = True
        for name, separator in (("tics.csv", ","), ("frames.csv", ","), ("soc.tsv", "\t"), ("sfx.csv", ",")):
            if (out / name).exists():
                data = rows(out / name, separator)
                result[name + "_rows"] = len(data)
                if name == "tics.csv" and data:
                    result["end_leveltic"] = data[-1]["leveltic"]
        result["idx_files"] = len(list(out.glob("*.idx")))
        result["comparison"] = compare(reference, out)
        failed |= bool(result["comparison"]["differences"])
        report["demos"][demo] = result
        (base / "report.json").write_text(json.dumps(report, indent=2))
        print("DONE", demo, "exit", result.get("exit_code"),
              "tics", result.get("tics.csv_rows", 0), "idx", result["idx_files"],
              "differences", len(result["comparison"]["differences"]), flush=True)
    report["inputs_changed"] = [str(p) for p in inputs if digest(p) != before[str(p)]]
    failed |= bool(report["inputs_changed"])
    report["passed"] = not failed
    (base / "report.json").write_text(json.dumps(report, indent=2))
    print("Report:", base / "report.json", flush=True)
    return int(failed)


if __name__ == "__main__":
    raise SystemExit(main())
