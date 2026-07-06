# Sonic Robo Blast 2 Banpyura

<img width="215" height="215" alt="srb2" src="https://github.com/user-attachments/assets/2ef6dbae-e755-4e60-b20b-9d4cda4466ae" />

SRB2 Banpyura is a custom netgame compatible Sonic Robo Blast 2 build that includes a plethora of fixes, improvements, and new features fully based on contributions and other changes the community made, but still holding modding accuracy with SRB2 Vanilla.

Icon credits goes to ClovesCloestarSRB2 and his addon [New Springs](https://mb.srb2.org/addons/new-springs.8942/) from the base spring.

## Features

### HUD/Menus

- Toggle screen fades (`wipes`). Not effective in Marathon Run.
- Lots of chat tweaks! Window positioning, snapping, copy-paste, selection, customizable opacity, and more.
- Menu tweaks! background color, text case switching, selection color.
- Extra camera options: Exact aiming, Clipping style, Gamepad Camera sensitivity
- Addons menu has it's background translucent and their text on thin font.
- A progress bar now displays when checking server files. (SRB2 v2.2.16 Nightly)
- Inverted crosshairs option (SRB2-edit)
- Player names on the rankings hud always use a thin font
- Servers on the Master Server list has a background (SRB2-Edit)
- Server information display! shows players, new download confirmation, addon listing, current map. (Lugent)
- "Quit Game" and "Abort" red colored
- OpenGL's black console color to be a proper black and gray is an color option (SRB2 v2.2.16 Nightly)
- `cecho` messages now prints on console.

### Visuals

- `renderhitbox` usable in multiplayer.
- OpenGL Light Dithering, smooth light ramps even for Palette rendering! (SRB2-Classic)
- Splat interpolation (SRB2-edit)
- Added 1360x768 resolution (personal use)
- `r_secbright` Configurable minimum sector brightress (SRB2-Legacy)

### Gameplay / Netplay

- `addfilelocal` allows you to load addons locally. Can be accessed via Addons Menu and pressing Right Alt as well. (SRB2 Kart Saturn)
- Fixed SRB2's loading time. (SRB2-Classic)
- Minimum input delay and Gentleman's delay (Ring Racers)
- `cam_centertoggle` and `cam2_centertoggle` are not exclusive to the Automatic playstyle.

### Lua

- Added `CameraThinker` hook to alter the camera's behavior.
- - `player` and `camera` are passed in as arguments. Return `true` to override vanilla camera movement.

### Miscellaneous

- Improved GIF recording.
- The game now goes to the title or drops a warning instead of crashing on the following situations: `Invalid sector number %u from server`, `Invalid line number %u from server`, `Savegame corrupted`, `polyobj count inconsistency`, SOCK_Send errors.
- `saveaddons` saves the current addon order to be loaded as a console script. Useful for quickly testing addon lists locally for servers.
- Don't reset chasecam and do not run special stage wipe on resync (Jisk, Lugent)
- Fixed chats not being saved on the logs in Linux systems
- Removed 5-second delay upon disconnecting from servers
- Fixed nameless servers causing other servers to not display.
- The `help` command is now sorted by origin (Vanilla, Banpyura, Addon). You can use `-v`, `-c`, and `-a` respectively to only print out only those sections. (You can use more than one flag at once).
- Doing merely `<cvar>` has the same effect as `help <cvar>`.
- Console variable information is more in-depth.
- - You can choose to hide certain sections via `cvarinfo` ("Show All" by default).
- See the amount of freeslots using `freeslots`.

**Most of these options can be found in the menu under Banpyura Options....**

# Installation:

- You can either use the Github Actions build (go to the Actions tab on the top, then select the one that corresponds to your platform; x64 Windows builds require - different DLLs)
- Compile from the source code.
- [Download the latest build from nightly.link (x86)](https://nightly.link/GLideKS/SRB2-Banpyura/workflows/windows-x86-makefile/vanilla-master?preview)

# Screenshots / GIFs

<img width="640" height="512" alt="image" src="https://github.com/user-attachments/assets/de0b15e8-b711-4aae-92bf-809005a2b70a" />
<img width="640" height="400" alt="srb20114" src="https://github.com/user-attachments/assets/ee678a31-d8b2-4743-9474-9fb6d1b1f0f8" />
<img width="640" height="400" alt="srb20113" src="https://github.com/user-attachments/assets/c0ed8a7a-bd52-49b0-90fd-425d982f1ae1" />
<img width="320" height="200" alt="srb20230" src="https://github.com/user-attachments/assets/43fdb009-b300-4e98-ac41-c60644c2f648" />
<img width="320" height="200" alt="srb20229" src="https://github.com/user-attachments/assets/ac1ff16b-54de-4803-822a-f866f575c1a9" />
<img width="320" height="200" alt="srb20228" src="https://github.com/user-attachments/assets/b2b46a4d-93d2-431c-822f-f6e1c6dc1872" />
<img width="320" height="200" alt="srb20226" src="https://github.com/user-attachments/assets/d49162f1-b5d9-4339-b92c-f1cfc147a304" />
<img width="320" height="256" alt="srb20225" src="https://github.com/user-attachments/assets/13dfa657-d72a-463f-87b5-c832e663fdad" />
<img width="320" height="256" alt="srb20224" src="https://github.com/user-attachments/assets/c1b8e0cf-6a57-40b8-b5a9-fa04c5d953e9" />

## Dependencies
- SDL2 (Linux/OS X only)
- SDL2-Mixer (Linux/OS X only)
- libupnp (Linux/OS X only)
- libgme (Linux/OS X only)
- libopenmpt (Linux/OS X only)

## Compiling

See [SRB2 Wiki/Source code compiling](http://wiki.srb2.org/wiki/Source_code_compiling)

## Disclaimer
Sonic Team Junior is in no way affiliated with SEGA or Sonic Team. We do not claim ownership of any of SEGA's intellectual property used in SRB2.
