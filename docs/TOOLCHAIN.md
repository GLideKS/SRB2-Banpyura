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

## Проверка каркаса G1 (2026-10-01)

* Запуски используют существующую private portable-копию
  `D:/PCSX2-test/pcsx2-qt.exe`: SHA256 **совпадает** с `D:/PCSX2`:
  `474dc24acf00ee398183235a15c97934bf099c2fa458dcc0cb174750aebe3cff`.
  Версия из launch-log — 2.6.3. Конфиг: ExtraMemory=false (32 МБ), HostFs=true,
  EECycleRate=0, EECycleSkip=0, EnableEECache=false. Тулчейн/оригинальные
  ассеты/конфиг `D:/PCSX2` в этой сессии не изменялись.
* LZ4 SDK-header — 1.10.0 (`LZ4_VERSION_{MAJOR,MINOR,RELEASE}`). Хостовые
  тесты/профиль собирают существующий внешний `lz4.c` из
  `build/scratch/lz4src/lz4-4.4.5/lz4libs/`, EE — SDK liblz4.
  Команды и пути зависимости: `docs/GATES/g1/host-profile/commands-20261001-150235.json`.
* Host-profile binary SHA256:
  `20873a6358abfe1dd2b43a3bf38c9a9eae654364b083aec5ff846041eab542c1`.
  `SRB2_CONFIG_PS2PROFILE=ON`, SDL только на хосте, без Lua-VM/сокетов.
* SHA256 ELF, пака, исходных pk3, эмулятора, конфигурации и source snapshot:
  `docs/GATES/g1/report.json`. Полный гейт пока красный:
  `docs/GATES/G1.md`.
