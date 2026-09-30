"""Record actual hashes; never derive a version from an assumed plan entry."""
import hashlib, json, struct, subprocess
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2]
DEV=Path('D:/ps2dev')
SOURCE=Path('D:/AI-projects/SONIC-PS2-NativePort/ps2dev')
def sha(p): return hashlib.file_digest(p.open('rb'),'sha256').hexdigest()
def pe(p):
    b=p.read_bytes(); off=struct.unpack_from('<I',b,0x3c)[0]
    return hex(struct.unpack_from('<H',b,off+4)[0])
files=[]
for p in sorted(SOURCE.rglob('*')):
    if p.is_file():
        q=DEV/p.relative_to(SOURCE)
        files.append({'path':p.relative_to(SOURCE).as_posix(),'sha256':sha(q),'source_identical':sha(p)==sha(q)})
out=ROOT/'build/ps2-hello'; out.mkdir(parents=True,exist_ok=True)
(out/'toolchain-manifest.json').write_text(json.dumps(files,indent=2))
dlls=[{'path':p.name,'machine':pe(p),'sha256':sha(p)} for p in sorted((DEV/'ee/bin').glob('*.dll'))]
(out/'dll-manifest.json').write_text(json.dumps(dlls,indent=2))
assert all(f['source_identical'] for f in files)
assert all(f['machine']=='0x14c' for f in dlls)
pin=subprocess.check_output(['git','rev-parse','upstream/vanilla-master'],cwd=ROOT,text=True).strip()
(ROOT/'docs/TOOLCHAIN.md').write_text(f'''# Пины окружения — фаза 0

* SRB2: https://github.com/GLideKS/SRB2-Banpyura, vanilla-master,
  `{pin}`. Чистый checkout из Git, ветка `ps2`.
* ps2dev: копия `{SOURCE}` → `{DEV}`; проверено {len(files)} файлов,
  каждый SHA256 совпал. Точный манифест: `build/ps2-hello/toolchain-manifest.json`.
* EE и IOP GCC: 15.2.0 (успешный запуск); PE32 machine 0x14c.
* DLL: из `D:/AI-projects/SONIC-PS2-NativePort/port/toolchain/win32-dll`,
  {len(dlls)} библиотек, все PE32 i386. SHA256: `build/ps2-hello/dll-manifest.json`.
  Добавлены рядом с bin/ и libexec/ в копии; оригинал не изменён.
* PATH сборки hello: только ee/bin, iop/bin, ps2dev/bin, Windows/System32, Windows.
* PCSX2: `D:/PCSX2/pcsx2-qt.exe`, SHA256 `{sha(Path('D:/PCSX2/pcsx2-qt.exe'))}`.
  Версия и поведение подтверждаются логами запуска, не планом.
* PC: MSVC 14.50.35717, CMake 4.2.1; зависимости только для эталона
  из `D:/AI-projects/SRB2B-plus/build/deps/vcpkg_installed/x64-windows`.
  Установка по manifest не прошла (yasm); использована существующая сборка зависимостей.

Стек hello: 512 KiB. DMA/IO/PCM/readback/CLUT выровнены на 64 байта.
COP0 Count в логах — сырые приращения регистра, включая ожидания;
они не объявляются чистой стоимостью CPU или числом EE cycles.
''',encoding='utf-8')
print('Verified source copy:',len(files),'files; i686 DLLs:',len(dlls))
