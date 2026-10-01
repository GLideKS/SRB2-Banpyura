"""Actual-code packed/composite texture alignment and unchanged-host equivalence.

python tools/ps2/texture_hosttest.py --negative-controls
Fixtures: vanilla BBFLRWAL (1739 packed bytes), odd-sized synthetic holes and an
opaque patch forcing single-to-composite fallback. Compare pixels/post metadata,
not padding/pointers (normalized binary snapshots compared byte for byte); check alignment, allocation red zones, scratch frees and
cache reconstruction on x86 and x64. No PCSX2, PNG or blend-drawer emulation.
"""
import argparse
import json
from pathlib import Path
import struct
import subprocess
import zipfile

from blend_hosttest import ROOT, build, section


def texture_sections():
    text = (ROOT / "src/r_textures.c").read_text()
    renderer = section(text, "INT32 numtextures = 0;", "// Painfully simple texture id")
    renderer += section(text, "static void R_DrawColumnInCache(", "UINT8 *R_GetFlatForTexture(")
    renderer += section(text, "void R_CheckTextureCache(", "INT32 R_GetTextureNumForFlat(")
    data = (ROOT / "src/r_data.c").read_text()
    renderer += section(data, "INT32 ASTTextureBlendingThreshold", "// Blends a pixel for a texture patch.")
    patch = section((ROOT / "src/r_patch.c").read_text(), "patch_t *Patch_Create(", "// Frees patches with a tag range.")
    return renderer, patch


def doom_patch(width, height, lengths, top):
    columns = []
    for x, length in enumerate(lengths):
        pixels = bytes((17*x + y + 1) % 255 for y in range(length))
        columns.append(bytes([top, length, 0]) + pixels + b"\0\xff")
    offset = 8 + 4*width
    offsets = []
    for column in columns:
        offsets.append(offset)
        offset += len(column)
    return struct.pack("<hhhh", width, height, 0, 0) + struct.pack("<" + "I"*width, *offsets) + b"".join(columns)


def run_fixture(exe, work, name, data, packed, negative=False):
    snapshot = work / (name + ".bin")
    tested = subprocess.run([str(exe), str(snapshot)], input=struct.pack("<II", len(data), packed) + data, capture_output=True)
    stdout = tested.stdout.decode("ascii", errors="replace")
    stderr = tested.stderr.decode("ascii", errors="replace")
    (work / (name + ".log")).write_text(stdout + stderr, encoding="utf-8")
    if negative:
        if tested.returncode != 2 or "column_t alignment" not in stderr:
            raise RuntimeError(f"Alignment negative control failed for an unexpected reason: {stdout}{stderr}")
    elif tested.returncode:
        raise RuntimeError(f"Texture fixture failed ({work.name}/{name}): {stdout}{stderr}")
    return stdout, snapshot.read_bytes()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", type=Path, default=ROOT / "build/agent-equivalence-g1")
    ap.add_argument("--negative-controls", action="store_true")
    args = ap.parse_args()
    out = args.out.resolve()
    if not out.parent.is_dir():
        raise SystemExit(f"Output parent does not exist: {out.parent}")
    out.mkdir(exist_ok=True)
    renderer, patch = texture_sections()
    with zipfile.ZipFile(ROOT / "srb2-assets/srb2.pk3") as assets:
        vanilla = assets.read("Textures/Alpine Paradise/BBFLRWAL")
    fixtures = [("vanilla-BBFLRWAL", vanilla, 1), ("holes-3x5", doom_patch(3, 5, [1, 2, 2], 1), 1),
                ("holes-1x3", doom_patch(1, 3, [1], 1), 1), ("opaque-3x5", doom_patch(3, 5, [5, 5, 5], 0), 0)]
    source = ROOT / "tools/ps2/texture_hosttest.c"
    results = []
    for arch in ["x86", "x64"]:
        outputs = {}
        for reference in [True, False]:
            work = out / (("texture-reference-" if reference else "texture-profile-") + arch)
            work.mkdir(exist_ok=True)
            (work / "texture_renderer.inc").write_text(renderer, encoding="utf-8")
            (work / "texture_patch.inc").write_text(patch, encoding="utf-8")
            exe = build(work, "texture_hosttest", arch, [(source, "texture.obj", ["/DTEXTURE_REFERENCE"] if reference else [])])
            outputs[reference] = {name: run_fixture(exe, work, name, data, packed) for name, data, packed in fixtures}
        for name, _, _ in fixtures:
            if outputs[True][name] != outputs[False][name]:
                raise RuntimeError(f"Texture pixel/post equivalence differs: {arch}/{name}")
        results.append({"arch": arch, "fixtures": len(fixtures), "cases_per_fixture": 5, "differences": 0})
        print(f"texture-{arch}: 4 fixtures x 5 cases match unchanged host; alignment, bounds, scratch frees and reconstruction PASS")
    if args.negative_controls:
        old = ("\t*columnofs = (pixels + columnalign - 1) & ~(columnalign - 1);\n"
               "\t*postofs = (*columnofs + sizeof(column_t) * width + postalign - 1) & ~(postalign - 1);")
        new = "\t*columnofs = pixels;\n\t*postofs = pixels + sizeof(column_t) * width;"
        if renderer.count(old) != 1:
            raise RuntimeError("Texture layout mutation anchor changed")
        work = out / "negative-unaligned-texture-x64"
        work.mkdir(exist_ok=True)
        (work / "texture_renderer.inc").write_text(renderer.replace(old, new), encoding="utf-8")
        (work / "texture_patch.inc").write_text(patch, encoding="utf-8")
        exe = build(work, "texture_hosttest", "x64", [(source, "texture.obj", [])])
        run_fixture(exe, work, "vanilla-BBFLRWAL", vanilla, 1, True)
        print("negative-unaligned-texture-x64: original offsets fail column_t alignment on vanilla BBFLRWAL, as expected")
        results.append({"case": work.name, "negative_control": True, "exit": 2})
    (out / "texture-results.json").write_text(json.dumps(results, indent=2) + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()
