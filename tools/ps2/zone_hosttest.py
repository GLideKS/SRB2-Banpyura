"""Run the actual PS2 zone allocator on x86 and the native host profile on x64; no PCSX2.

usage: python tools/ps2/zone_hosttest.py [--out build/agent-zone-g1] [--negative-controls]
Logs and executables stay in the isolated output directory. Engine sources are not rewritten.
"""
import argparse
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
VCVARS = Path(r"C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvarsall.bat")


def run_case(out, name, arch, defines, expect_failure=False):
    work = out / name
    work.mkdir(exist_ok=True)
    cmd = ["cl", "/nologo", "/std:c17", "/O2", "/W4", "/WX", "/D_CRT_SECURE_NO_WARNINGS",
           "/DPARANOIA", *defines, "/Fe:zone_hosttest.exe", str(ROOT / "tools/ps2/zone_hosttest.c")]
    bat = work / "build.bat"
    bat.write_text(f'@echo off\ncall "{VCVARS}" {arch} >nul 2>&1\nif errorlevel 1 exit /b 1\n'
                   + subprocess.list2cmdline(cmd) + "\n", encoding="utf-8")
    built = subprocess.run(["cmd", "/c", str(bat)], cwd=work, capture_output=True, text=True,
                           encoding="oem", errors="replace")
    (work / "build.log").write_text(built.stdout + built.stderr, encoding="utf-8")
    if built.returncode:
        print(built.stdout + built.stderr)
        raise RuntimeError(f"{name}: build failed")
    tested = subprocess.run([str(work / "zone_hosttest.exe")], cwd=work, capture_output=True, text=True)
    (work / "test.log").write_text(tested.stdout + tested.stderr, encoding="utf-8")
    print(f"{name}: exit={tested.returncode}")
    print(tested.stdout + tested.stderr, end="")
    if (tested.returncode != 0) != expect_failure:
        raise RuntimeError(f"{name}: unexpected test result")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default=str(ROOT / "build/agent-zone-g1"))
    ap.add_argument("--negative-controls", action="store_true")
    args = ap.parse_args()
    out = Path(args.out).resolve()
    if not out.parent.is_dir():
        raise SystemExit(f"Output parent does not exist: {out.parent}")
    out.mkdir(exist_ok=True)
    run_case(out, "ps2-x86", "x86", [])
    run_case(out, "host-profile", "x64", ["/DZONE_HOST_NATIVE"])
    if args.negative_controls:
        original = (ROOT / "src/z_zone.c").read_text()
        mutations = {
            "accounting-drift": ("zused -= *(size_t *)block->raw;",
                                 "zused -= (block->size - sizeof (memblock_t)) + Z_BLOCK_OVERHEAD;"),
            "ignored-alignment": ("void *raw = xm(sizeof (memblock_t) + size, align);",
                                  "align = minimum;\n\t\tvoid *raw = xm(sizeof (memblock_t) + size, align);"),
            "locked-eviction": ("if (!zpurgelock && (charge > limit || zused > limit - charge))",
                                "if (charge > limit || zused > limit - charge)"),
        }
        for name, (old, new) in mutations.items():
            if original.count(old) != 1:
                raise RuntimeError(f"Mutation anchor changed: {name}")
            source = out / (name + ".c")
            modified = original.replace(old, new)
            if name == "locked-eviction":
                modified = modified.replace("if (zpurgelock)\n\t\treturn;", "if (0)\n\t\treturn;", 1)
            source.write_text(modified, encoding="utf-8")
            run_case(out, "negative-" + name, "x86",
                     ["/I" + str(ROOT / "src"), '/DZONE_HOST_SOURCE="' + source.as_posix() + '"'], True)
    print("All requested zone host tests passed (negative controls failed as expected).")


if __name__ == "__main__":
    main()
