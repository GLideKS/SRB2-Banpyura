/* PS2 system layer test: links the real src/ps2/{ps2_boot,i_system,i_joy,i_sound,ps2_padmap}.c and
 * stubs the few engine functions they call. Run through tools/ps2/build_sys_test.py --run.
 * Every result line starts with "ST ". Usage (PCSX2 -gameargs): [-alpha beta] [-iopreset] [-test-error] */
#include <kernel.h>
#include <sifrpc.h>
#include <timer.h>
#include <delaythread.h>
#include <loadfile.h>
#define NEWLIB_PORT_AWARE
#include <fileXio_rpc.h>
#include <io_common.h>
#include <libpad.h>

#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include "../../src/doomdef.h"
#include "../../src/d_main.h"
#include "../../src/d_event.h"
#include "../../src/g_demo.h"
#include "../../src/doomstat.h"
#include "../../src/g_game.h"
#include "../../src/g_input.h"
#include "../../src/i_joy.h"
#include "../../src/i_sound.h"
#include "../../src/i_system.h"
#include "../../src/i_time.h"
#include "../../src/i_threads.h"
#include "../../src/m_argv.h"
#include "../../src/m_cond.h"
#include "../../src/m_menu.h"
#include "../../src/m_misc.h"
#include "../../src/netcode/d_clisrv.h"
#include "../../src/netcode/d_netcmd.h"
#include "../../src/ps2/ps2_boot.h"
#include "../../src/ps2/ps2_padmap.h"

static int failures;

#define CHECK(cond, ...) do { if (!(cond)) { failures++; printf("ST FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* ---------------------------------------------------------------------------------------------
 * Engine stand-ins (the real ones live in files that are not linked here). Each save/shutdown call
 * prints a line so the I_Quit / I_Error sequences can be checked in the log.
 * ------------------------------------------------------------------------------------------- */
event_t events[MAXEVENTS];
INT32 eventhead, eventtail;
UINT8 shiftdown, ctrldown, altdown;
boolean capslock;
boolean demorecording, metalrecording;
moviemode_t moviemode = MM_OFF;
gamedata_t *clientGamedata;
JoyType_t Joystick, Joystick2;
consvar_t cv_usejoystick = CVAR_INIT("use_gamepad", "1", 0, NULL, NULL);
consvar_t cv_usejoystick2 = CVAR_INIT("use_gamepad2", "2", 0, NULL, NULL);
consvar_t cv_joyscale = CVAR_INIT("padscale", "1", 0, NULL, NULL);
consvar_t cv_joyscale2 = CVAR_INIT("padscale2", "1", 0, NULL, NULL);

void D_PostEvent(const event_t *ev)
{
	events[eventhead] = *ev;
	eventhead = (eventhead+1) & (MAXEVENTS-1);
}

void CONS_Printf(const char *fmt, ...)
{
	char buf[512];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof buf, fmt, ap);
	va_end(ap);
	I_OutputMsg("%s", buf);
}

void M_SaveConfig(const char *filename) { (void)filename; printf("STUB M_SaveConfig\n"); }
void G_SaveGameData(gamedata_t *data) { (void)data; printf("STUB G_SaveGameData\n"); }
boolean G_CheckDemoStatus(void) { printf("STUB G_CheckDemoStatus\n"); return false; }
void G_StopMetalRecording(boolean kill) { (void)kill; printf("STUB G_StopMetalRecording\n"); for (;;) {} }
void M_StopMovie(void) { printf("STUB M_StopMovie\n"); }
void D_QuitNetGame(void) { printf("STUB D_QuitNetGame\n"); }
void M_FreePlayerSetupColors(void) { printf("STUB M_FreePlayerSetupColors\n"); }
void W_Shutdown(void) { printf("STUB W_Shutdown\n"); }
void I_ShutdownGraphics(void) { printf("STUB I_ShutdownGraphics\n"); }
UINT32 R_GetFramerateCap(void) { return 35; }

/* ---------------------------------------------------------------------------------------------
 * helpers
 * ------------------------------------------------------------------------------------------- */
static u32 cop0(void)
{
	u32 v;
	__asm__ volatile("mfc0 %0,$9" : "=r"(v));
	return v;
}

static double ms_of(UINT64 ticks)
{
	return (double)ticks * 1000.0 / (double)I_GetPrecisePrecision();
}

static volatile int vblanks;

static s32 VBlank(s32 cause)
{
	(void)cause;
	vblanks++;
	ExitHandler();
	return 0;
}

/* ---------------------------------------------------------------------------------------------
 * tests
 * ------------------------------------------------------------------------------------------- */
static void TestBoot(int argc, char **argv)
{
	int i, alpha = 0;
	const boolean wantreset = M_CheckParm("-iopreset") != 0;

	printf("ST boot argc=%d", argc);
	for (i = 0; i < argc; i++)
		printf(" [%d]=%s", i, argv[i]);
	printf("\n");
	printf("ST boot modules sio2man=%d padman=%d iomanX=%d fileXio=%d poweroff=%d poweroff_ok=%d iopreset=%d\n",
		(int)ps2boot.sio2man, (int)ps2boot.padman, (int)ps2boot.iomanx, (int)ps2boot.fileXio, (int)ps2boot.poweroff, ps2boot.poweroff_ok, ps2boot.iopreset);
	CHECK(ps2boot.sio2man > 0 && ps2boot.padman > 0 && ps2boot.iomanx > 0 && ps2boot.fileXio > 0 && ps2boot.poweroff > 0 && ps2boot.poweroff_ok, "IOP modules");
	CHECK(ps2boot.iopreset == wantreset, "iopreset=%d want %d", ps2boot.iopreset, wantreset);

	CHECK(!strcmp(argv[0], "SRB2.ELF"), "argv[0]=%s", argv[0]);
	alpha = M_CheckParm("-alpha");
	CHECK(alpha == (wantreset ? 2 : 1) && argc >= alpha + 5 && !strcmp(argv[alpha + 1], "beta"), "command line arguments follow the program name in order");
	CHECK(argc >= 3 && !strcmp(argv[argc - 3], "-fromfile") && !strcmp(argv[argc - 2], "+x") && !strcmp(argv[argc - 1], "last"),
		"ps2args lines come last, comments/blank lines dropped, whitespace trimmed");
	CHECK(M_CheckParm("-fromfile") == argc - 3, "M_CheckParm finds a ps2args line");

	printf("ST boot paths device=%s data=%s home=%s host=%d\n", ps2boot.bootpath, ps2boot.datadir, ps2boot.homedir, ps2boot.host);
	CHECK(!strcmp(ps2boot.datadir, "host:") && !strcmp(ps2boot.homedir, "host:") && ps2boot.host, "storage roots under host:");
}

static void TestPaths(void)
{
	const char *wad = I_LocateWad();
	FILE *f;
	char buf[16] = "";

	CHECK(wad && !strcmp(wad, "host:"), "I_LocateWad=%s", wad ? wad : "(null)");
	CHECK(I_GetEnv("HOME") && !strcmp(I_GetEnv("HOME"), "host:"), "I_GetEnv(HOME)");
	CHECK(I_GetEnv("SRB2_NO_SUCH_VARIABLE") == NULL, "I_GetEnv unknown");

	/* srb2home = HOME/DEFAULTDIR; the engine only creates srb2home/addons: parents must appear */
	I_mkdir("host:/" DEFAULTDIR "/addons", 0755);
	f = fopen("host:/" DEFAULTDIR "/addons/mkdir_probe.txt", "w");
	CHECK(f != NULL, "write under a freshly created nested directory");
	if (f)
	{
		fputs("config", f);
		fclose(f);
		f = fopen("host:/" DEFAULTDIR "/addons/mkdir_probe.txt", "r");
		CHECK(f && fgets(buf, sizeof buf, f) && !strcmp(buf, "config"), "read back");
		if (f)
			fclose(f);
	}
	printf("ST paths wad=%s home=%s srb2home=host:/%s\n", wad ? wad : "(null)", I_GetEnv("HOME"), DEFAULTDIR);

	{
		INT64 freespace = 0;
		const CPUInfoFlags *c = I_CPUInfo();
		I_GetDiskFreeSpace(&freespace);
		CHECK(c && c->FPU && freespace > 0 && !strcmp(I_GetSysName(), "PS2"), "cpuinfo/sysname/diskfree");
	}
	{
		char a[16], b[16];
		memset(a, 0, sizeof a);
		memset(b, 0, sizeof b);
		CHECK(I_GetRandomBytes(a, 13) == 13 && I_GetRandomBytes(b, 13) == 13 && memcmp(a, b, 13) != 0, "random bytes");
	}
	{
		size_t total = 0, freemem = I_GetFreeMem(&total);
		printf("ST freemem free=%u total=%u\n", (unsigned)freemem, (unsigned)total);
		CHECK(total == 32u*1024*1024 && freemem > 1024*1024 && freemem < total, "I_GetFreeMem");
	}
	I_ClipboardCopy("hello\tworld\nrest", 16);
	CHECK(I_ClipboardPaste() && !strcmp(I_ClipboardPaste(), "hello world"), "clipboard round trip (tab, newline)");
	CHECK(!I_can_thread(), "I_can_thread");
	{
		I_mutex m = NULL;
		I_lock_mutex(&m);
		I_unlock_mutex(m);
	}
	I_SetTextInputMode(true);
	CHECK(I_GetTextInputMode(), "text input mode");
	I_SetTextInputMode(false);
	CHECK(I_BaseTiccmd() != I_BaseTiccmd2() && I_BaseTiccmd(), "ticcmds");
}

#define WINDOWS 10 /* x 35 tics = 10 s */

static void TestTime(void)
{
	const UINT64 prec = I_GetPrecisePrecision();
	precise_t a, b, t0;
	u32 c0;
	int i, nonmono = 0, k;
	double tictimer = 0.0;
	INT32 entertic = 0;
	precise_t oldp;
	double total_ms = 0.0, total_cop_ms = 0.0;
	int total_vb = 0;

	printf("ST time precision=%llu\n", (unsigned long long)prec);
	CHECK(prec == 147456000ULL, "precision");

	a = I_GetPreciseTime();
	c0 = cop0();
	for (i = 0; i < 200000; i++)
	{
		b = I_GetPreciseTime();
		if ((INT64)(b - a) < 0)
			nonmono++;
		a = b;
	}
	printf("ST time monotonic samples=200000 nonmonotonic=%d cycles_per_call=%u\n", nonmono, (unsigned)((cop0() - c0) / 200000));
	CHECK(nonmono == 0, "I_GetPreciseTime went backwards %d times", nonmono);

	t0 = I_GetPreciseTime();
	I_Sleep(50);
	printf("ST time I_Sleep(50) elapsed_ms=%.3f\n", ms_of(I_GetPreciseTime() - t0));
	CHECK(ms_of(I_GetPreciseTime() - t0) >= 49.0 && ms_of(I_GetPreciseTime() - t0) < 90.0, "I_Sleep(50)");

	t0 = I_GetPreciseTime();
	I_SleepDuration(prec / 100); /* 10 ms */
	printf("ST time I_SleepDuration(10ms) elapsed_ms=%.3f\n", ms_of(I_GetPreciseTime() - t0));
	CHECK(ms_of(I_GetPreciseTime() - t0) >= 10.0 && ms_of(I_GetPreciseTime() - t0) < 14.0, "I_SleepDuration(10ms)");

	t0 = I_GetPreciseTime();
	I_SleepDuration(prec / 2000); /* 0.5 ms: all spin */
	printf("ST time I_SleepDuration(0.5ms) elapsed_ms=%.3f\n", ms_of(I_GetPreciseTime() - t0));
	CHECK(ms_of(I_GetPreciseTime() - t0) >= 0.5 && ms_of(I_GetPreciseTime() - t0) < 3.0, "I_SleepDuration(0.5ms)");
	I_SleepDuration(0);
	I_Sleep(0);

	/* 35 Hz: the accumulator of src/i_time.c I_UpdateTime, fed only by I_GetPreciseTime/Precision. Four windows of
	 * 35 tics (1 s). Reference clocks: COP0 Count (294.912 MHz) and the vblank interrupt counter. The host
	 * wall clock comes from the log's own timestamps around the wall_begin/wall_end lines. */
	AddIntcHandler(INTC_VBLANK_S, VBlank, 0);
	EnableIntc(INTC_VBLANK_S);
	I_Sleep(50);

	printf("ST wall_begin\n");
	oldp = I_GetPreciseTime();
	for (k = 0; k < WINDOWS; k++)
	{
		const precise_t tp0 = I_GetPreciseTime();
		const u32 tc0 = cop0();
		const int tv0 = vblanks;
		const INT32 want = entertic + 35;
		while (entertic < want)
		{
			precise_t now;
			I_Sleep(1);
			now = I_GetPreciseTime();
			tictimer += (double)(now - oldp) / prec;
			oldp = now;
			while (tictimer > 1.0/35.0)
			{
				entertic += 1;
				tictimer -= 1.0/35.0;
			}
		}
		{
			const double pms = ms_of(I_GetPreciseTime() - tp0);
			const double cms = (double)(u32)(cop0() - tc0) * 1000.0 / 294912000.0;
			const int vb = vblanks - tv0;
			printf("ST tic35 window=%d precise_ms=%.3f cop0_ms=%.3f vblanks=%d\n", k, pms, cms, vb);
			total_ms += pms;
			total_cop_ms += cms;
			total_vb += vb;
			CHECK(pms > 990.0 && pms < 1010.0, "35 tics took %.3f ms (want 1000 +-1%%)", pms);
			CHECK(cms > pms * 0.999 && cms < pms * 1.001, "COP0 Count disagrees with the bus timer: %.3f vs %.3f ms", cms, pms);
		}
	}
	printf("ST wall_end\n");
	printf("ST tic35 total tics=%d precise_ms=%.3f cop0_ms=%.3f vblanks=%d vblank_hz=%.3f\n", (int)entertic, total_ms, total_cop_ms, total_vb, total_vb / (total_ms / 1000.0));
	CHECK(total_ms > 0.995 * (WINDOWS * 1000.0) && total_ms < 1.005 * (WINDOWS * 1000.0), "%d windows of 35 tics: %.3f ms", WINDOWS, total_ms);
}

static UINT32 fnv(UINT32 h, const UINT8 *p, size_t n)
{
	while (n--)
		h = (h ^ *p++) * 16777619u;
	return h;
}

/* One pass over the big file through stdio. Only the fread calls are timed (the checksum is not). */
static void ReadStdio(const char *label, UINT32 expect, size_t block, size_t vbuf, UINT8 *buf)
{
	UINT32 sum = 2166136261u;
	size_t got, total = 0;
	UINT64 t0, dt = 0;
	FILE *f = fopen("host:/sys_test_big.bin", "rb");

	CHECK(f != NULL, "open for %s", label);
	if (!f)
		return;
	if (vbuf)
		setvbuf(f, memalign(64, vbuf), _IOFBF, vbuf);
	for (;;)
	{
		t0 = I_GetPreciseTime();
		got = fread(buf, 1, block, f);
		dt += I_GetPreciseTime() - t0;
		if (!got)
			break;
		sum = fnv(sum, buf, got);
		total += got;
	}
	fclose(f);
	printf("ST file %-22s bytes=%u fnv=%08x ms=%.1f MiB/s=%.2f\n", label, (unsigned)total, (unsigned)sum, ms_of(dt),
		ms_of(dt) > 0 ? (double)total / 1048576.0 / (ms_of(dt) / 1000.0) : 0.0);
	CHECK(total >= 8u*1024*1024 && sum == expect, "%s: read of a >= 8 MiB file (bytes=%u fnv=%08x expect=%08x)", label, (unsigned)total, (unsigned)sum, (unsigned)expect);
}

static void TestFiles(void)
{
	static const char name[] = "host:/sys_test_big.bin";
	UINT32 expect = 0, sum;
	UINT8 *buf = memalign(64, 65536);
	FILE *f;
	size_t total = 0;
	UINT64 t0, dt = 0;
	int fd;
	char line[64] = "";

	CHECK(buf && !(((unsigned)buf) & 63), "64-byte aligned I/O buffer");
	f = fopen("host:/sys_test_big.sum", "r");
	if (f)
	{
		if (fgets(line, sizeof line, f))
			expect = (UINT32)strtoul(line, NULL, 16);
		fclose(f);
	}
	CHECK(expect != 0, "checksum file");

	/* stdio is what the engine uses (fopen/fread): default buffering, small blocks, big stdio buffer */
	ReadStdio("stdio fread 64K", expect, 65536, 0, buf);
	ReadStdio("stdio fread 4K", expect, 4096, 0, buf);
	ReadStdio("stdio 64K setvbuf", expect, 65536, 65536, buf);

	/* fileXio direct, aligned buffer, 64 KiB blocks */
	total = 0;
	sum = 2166136261u;
	fd = fileXioOpen(name, FIO_O_RDONLY, 0);
	CHECK(fd >= 0, "fileXioOpen %d", fd);
	while (fd >= 0)
	{
		int n;
		t0 = I_GetPreciseTime();
		n = fileXioRead(fd, buf, 65536);
		dt += I_GetPreciseTime() - t0;
		if (n <= 0)
			break;
		sum = fnv(sum, buf, (size_t)n);
		total += (size_t)n;
	}
	if (fd >= 0)
		fileXioClose(fd);
	printf("ST file %-22s bytes=%u fnv=%08x ms=%.1f MiB/s=%.2f\n", "fileXio 64K aligned", (unsigned)total, (unsigned)sum, ms_of(dt),
		ms_of(dt) > 0 ? (double)total / 1048576.0 / (ms_of(dt) / 1000.0) : 0.0);
	CHECK(total >= 8u*1024*1024 && sum == expect, "fileXio read of a >= 8 MiB file");
	free(buf);
}

static void TestPad(void)
{
	INT32 n, num, p;
	const INT32 before = (eventhead - eventtail) & (MAXEVENTS-1);
	char name[64], name2[64];

	cv_joyscale.value = cv_joyscale2.value = 1; /* default "padscale 1": analog axes */
	cv_usejoystick.value = 1;
	cv_usejoystick2.value = 2;
	I_InitJoystick();
	I_InitJoystick2();
	num = I_NumJoys();
	strlcpy(name, I_GetJoyName(1), sizeof name); /* one static buffer */
	strlcpy(name2, I_GetJoyName(2), sizeof name2);
	printf("ST pad numjoys=%d name1=\"%s\" name2=\"%s\" use1=%d use2=%d\n", (int)num, name, name2, cv_usejoystick.value, cv_usejoystick2.value);
	CHECK(num >= 1 && strstr(name, "DualShock") != NULL, "port 1 is a DualShock in analog mode");

	for (p = 0; p < 2; p++)
	{
		printf("ST pad port=%d state=%d\n", (int)p + 1, padGetState(p, 0));
	}
	CHECK(padGetState(0, 0) == 6, "pad port 1 state 6 (stable)");

	for (n = 0; n < 40; n++)
	{
		I_OsPolling();
		I_Sleep(16);
	}
	printf("ST pad polled 40x, events posted=%d (nothing is pressed: 0 expected)\n", (int)(((eventhead - eventtail) & (MAXEVENTS-1)) - before));
	CHECK((((eventhead - eventtail) & (MAXEVENTS-1)) - before) == 0, "no input must produce no events");

	/* vibration path must not crash even if no actuator answers */
	{
		JoyFF_t fx = {0};
		fx.Magnitude = 8000;
		fx.Duration = 20000;
		I_Tactile(ConstantForce, &fx);
		I_Sleep(40);
		I_OsPolling();
		I_Tactile(ConstantForce, NULL);
		I_Tactile2(ConstantForce, &fx);
	}
	I_JoyScale();
	I_JoyScale2();
	printf("ST pad gamepadstyle=%d/%d\n", Joystick.bGamepadStyle, Joystick2.bGamepadStyle);
	CHECK(Joystick.bGamepadStyle == 0, "padscale 1 = analog axes");

	printf("ST pad NOTE: no input device is attached to PCSX2 here; button/stick events are verified only on synthetic data\n");
	CHECK(PS2Pad_SelfTest() == 0, "padmap self-test");
}

static int fade_calls;
static void FadeDone(void) { fade_calls++; }

static void TestSound(void)
{
	sfxinfo_t info;
	void *data;
	INT32 h;
	precise_t t0;

	memset(&info, 0, sizeof info);
	CHECK(!sound_started, "sound starts stopped");
	I_StartupSound();
	I_InitMusic();
	CHECK(sound_started, "sound_started after I_StartupSound");
	I_SetSfxVolume(31);

	data = I_GetSfx(&info);
	CHECK(data != NULL, "I_GetSfx must return a non-NULL marker (s_sound.c caches it)");
	info.data = data;
	h = I_StartSound(sfx_jump, 255, 128, 128, 0, 5);
	CHECK(h == 5 && I_SoundIsPlaying(h), "started sound plays (handle %d)", (int)h);
	I_UpdateSoundParams(h, 100, 128, 128);
	I_StopSound(h);
	CHECK(!I_SoundIsPlaying(h), "stopped sound does not play");
	h = I_StartSound(sfx_jump, 255, 128, 128, 0, 6);
	t0 = I_GetPreciseTime();
	while (I_SoundIsPlaying(h) && ms_of(I_GetPreciseTime() - t0) < 1000.0)
		I_Sleep(5);
	printf("ST sound sfx playing-time ms=%.1f\n", ms_of(I_GetPreciseTime() - t0));
	CHECK(!I_SoundIsPlaying(h), "channel is freed after the nominal duration");
	CHECK(!I_SoundIsPlaying(-1) && !I_SoundIsPlaying(100000), "bad handles are silent");
	I_FreeSfx(&info);
	CHECK(info.data == NULL, "I_FreeSfx clears data");

	/* songs: nothing loaded -> MU_NONE, cannot play */
	CHECK(I_SongType() == MU_NONE && !I_SongPlaying() && !I_PlaySong(false), "no song loaded");
	CHECK(I_LoadSong((char *)"x", 1) && I_SongType() != MU_NONE && I_SongType() != MU_MID, "I_LoadSong");
	CHECK(I_PlaySong(true) && I_SongPlaying() && !I_SongPaused(), "I_PlaySong");
	I_Sleep(100);
	CHECK(I_GetSongPosition() >= 90 && I_GetSongPosition() < 400, "song position advances (%u ms)", (unsigned)I_GetSongPosition());
	I_PauseSong();
	{
		UINT32 p1 = I_GetSongPosition();
		I_Sleep(60);
		CHECK(I_SongPaused() && I_GetSongPosition() == p1, "position frozen while paused");
	}
	I_ResumeSong();
	CHECK(!I_SongPaused() && I_SongPlaying(), "resume");
	CHECK(I_SetSongPosition(5000) && I_GetSongPosition() >= 5000, "seek");
	CHECK(I_SetSongLoopPoint(1234) && I_GetSongLoopPoint() == 1234, "loop point");
	CHECK(!I_SetSongSpeed(1.5f) && !I_SetSongTrack(1) && I_GetSongLength() == 0, "unsupported features say so");

	/* fade out with a callback: must complete from I_UpdateSound (S_ChangeMusic queues the next song there) */
	fade_calls = 0;
	CHECK(I_FadeSong(0, 200, FadeDone), "I_FadeSong starts");
	t0 = I_GetPreciseTime();
	while (!fade_calls && ms_of(I_GetPreciseTime() - t0) < 1000.0)
	{
		I_Sleep(5);
		I_UpdateSound();
	}
	printf("ST sound fade 200ms callback after ms=%.1f calls=%d\n", ms_of(I_GetPreciseTime() - t0), fade_calls);
	CHECK(fade_calls == 1 && ms_of(I_GetPreciseTime() - t0) >= 190.0 && ms_of(I_GetPreciseTime() - t0) < 400.0, "fade callback once, on time");
	I_UpdateSound();
	CHECK(fade_calls == 1, "callback not repeated");

	/* instant fade runs the callback immediately; fade+stop ends the song */
	fade_calls = 0;
	CHECK(I_FadeSong(100, 0, FadeDone) && fade_calls == 1, "0 ms fade calls back immediately");
	CHECK(I_FadeOutStopSong(100), "I_FadeOutStopSong");
	t0 = I_GetPreciseTime();
	while (I_SongPlaying() && ms_of(I_GetPreciseTime() - t0) < 1000.0)
	{
		I_Sleep(5);
		I_UpdateSound();
	}
	CHECK(!I_SongPlaying(), "song stopped by the fade-out");
	CHECK(I_LoadSong((char *)"y", 1) && I_FadeInPlaySong(100, false) && I_SongPlaying(), "fade-in play");
	I_StopFadingSong();
	I_StopSong();
	CHECK(!I_SongPlaying(), "I_StopSong");
	I_UnloadSong();
	CHECK(I_SongType() == MU_NONE, "unloaded");
	I_SetMusicVolume(20);
	I_SetInternalMusicVolume(50);
	I_ShutdownMusic();
	I_ShutdownSound();
	CHECK(!sound_started, "sound_started cleared by I_ShutdownSound");
	I_StartupSound();
}

int main(int argc, char **argv)
{
	setvbuf(stdout, NULL, _IONBF, 0);
	printf("ST begin\n");
	PS2Boot_Init(&argc, &argv);
	myargc = argc;
	myargv = argv;

	I_StartupSystem();
	I_StartupTimer();

	TestBoot(argc, argv);
	TestPaths();
	TestTime();
	TestFiles();
	TestPad();
	TestSound();

	printf("ST COMPLETE failures=%d\n", failures);
	if (M_CheckParm("-test-error"))
		I_Error("test error %d: %s", 42, "fatal path");
	I_Quit();
}
