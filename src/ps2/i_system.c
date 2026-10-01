// SONIC ROBO BLAST 2 - PS2 platform layer: time, errors, quit, storage roots, environment.
// Logic follows src/sdl/i_system.c; the SDL, console, signal and mumble parts do not exist here.
#include <kernel.h>
#include <timer.h>
#include <delaythread.h>
#include <malloc.h>

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "../doomdef.h"
#include "../m_misc.h"
#include "../i_time.h"
#include "../i_video.h"
#include "../i_sound.h"
#include "../i_system.h"
#include "../i_threads.h"
#include "../g_game.h"
#include "../g_demo.h"
#include "../doomstat.h"
#include "../m_argv.h"
#include "../m_menu.h"
#include "../m_cond.h"
#include "../w_wad.h"
#include "../z_zone.h"
#include "../r_fps.h"
#include "../netcode/d_clisrv.h"

#include "ps2_boot.h"
#include "ps2_sys.h"

#define WADKEYWORD1 "srb2.pk3"
#define PS2_PRECISION ((UINT64)kBUSCLK) // GetTimerSystemTime ticks per second (147 456 000)

FILE *logstream = NULL;
char logfilename[1024];

UINT8 graphics_started __attribute__((weak)) = 0; // the video layer normally owns this
UINT8 keyboard_started = 0;

static boolean textinputmode = false;

// ---------------------------------------------------------------------------------------------
// Output
// ---------------------------------------------------------------------------------------------

// -memtrace: after every boot-step line ("Xxx()...", "Added file") print time and memory use.
static void MemTrace(const char *txt, int len)
{
	static boolean busy;
	struct mallinfo mi;
	char line[160];
	int n;

	if (busy || len < 4 || !M_CheckParm("-memtrace"))
		return;
	if (!(strstr(txt, "()...") || strncmp(txt, "Added file", 10) == 0))
		return;
	busy = true;
	mi = mallinfo();
	n = snprintf(line, sizeof line, "[mem] t=%.2fs heap_in_use=%luK zone_static=%luK zone_cache=%luK\n",
		(double)GetTimerSystemTime() / 147456000.0, (unsigned long)(mi.uordblks >> 10),
		(unsigned long)(Z_TagUsage(PU_STATIC) >> 10), (unsigned long)(Z_TagsUsage(PU_PURGELEVEL, INT32_MAX) >> 10));
	fwrite(line, 1, (size_t)n, stdout);
	if (logstream)
		fwrite(line, 1, (size_t)n, logstream);
	busy = false;
}

// EE stdout goes to the emulator log (PCSX2 prints each line); with -logfile also to a file.
void I_OutputMsg(const char *fmt, ...)
{
	char stackbuf[1024];
	char *txt = stackbuf;
	va_list argptr;
	int len;

	va_start(argptr, fmt);
	len = vsnprintf(stackbuf, sizeof stackbuf, fmt, argptr);
	va_end(argptr);
	if (len <= 0)
		return;

	if ((size_t)len >= sizeof stackbuf)
	{
		txt = malloc((size_t)len + 1);
		if (txt)
		{
			va_start(argptr, fmt);
			vsnprintf(txt, (size_t)len + 1, fmt, argptr);
			va_end(argptr);
		}
		else
		{
			txt = stackbuf;
			len = (int)sizeof stackbuf - 1;
		}
	}

	fwrite(txt, 1, (size_t)len, stdout);
	fflush(stdout);
	if (logstream)
	{
		fwrite(txt, 1, (size_t)len, logstream);
		fflush(logstream);
	}

	MemTrace(txt, len);

	if (txt != stackbuf)
		free(txt);
}

// ---------------------------------------------------------------------------------------------
// Time. GetTimerSystemTime is the kernel's 64-bit bus-clock counter: no wrap handling needed.
// ---------------------------------------------------------------------------------------------

precise_t I_GetPreciseTime(void)
{
	return GetTimerSystemTime();
}

UINT64 I_GetPrecisePrecision(void)
{
	return PS2_PRECISION;
}

static UINT32 frame_rate;
static double frame_frequency;
static UINT64 frame_epoch;
static double elapsed_frames;

static void I_InitFrameTime(const UINT64 now, const UINT32 cap)
{
	frame_rate = cap;
	frame_epoch = now;

	if (frame_rate == 0)
	{
		// Shouldn't be used, but just in case...?
		frame_frequency = 1.0;
		return;
	}

	frame_frequency = PS2_PRECISION / (double)frame_rate;
}

double I_GetFrameTime(void)
{
	const UINT64 now = I_GetPreciseTime();
	const UINT32 cap = R_GetFramerateCap();

	if (cap != frame_rate)
		I_InitFrameTime(now, cap);

	if (frame_rate == 0)
		elapsed_frames += 1.0; // always advance a frame
	else
		elapsed_frames += (now - frame_epoch) / frame_frequency;

	frame_epoch = now; // moving epoch
	return elapsed_frames;
}

void I_StartupTimer(void)
{
	I_InitFrameTime(0, R_GetFramerateCap());
	elapsed_frames = 0.0;
}

void I_Sleep(UINT32 ms)
{
	if (ms)
		DelayThread((s32)(ms * 1000));
}

// Thread sleep for all but the last millisecond, spin for the rest.
void I_SleepDuration(precise_t duration)
{
	const precise_t dest = I_GetPreciseTime() + duration;
	const INT64 slack = (INT64)(PS2_PRECISION / 1000);
	precise_t cur = I_GetPreciseTime();

	// two's complement: the counter may wrap
	while ((INT64)(dest - cur) > slack)
	{
		// 147.456 ticks per microsecond: dividing by 148 never oversleeps
		UINT64 us = (UINT64)((INT64)(dest - cur) - slack) / 148;
		if (us == 0)
			break;
		if (us > 100000)
			us = 100000;
		DelayThread((s32)us);
		cur = I_GetPreciseTime();
	}

	while ((INT64)(dest - cur) > 0)
		cur = I_GetPreciseTime();
}

// ---------------------------------------------------------------------------------------------
// Events
// ---------------------------------------------------------------------------------------------

void I_GetEvent(void) __attribute__((weak));
void I_GetEvent(void)
{
	// no window system: nothing besides the pads, which I_OsPolling reads
}

void I_OsPolling(void)
{
	PS2Joy_Poll();
	I_GetEvent();

	// the console's power button: the callback only sets a flag, quit from the game's own thread
	if (PS2Boot_PowerRequested())
		I_Quit();

	// no keyboard: modifier state is always clear
	shiftdown = ctrldown = altdown = 0;
	capslock = false;
}

INT32 I_GetKey(void)
{
	// Warning: I_GetKey empties the event queue till next keypress
	event_t *ev;
	INT32 rc = 0;

	// return the first keypress from the event queue
	for (; eventtail != eventhead; eventtail = (eventtail+1)&(MAXEVENTS-1))
	{
		ev = &events[eventtail];
		if (ev->type == ev_keydown || ev->type == ev_console)
		{
			rc = ev->key;
			continue;
		}
	}

	return rc;
}

void I_StartupMouse(void) {}
void I_StartupMouse2(void) {}
void I_GetMouseEvents(void) {}
void I_UpdateMouseGrab(void) {}
void I_SetMouseGrab(boolean grab) { (void)grab; }

void I_GetCursorPosition(INT32 *x, INT32 *y)
{
	if (x) *x = 0;
	if (y) *y = 0;
}

void I_SetTextInputMode(boolean active)
{
	textinputmode = active; // no keyboard yet: only the flag
}

boolean I_GetTextInputMode(void)
{
	return textinputmode;
}

/**	\brief empty ticcmd for player 1
*/
static ticcmd_t emptycmd;

ticcmd_t *I_BaseTiccmd(void)
{
	return &emptycmd;
}

/**	\brief empty ticcmd for player 2
*/
static ticcmd_t emptycmd2;

ticcmd_t *I_BaseTiccmd2(void)
{
	return &emptycmd2;
}

// ---------------------------------------------------------------------------------------------
// Threads: single-threaded engine, the same no-op set as a build without thread support.
// ---------------------------------------------------------------------------------------------

int I_can_thread(void) { return 0; }
void I_start_threads(void) {}
void I_stop_threads(void) {}
int I_spawn_thread(const char *name, I_thread_fn fn, void *userdata) { (void)name; (void)fn; (void)userdata; return 0; }
int I_thread_is_stopped(void) { return 0; }
void I_lock_mutex(I_mutex *anchor) { (void)anchor; }
void I_unlock_mutex(I_mutex id) { (void)id; }
void I_hold_cond(I_cond *cond_anchor, I_mutex mutex_id) { (void)cond_anchor; (void)mutex_id; }
void I_wake_one_cond(I_cond *cond_anchor) { (void)cond_anchor; }
void I_wake_all_cond(I_cond *cond_anchor) { (void)cond_anchor; }

// ---------------------------------------------------------------------------------------------
// Quit and errors
// ---------------------------------------------------------------------------------------------

/**	\brief quit function table
*/
static quitfuncptr quit_funcs[MAX_QUIT_FUNCS]; /* initialized to all bits 0 */

void I_AddExitFunc(void (*func)())
{
	INT32 c;

	for (c = 0; c < MAX_QUIT_FUNCS; c++)
	{
		if (!quit_funcs[c])
		{
			quit_funcs[c] = func;
			break;
		}
	}
}

void I_RemoveExitFunc(void (*func)())
{
	INT32 c;

	for (c = 0; c < MAX_QUIT_FUNCS; c++)
	{
		if (quit_funcs[c] == func)
		{
			while (c < MAX_QUIT_FUNCS-1)
			{
				quit_funcs[c] = quit_funcs[c+1];
				c++;
			}
			quit_funcs[MAX_QUIT_FUNCS-1] = NULL;
			break;
		}
	}
}

INT32 I_StartupSystem(void)
{
	I_start_threads();
	I_AddExitFunc(I_stop_threads);
	return 0;
}

// Closes everything registered with I_AddExitFunc (in reverse order) and the log file.
void I_ShutdownSystem(void)
{
	INT32 c;

	for (c = MAX_QUIT_FUNCS-1; c >= 0; c--)
		if (quit_funcs[c])
			(*quit_funcs[c])();

	if (logstream)
	{
		I_OutputMsg("I_ShutdownSystem(): end of logstream.\n");
		fclose(logstream);
		logstream = NULL;
	}
}

static void I_ShutdownInput(void)
{
	PS2Joy_Shutdown();
}

void I_Quit(void)
{
	static boolean quiting = false;

	/* prevent recursive I_Quit() */
	if (quiting) goto death;
	quiting = true;

	M_SaveConfig(NULL); //save game config, cvars..
	G_SaveGameData(clientGamedata); // Tails 12-08-2002
	//added:16-02-98: when recording a demo, should exit using 'q' key,
	//        but sometimes we forget and use 'F10'.. so save here too.

	if (demorecording)
		G_CheckDemoStatus();
	if (metalrecording)
		G_StopMetalRecording(false);
	if (moviemode)
		M_StopMovie();

	D_QuitNetGame();
	M_FreePlayerSetupColors();
	I_ShutdownMusic();
	I_ShutdownSound();
	I_ShutdownGraphics();
	I_ShutdownInput();
	I_ShutdownSystem();
	if (myargmalloc)
		free(myargv); // Deallocate allocated memory
death:
	W_Shutdown();
	PS2Boot_Exit(0);
}

void I_WaitVBL(INT32 count)
{
	(void)count;
	I_Sleep(1);
}

void I_BeginRead(void) {}
void I_EndRead(void) {}

/**	\brief phuck recursive errors
*/
static INT32 errorcount = 0;

/**	\brief recursive error detecting
*/
static boolean shutdowning = false;

void I_Error(const char *error, ...)
{
	va_list argptr;
	char buffer[8192];

	// recursive error detecting
	if (shutdowning)
	{
		errorcount++;
		// try to shutdown each subsystem separately
		if (errorcount == 2)
			I_ShutdownMusic();
		if (errorcount == 3)
			I_ShutdownSound();
		if (errorcount == 4)
			I_ShutdownGraphics();
		if (errorcount == 5)
			I_ShutdownInput();
		if (errorcount == 6)
			I_ShutdownSystem();
		if (errorcount == 8)
		{
			M_SaveConfig(NULL);
			G_SaveGameData(clientGamedata);
		}
		if (errorcount > 20)
		{
			va_start(argptr, error);
			vsnprintf(buffer, sizeof buffer, error, argptr);
			va_end(argptr);
			printf("I_Error(): recursive error: %s\n", buffer);
			W_Shutdown();
			PS2Boot_Exit(-1); // recursive errors detected
		}
	}

	shutdowning = true;

	// Display error message in the console before we start shutting it down
	va_start(argptr, error);
	vsnprintf(buffer, sizeof buffer, error, argptr);
	va_end(argptr);
	I_OutputMsg("\nI_Error(): %s\n", buffer);
	// ---

	M_SaveConfig(NULL); // save game config, cvars..
	G_SaveGameData(clientGamedata); // Tails 12-08-2002

	// Shutdown. Here might be other errors.
	if (demorecording)
		G_CheckDemoStatus();
	if (metalrecording)
		G_StopMetalRecording(false);
	if (moviemode)
		M_StopMovie();

	D_QuitNetGame();
	M_FreePlayerSetupColors();
	I_ShutdownMusic();
	I_ShutdownSound();
	I_ShutdownGraphics();

	// On hardware the player must be able to read it: draw the message and wait for Start.
	// Under host: (emulator, CI) the log has it and the run has to end by itself.
	if (PS2Video_FatalError)
	{
		PS2Video_FatalError(buffer);
		if (!ps2boot.host)
			PS2Joy_WaitStart();
	}

	I_ShutdownInput();
	I_ShutdownSystem();

	W_Shutdown();
	PS2Boot_Exit(-1);
}

// ---------------------------------------------------------------------------------------------
// Memory, random, storage roots, environment
// ---------------------------------------------------------------------------------------------

extern char _stack_size[]; // linker symbol: value of -Wl,--defsym,_stack_size

// total = EE RAM; free = unused heap + free blocks inside it (the heap ends below the stack)
size_t I_GetFreeMem(size_t *total)
{
	const size_t ram = (size_t)GetMemorySize();
	const size_t stack = (size_t)_stack_size < ram ? (size_t)_stack_size : 0; // -1 = "not set" in crt0
	const size_t limit = ram - stack;
	const size_t brk = (size_t)sbrk(0);
	struct mallinfo mi = mallinfo();

	if (total)
		*total = ram;
	return (limit > brk ? limit - brk : 0) + (size_t)mi.fordblks;
}

// no /dev/urandom: mix the clocks and the calendar through splitmix64
size_t I_GetRandomBytes(char *destination, size_t count)
{
	static UINT64 counter;
	UINT64 x = GetTimerSystemTime() ^ ((UINT64)cpu_ticks() << 32) ^ ((UINT64)time(NULL) << 16) ^ (++counter * 0x9e3779b97f4a7c15ULL);
	size_t i;

	for (i = 0; i < count; i += 8)
	{
		UINT64 z = (x += 0x9e3779b97f4a7c15ULL);
		size_t n = count - i < 8 ? count - i : 8;
		z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
		z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
		z ^= z >> 31;
		memcpy(destination + i, &z, n);
	}
	return count;
}

void I_GetDiskFreeSpace(INT64 *freespace)
{
	*freespace = 1024*1024*1024; // not queried; only the (absent) addon downloads ask
}

char *I_GetUserName(void)
{
	static char username[MAXPLAYERNAME+1];
	char *p = I_GetEnv("USER");

	if (!p)
		return NULL;
	strncpy(username, p, MAXPLAYERNAME);
	if (strcmp(username, "") != 0)
		return username;
	return NULL;
}

// Creates the missing parents too: srb2home may not exist yet on a fresh device.
INT32 I_mkdir(const char *dirname, INT32 unixright)
{
	char tmp[PS2BOOT_PATHMAX];
	char *p = strchr(dirname, ':');

	strlcpy(tmp, dirname, sizeof tmp);
	for (p = tmp + (p ? p - dirname + 1 : 0); *p; p++)
	{
		if (*p == '/' && p > tmp && p[-1] != ':' && p[-1] != '/')
		{
			*p = '\0';
			mkdir(tmp, unixright);
			*p = '/';
		}
	}
	return mkdir(dirname, unixright);
}

// HOME is the writable storage root chosen by PS2Boot_Init: srb2home = HOME/DEFAULTDIR (D_Home)
char *I_GetEnv(const char *name)
{
	if (!strcmp(name, "HOME"))
		return ps2boot.homedir;
	return getenv(name);
}

INT32 I_PutEnv(char *variable)
{
	return putenv(variable);
}

// No system clipboard: a 255-byte buffer that the console and chat fields share.
static char clipboard[256];

INT32 I_ClipboardCopy(const char *data, size_t size)
{
	if (size > 255)
		size = 255;
	memcpy(clipboard, data, size);
	clipboard[size] = 0;
	return 0;
}

const char *I_ClipboardPaste(void)
{
	static char clipboard_modified[256];
	char *i = clipboard_modified;

	if (!clipboard[0])
		return NULL;

	strlcpy(clipboard_modified, clipboard, sizeof clipboard_modified);
	while (*i)
	{
		if (*i == '\n' || *i == '\r')
		{ // End on newline
			*i = 0;
			break;
		}
		else if (*i == '\t')
			*i = ' '; // Tabs become spaces
		else if (*i < 32 || (unsigned)*i > 127)
			*i = '?'; // Nonprintable chars become question marks
		++i;
	}
	return clipboard_modified;
}

static boolean FileExists(const char *path)
{
	FILE *f = fopen(path, "rb");
	if (!f)
		return false;
	fclose(f);
	return true;
}

// The game data lives next to the program (PS2Boot_Init chose the root: host: while developing, the disc,
// the USB stick). Always returns that directory, absolute (no chdir), also when srb2.pk3 is not there:
// IdentifyVersion then reports where it looked.
const char *I_LocateWad(void)
{
	char path[PS2BOOT_PATHMAX + 16];

	snprintf(path, sizeof path, "%s/" WADKEYWORD1, ps2boot.datadir);
	I_OutputMsg("Looking for WADs in: %s (%s)\n", ps2boot.datadir, FileExists(path) ? "found" : "not found");
	return ps2boot.datadir;
}

// newlib has no realpath; filesrch.c calls it only for symbolic links, which the PS2 devices do not have
char *realpath(const char *__restrict path, char *__restrict resolved) __attribute__((weak));
char *realpath(const char *__restrict path, char *__restrict resolved)
{
	return resolved ? strcpy(resolved, path) : strdup(path);
}

const CPUInfoFlags *I_CPUInfo(void)
{
	static CPUInfoFlags info;

	info.FPU = 1; // single-precision COP1; doubles are soft-float
	info.CPUs = 1;
	return &info;
}

void I_RegisterSysCommands(void) {}

const char *I_GetSysName(void)
{
	return "PS2";
}

// Called by Z_Malloc just before the fatal "Out of memory" error.
void PS2_ReportOOM(void)
{
	struct mallinfo mi = mallinfo();
	INT32 tag;
	size_t total = 0;

	CONS_Printf("OOM: newlib arena %lu B, in use %lu B, free %lu B\n",
		(unsigned long)mi.arena, (unsigned long)mi.uordblks, (unsigned long)mi.fordblks);
	for (tag = 0; tag < 256; tag++)
	{
		size_t n = Z_TagUsage(tag);
		if (n)
		{
			CONS_Printf("OOM: zone tag %d: %lu B\n", (int)tag, (unsigned long)n);
			total += n;
		}
	}
	CONS_Printf("OOM: zone total %lu B\n", (unsigned long)total);
#ifdef ZDEBUG
	{
		extern void Z_DumpOwners(size_t top);
		Z_DumpOwners(24);
	}
#endif
}

// Largest block malloc can still give plus what is already allocated = the heap capacity the zone may use.
// Probed once (binary search over malloc/free), before the zone fills the heap.
size_t PS2_HeapCapacity(void)
{
	size_t lo = 0, hi = 32u << 20, used = (size_t)mallinfo().uordblks;

	while (hi - lo > 4096)
	{
		size_t mid = lo + (hi - lo) / 2;
		void *p = malloc(mid);
		if (p)
		{
			free(p);
			lo = mid;
		}
		else
			hi = mid;
	}
	return used + lo;
}
