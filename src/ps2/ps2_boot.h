// PS2 start-up: IOP modules, arguments, storage roots, exit. Called from main() before D_SRB2Main.
#ifndef PS2_BOOT_H
#define PS2_BOOT_H

#include "../doomtype.h"

#define PS2BOOT_PATHMAX 256

typedef struct
{
	boolean iopreset;               // IOP was rebooted by us
	INT32 sio2man, padman;          // IOP module ids, 0 = not loaded, < 0 = failed
	INT32 iomanx, fileXio;
	INT32 poweroff, cdfs;
	boolean poweroff_ok;            // poweroff.irx + EE library are up
	boolean host;                   // booted from host: (PCSX2 -elf / ps2link): dev environment
	char bootpath[PS2BOOT_PATHMAX]; // where the program came from (argv[0] or the working directory)
	char datadir[PS2BOOT_PATHMAX];  // read-only game data root (srb2.pk3, ...): no trailing slash
	char homedir[PS2BOOT_PATHMAX];  // writable root, the engine's "$HOME"; srb2home = homedir/DEFAULTDIR
} ps2boot_info_t;

extern ps2boot_info_t ps2boot;

// Brings the system up and rewrites argc/argv: a program name is always argv[0]; command line
// arguments follow, then the lines of <datadir>/ps2args (one argument per line, '#' comments).
// Flags read from the command line before anything else: -iopreset, -noiopreset, -nopoweroff.
void PS2Boot_Init(int *argc, char ***argv);

// true once the console's power button was pressed (the poweroff callback runs in another thread)
boolean PS2Boot_PowerRequested(void);

// Final exit: flush stdio, then power off (host: boots, or -poweroff) or return to the loader.
void PS2Boot_Exit(INT32 code) FUNCNORETURN;

#endif
