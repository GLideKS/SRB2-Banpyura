# Пины окружения — фаза 0

* SRB2: https://github.com/GLideKS/SRB2-Banpyura, vanilla-master,
  `0e09462308610005f640ed84b21ec8a4ef116a4b`. Чистый checkout из Git, ветка `ps2`.
* ps2dev: копия `D:\AI-projects\SONIC-PS2-NativePort\ps2dev` → `D:\ps2dev`; проверено 4267 файлов,
  каждый SHA256 совпал. Точный манифест: `build/ps2-hello/toolchain-manifest.json`.
* EE и IOP GCC: 15.2.0 (успешный запуск); PE32 machine 0x14c.
* DLL: из `D:/AI-projects/SONIC-PS2-NativePort/port/toolchain/win32-dll`,
  19 библиотек, все PE32 i386. SHA256: `build/ps2-hello/dll-manifest.json`.
  Добавлены рядом с bin/ и libexec/ в копии; оригинал не изменён.
* PATH сборки hello: только ee/bin, iop/bin, ps2dev/bin, Windows/System32, Windows.
* PCSX2: `D:/PCSX2/pcsx2-qt.exe`, SHA256 `474dc24acf00ee398183235a15c97934bf099c2fa458dcc0cb174750aebe3cff`.
  Версия и поведение подтверждаются логами запуска, не планом.
* PC: MSVC 14.50.35717, CMake 4.2.1; зависимости только для эталона
  из `D:/AI-projects/SRB2B-plus/build/deps/vcpkg_installed/x64-windows`.
  Установка по manifest не прошла (yasm); использована существующая сборка зависимостей.

Стек hello: 512 KiB. DMA/IO/PCM/readback/CLUT выровнены на 64 байта.
COP0 Count в логах — сырые приращения регистра, включая ожидания;
они не объявляются чистой стоимостью CPU или числом EE cycles.
