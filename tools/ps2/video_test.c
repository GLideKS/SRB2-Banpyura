/* Hardware test for the PS2 video layer (src/ps2/i_video.c + src/ps2/ps2_gs.c).
 * Links the two real files plus minimal stand-ins for the engine symbols they use; no game logic here.
 * Everything is checked by reading VRAM back through the GS (local -> host) and comparing with the expected pixels.
 *   args (PCSX2 -gameargs; the leading dash is optional): ntsc | pal | 480p   video mode (default: by region)
 *         noswap                   run with the CLUT bit 3/4 swap disabled: the test must turn red
 *         frames=N                 length of the paced soak run (default 700)
 */
#include "doomdef.h"
#include "doomstat.h"
#include "i_system.h"
#include "v_video.h"
#include "m_argv.h"
#include "s_sound.h"
#include "i_video.h"
#include "console.h"
#include "command.h"
#include "netcode/d_netcmd.h"
#include "netcode/tic_command.h"
#include "ps2_gs.h"

#include <tamtypes.h>
#include <kernel.h>
#include <screenshot.h>
#include <rom0_info.h>
#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- stand-ins for engine symbols used by i_video.c ---- */
viddef_t vid;
UINT8 *screens[5];
INT32 setmodeneeded;
UINT8 setrenderneeded;
boolean dedicated;
boolean netgame;
INT32 consoleplayer;
INT32 serverplayer;
tic_t simulated_lag;
marathonmode_t marathonmode;
CV_PossibleValue_t CV_OnOff[] = {{0, "Off"}, {1, "On"}, {0, NULL}};
consvar_t cv_ticrate, cv_showping, cv_closedcaptioning;

static int g_argc;
static char **g_argv;
static int overlay_calls, regs, drawfuncs, fps_calls;

INT32 M_CheckParm(const char *check)
{
	int i;
	for (i = 0; i < g_argc; i++) /* PCSX2 -gameargs: the first token arrives as argv[0] */
		if (!strcmp(g_argv[i], check) || !strcmp(g_argv[i], check + (check[0] == '-')) || !strcmp(g_argv[i], check + 2 * (check[0] == '-' && check[1] == '-')))
			return i + 1;
	return 0;
}
void I_Error(const char *error, ...)
{
	printf("V0 I_Error: %s\n", error);
	for (;;)
		SleepThread();
}
void CV_RegisterVar(consvar_t *variable) { (void)variable; regs++; }
boolean Playing(void) { return false; }
void SCR_SetDrawFuncs(void) { drawfuncs++; }
void SCR_CalculateFPS(void) { fps_calls++; }
void SCR_ClosedCaptions(void) { overlay_calls++; }
void SCR_DisplayTicRate(void) { overlay_calls++; }
void SCR_DisplayLocalPing(void) { overlay_calls++; }
void SCR_DisplayMarathonInfo(void) { overlay_calls++; }
void VID_BlitLinearScreen(const UINT8 *srcptr, UINT8 *destptr, INT32 width, INT32 height, size_t srcrowbytes, size_t destrowbytes)
{
	INT32 y;
	for (y = 0; y < height; y++)
		memcpy(destptr + y * destrowbytes, srcptr + y * srcrowbytes, width);
}

/* the part of V_Init that matters here */
static void test_V_Init(void)
{
	int i;
	for (i = 0; i < NUMSCREENS; i++)
		screens[i] = vid.buffer + (size_t)i * vid.rowbytes * vid.height;
}

/* ---- helpers ---- */
static u32 count(void)
{
	u32 v;
	__asm__ volatile("mfc0 %0,$9" : "=r"(v));
	return v;
}
static unsigned vbl(void)
{
	ps2gs_stats_t s;
	ps2gs_get_stats(&s);
	return s.vblanks;
}

static int failures;
#define CHECK(name, cond, ...) do { int ok_ = (cond) ? 1 : 0; failures += !ok_; printf("V0 %s %s ", ok_ ? "PASS" : "FAIL", name); printf(__VA_ARGS__); printf("\n"); } while (0)

static UINT32 palA[256], palB[256];
static RGBA_t rgbaA[256], rgbaB[256];
static u32 *readback;
static UINT8 *fbuf;

static void make_palettes(void)
{
	unsigned i;
	for (i = 0; i < 256; i++)
	{
		/* both are bijections per channel: every index has its own colour, so a wrong index can not hide */
		palA[i] = i | ((i ^ 0x55u) << 8) | ((255u - i) << 16);
		palB[i] = ((i * 7u) & 255u) | (((i * 13u + 5u) & 255u) << 8) | (((i * 29u + 101u) & 255u) << 16);
		rgbaA[i].s.red = palA[i] & 255; rgbaA[i].s.green = (palA[i] >> 8) & 255; rgbaA[i].s.blue = (palA[i] >> 16) & 255; rgbaA[i].s.alpha = 255;
		rgbaB[i].s.red = palB[i] & 255; rgbaB[i].s.green = (palB[i] >> 8) & 255; rgbaB[i].s.blue = (palB[i] >> 16) & 255; rgbaB[i].s.alpha = 255;
	}
}

static const unsigned char glyph[][7] = {
	{0x00,0x00,0x00,0x00,0x00,0x00,0x00}, /* ' ' */
	{0x0F,0x10,0x10,0x0E,0x01,0x01,0x1E}, /* S */
	{0x1E,0x11,0x11,0x1E,0x14,0x12,0x11}, /* R */
	{0x1E,0x11,0x11,0x1E,0x11,0x11,0x1E}, /* B */
	{0x0E,0x11,0x01,0x02,0x04,0x08,0x1F}, /* 2 */
	{0x1E,0x11,0x11,0x1E,0x10,0x10,0x10}, /* P */
	{0x11,0x11,0x11,0x11,0x11,0x0A,0x04}, /* V */
	{0x0E,0x04,0x04,0x04,0x04,0x04,0x0E}, /* I */
	{0x1E,0x11,0x11,0x11,0x11,0x11,0x1E}, /* D */
	{0x1F,0x10,0x10,0x1E,0x10,0x10,0x1F}, /* E */
	{0x0E,0x11,0x11,0x11,0x11,0x11,0x0E}, /* O */
	{0x1F,0x04,0x04,0x04,0x04,0x04,0x04}, /* T */
};
static int glyph_of(char c)
{
	static const char order[] = " SRB2PVIDEOT";
	const char *p = strchr(order, c);
	return p ? (int)(p - order) : 0;
}
static void put(UINT8 *b, int x, int y, UINT8 v)
{
	if (x >= 0 && x < 320 && y >= 0 && y < 200)
		b[y * 320 + x] = v;
}
static void text(UINT8 *b, int x, int y, int scale, UINT8 v, const char *s)
{
	int gx, gy, sx, sy;
	for (; *s; s++, x += 6 * scale)
		for (gy = 0; gy < 7; gy++)
			for (gx = 0; gx < 5; gx++)
				if (glyph[glyph_of(*s)][gy] & (0x10 >> gx))
					for (sy = 0; sy < scale; sy++)
						for (sx = 0; sx < scale; sx++)
							put(b, x + gx * scale + sx, y + gy * scale + sy, v);
}

/* known index pattern; frame != 0 moves the gradient and a box so every soak frame differs */
static void draw_pattern(UINT8 *b, int frame)
{
	int x, y, bx;
	for (y = 0; y < 200; y++)
		for (x = 0; x < 320; x++)
		{
			UINT8 v;
			if (y < 48) v = (UINT8)((x * 256) / 320 + frame);                        /* gradient, every index */
			else if (y < 96) v = (x < 160) ? (((x / 8 + y / 8) & 1) ? 0x08 : 0x10)  /* checker: bits 3/4 swap-sensitive */
				: (((x / 8 + y / 8) & 1) ? 0x18 : 0xE7);
			else if (y < 128) v = (UINT8)(((y - 96) * 8 + (x >> 6) * 3) & 255);      /* vertical gradient */
			else v = 0x20;
			b[y * 320 + x] = v;
		}
	text(b, 8, 134, 2, 0xF0, "SRB2 PS2");
	text(b, 8, 154, 2, 0x0F, "VIDEO TEST");
	for (x = 0; x < 320; x++) { b[x] = 0xFF; b[199 * 320 + x] = 0xFF; }
	for (y = 0; y < 200; y++) { b[y * 320] = 0xFF; b[y * 320 + 319] = 0xFF; }
	b[1 * 320 + 1] = 1; b[1 * 320 + 318] = 2; b[198 * 320 + 1] = 3; b[198 * 320 + 318] = 4;
	bx = 150 + (frame * 3) % 150;
	for (y = 170; y < 186; y++)
		for (x = bx; x < bx + 16; x++)
			b[y * 320 + x] = (UINT8)(0x40 + frame);
}

/* expected RGB of screen pixel (x,y) for dest rect (dx,dy,dw,dh): nearest texel, black outside */
static u32 expect(int x, int y, const UINT8 *idx, const UINT32 *pal, int dx, int dy, int dw, int dh)
{
	int u, v;
	if (x < dx || x >= dx + dw || y < dy || y >= dy + dh)
		return 0;
	/* GS pixel centres sit on integer coordinates: texel = floor((x - dx) * 320 / dw) (a +0.5 here would be wrong) */
	u = ((x - dx) * 320) / dw;
	v = ((y - dy) * 200) / dh;
	return pal[idx[v * 320 + u]];
}

static int read_fb(int index)
{
	int w = ps2gs_fb_width(), h = ps2gs_fb_height(), r = 1, y, n;
	memset(readback, 0xCD, (size_t)w * h * 4); /* a stale buffer must not pass */
	SyncDCache(readback, (u8 *)readback + (size_t)w * h * 4);
	/* ps2_screenshot moves at most 64K pixels per call: read in bands of 96 rows */
	for (y = 0; y < h; y += 96)
	{
		n = (h - y) < 96 ? (h - y) : 96;
		r &= ps2_screenshot(readback + (size_t)y * w, ps2gs_fb_block(index), 0, y, w, n, 0) == 1;
	}
	SyncDCache(readback, (u8 *)readback + (size_t)w * h * 4); /* drop cached lines of the DMA target */
	return r;
}

/* returns the number of mismatching pixels; first mismatch is printed */
static unsigned compare(const UINT8 *idx, const UINT32 *pal, int dx, int dy, int dw, int dh, const char *what)
{
	int w = ps2gs_fb_width(), h = ps2gs_fb_height(), x, y;
	unsigned bad = 0;
	int fx = -1, fy = -1;
	u32 fgot = 0, fexp = 0;
	for (y = 0; y < h; y++)
		for (x = 0; x < w; x++)
		{
			u32 got = readback[y * w + x] & 0xFFFFFF, want = expect(x, y, idx, pal, dx, dy, dw, dh);
			if (got != want)
			{
				if (!bad) { fx = x; fy = y; fgot = got; fexp = want; }
				bad++;
			}
		}
	if (bad)
		printf("V0 info %s first mismatch at (%d,%d) got=%06lx expected=%06lx\n", what, fx, fy, (unsigned long)fgot, (unsigned long)fexp);
	return bad;
}

static char hostname[64];

static void dump_fb(const char *name)
{
	char path[128];
	FILE *f;
	snprintf(path, sizeof path, "%s-%s.bin", hostname, name);
	f = fopen(path, "wb");
	if (!f) { printf("V0 info cannot open %s\n", path); return; }
	fwrite(readback, 4, (size_t)ps2gs_fb_width() * ps2gs_fb_height(), f);
	fclose(f);
}

/* present through the real engine entry point, then let the vblank flip happen */
static void show(void)
{
	I_FinishUpdate();
	ps2gs_wait_vblank(3);
}

static void verify(const char *name, const UINT32 *pal, int dx, int dy, int dw, int dh, unsigned expect_bad_min, int dump)
{
	int r;
	unsigned bad;
	r = read_fb(ps2gs_displayed());
	bad = compare(screens[0], pal, dx, dy, dw, dh, name);
	if (dump) dump_fb(name);
	if (expect_bad_min)
		CHECK(name, r == 1 && bad >= expect_bad_min, "readback=%d mismatch=%u (must be >= %u) of %d", r, bad, expect_bad_min, ps2gs_fb_width() * ps2gs_fb_height());
	else
		CHECK(name, r == 1 && bad == 0, "readback=%d mismatch=%u of %d displayed=%d pending=%d", r, bad, ps2gs_fb_width() * ps2gs_fb_height(), ps2gs_displayed(), ps2gs_flip_pending());
}

int main(int argc, char **argv)
{
	ps2gs_stats_t st;
	int i, noswap, frames = 700, fh, fw, dy, region = 0;
	u32 t;
	static const char *modename[] = {"ntsc", "pal", "480p"};

	g_argc = argc;
	g_argv = argv;
	for (i = 0; i < argc; i++)
		if (!strncmp(argv[i], "frames=", 7))
			frames = atoi(argv[i] + 7);
	noswap = M_CheckParm("--noswap") != 0;
	ps2gs_dbg_noswap = noswap; /* negative control: every pattern test below has to fail with it */

	printf("V0 hello argc=%d noswap=%d frames=%d\n", argc, noswap, frames);
	for (i = 0; i < argc; i++)
		printf("V0 info argv[%d]=%s\n", i, argv[i]);
	{
		char rom[16];
		memset(rom, 0, sizeof rom);
		GetRomName(rom);
		printf("V0 info rom0:ROMVER=%s (region letter '%c' -> %s)\n", rom, rom[4], rom[4] == 'E' ? "PAL" : "NTSC");
	}
	readback = memalign(64, 640 * 512 * 4);
	make_palettes();
	cv_ticrate.value = 1; /* make the overlay hooks run */
	cv_closedcaptioning.value = 1;
	marathonmode = MA_RUNNING;

	/* 1. startup through the engine entry point */
	I_StartupGraphics();
	test_V_Init();
	fbuf = screens[0];
	CHECK("startup", rendermode == render_soft && chosenrendermode == render_soft && graphics_started == 1 && drawfuncs >= 1,
		"rendermode=%d chosen=%d started=%d cvars=%d drawfuncs=%d", rendermode, chosenrendermode, graphics_started, regs, drawfuncs);
	CHECK("vid", vid.width == 320 && vid.height == 200 && vid.bpp == 1 && vid.rowbytes == 320 && vid.direct == NULL,
		"%dx%dx%d rowbytes=%d", vid.width, vid.height, vid.bpp, (int)vid.rowbytes);
	{
		int aligned = 1;
		for (i = 0; i < NUMSCREENS; i++)
			aligned &= !((u32)screens[i] & 63);
		CHECK("screens_aligned64", aligned && NUMSCREENS == 5, "screens[0]=%08lx [4]=%08lx stride=%d", (unsigned long)screens[0], (unsigned long)screens[4], (int)(screens[1] - screens[0]));
	}
	CHECK("modes", VID_NumModes() == 1 && VID_GetModeForSize(1280, 800) == 0 && !strcmp(VID_GetModeName(0), "320x200") && I_GetRefreshRate() == 35,
		"nummodes=%d forsize=%d name=%s refresh=%u", VID_NumModes(), VID_GetModeForSize(1280, 800), VID_GetModeName(0), (unsigned)I_GetRefreshRate());
	fw = ps2gs_fb_width();
	fh = ps2gs_fb_height();
	region = ps2gs_mode();
	printf("V0 info mode=%s fb=%dx%d fb0=%08lx fb1=%08lx\n", modename[region], fw, fh, (unsigned long)ps2gs_fb_block(0) * 256, (unsigned long)ps2gs_fb_block(1) * 256);
	snprintf(hostname, sizeof hostname, "host:video-%s", modename[region]);

	/* the clear done at init: both buffers black */
	{
		int r = read_fb(0), nz = 0;
		for (i = 0; i < fw * fh; i++) nz += (readback[i] & 0xFFFFFF) != 0;
		CHECK("init_clear", r == 1 && nz == 0, "readback=%d nonblack=%d", r, nz);
	}

	/* 2. exact 2x placement (integer scale, letterboxed): nearest sampling is unambiguous */
	dy = (fh - 400) / 2;
	ps2gs_set_dest(0, dy, 640, 400);
	draw_pattern(fbuf, 0);
	I_SetPalette(rgbaA);
	overlay_calls = 0;
	show();
	CHECK("overlay_hooks", overlay_calls == 3 && fps_calls == 1, "overlay_calls=%d (marathon, captions, ticrate) fps_calls=%d", overlay_calls, fps_calls);
	verify("pattern_2x", palA, 0, dy, 640, 400, 0, 1);

	/* 3. default placement: the whole frame buffer (stretched) */
	ps2gs_set_dest(0, 0, fw, fh);
	show();
	verify("pattern_stretch", palA, 0, 0, fw, fh, 0, 1);

	/* 4. palette change only: the CLUT must be reloaded, pixels untouched */
	I_SetPalette(rgbaB);
	show();
	verify("palette_change", palB, 0, 0, fw, fh, 0, 0);
	I_SetPalette(rgbaA);
	show();
	verify("palette_back", palA, 0, 0, fw, fh, 0, 0);

	/* 5. negative control: without the bit 3/4 swap the comparison has to go red */
	ps2gs_dbg_noswap = 1;
	I_SetPalette(rgbaA);
	show();
	verify("negative_control_noswap", palA, 0, 0, fw, fh, 1000, 0);
	ps2gs_dbg_noswap = noswap;
	I_SetPalette(rgbaA);
	show();
	verify("swap_restored", palA, 0, 0, fw, fh, 0, 0);

	/* 6. I_ReadScreen */
	{
		static UINT8 copy[64000] __attribute__((aligned(64)));
		I_ReadScreen(copy);
		CHECK("read_screen", !memcmp(copy, screens[0], 64000), "64000 bytes");
	}

	/* 6b. a source buffer that is not 64-byte aligned goes through the internal aligned copy */
	{
		UINT8 *raw = memalign(64, 64000 + 64);
		memcpy(raw + 1, screens[0], 64000);
		FlushCache(0);
		ps2gs_present(raw + 1, 0);
		ps2gs_wait_vblank(3);
		verify("unaligned_source", palA, 0, 0, fw, fh, 0, 0);
		free(raw);
	}

	/* 6c. optional bilinear filter (-linear): colours blend after the CLUT lookup, flat areas stay exact */
	if (!noswap)
	{
		unsigned bad, total = (unsigned)(fw * fh);
		int r;
		ps2gs_set_filter(1);
		show();
		r = read_fb(ps2gs_displayed());
		bad = compare(screens[0], palA, 0, 0, fw, fh, "linear");
		CHECK("filter_linear", r == 1 && bad > 0 && bad < total / 2, "readback=%d pixels differing from nearest=%u of %u (edges blend, must be >0 and <50%%)", r, bad, total);
		ps2gs_set_filter(0);
		show();
		verify("filter_nearest_restored", palA, 0, 0, fw, fh, 0, 0);
	}

	/* 7. blocking behaviour: back-to-back calls with vid_wait on and off, never more than one vblank per call */
	{
		unsigned maxd = 0, d, v0;
		u32 maxc = 0, c, sumc = 0;
		ps2gs_get_stats(&st);
		for (i = 0; i < 40; i++)
		{
			draw_pattern(fbuf, i);
			cv_vidwait.value = 1;
			v0 = vbl(); t = count();
			I_FinishUpdate();
			c = count() - t; d = vbl() - v0;
			if (d > maxd) maxd = d;
			if (c > maxc) maxc = c;
			sumc += c;
		}
		CHECK("burst_vidwait_on", maxd <= 1, "40 back-to-back calls: max vblanks per call=%u max cycles=%lu avg cycles=%lu (vblank=%u cycles)", maxd, (unsigned long)maxc, (unsigned long)(sumc / 40), 294912000u / ((region == PS2GS_MODE_PAL) ? 50u : 60u));
		maxd = 0; maxc = 0; sumc = 0;
		for (i = 0; i < 40; i++)
		{
			draw_pattern(fbuf, i);
			v0 = vbl(); t = count();
			I_UpdateNoVsync();
			c = count() - t; d = vbl() - v0;
			if (d > maxd) maxd = d;
			if (c > maxc) maxc = c;
			sumc += c;
		}
		CHECK("burst_novsync", maxd <= 1, "40 back-to-back I_UpdateNoVsync: max vblanks per call=%u max cycles=%lu avg cycles=%lu", maxd, (unsigned long)maxc, (unsigned long)(sumc / 40));
		cv_vidwait.value = 1;
		ps2gs_wait_vblank(3);
		ps2gs_get_stats(&st);
		printf("V0 info burst stats: frames=%u dropped=%u flips=%u waited=%u timeouts=%u\n", st.frames, st.dropped, st.flips, st.waited, st.timeouts);
	}

	/* 8. paced soak: logic at 35 Hz against a 59.94 Hz vblank, verified every 64th frame; also the timing of I_FinishUpdate */
	{
		unsigned done = 0, v_start, v_start0, vhz = (region == PS2GS_MODE_PAL) ? 50u : 60u, v, maxd = 0, d, v0, verified = 0, bad_total = 0;
		u32 maxc = 0, minc = 0xFFFFFFFFu, c, sumc = 0, nc = 0, maxc_clut = 0, sumc_clut = 0, nclut = 0;
		int frame = 0;
		ps2gs_get_stats(&st);
		v_start = vbl();
		v_start0 = v_start;
		while (done < (unsigned)frames)
		{
			ps2gs_wait_vblank(1);
			v = vbl() - v_start;
			if ((v * 35u) / vhz < done)
				continue;
			draw_pattern(fbuf, frame);
			if (frame % 50 == 25) I_SetPalette(rgbaB);
			if (frame % 50 == 0) I_SetPalette(rgbaA);
			v0 = vbl(); t = count();
			I_FinishUpdate();
			c = count() - t; d = vbl() - v0;
			if (d > maxd) maxd = d;
			if (c > maxc) maxc = c;
			if (c < minc) minc = c;
			sumc += c; nc++;
			if (frame % 50 == 25 || frame % 50 == 0) { sumc_clut += c; nclut++; if (c > maxc_clut) maxc_clut = c; }
			done++;
			frame++;
			if ((frame & 63) == 0)
			{
				int r;
				unsigned bad;
				ps2gs_wait_vblank(3);
				r = read_fb(ps2gs_displayed());
				bad = compare(screens[0], (((frame - 1) % 50) >= 25) ? palB : palA, 0, 0, fw, fh, "soak");
				verified++;
				bad_total += bad + (r != 1);
			}
		}
		ps2gs_wait_vblank(3);
		{
			int r = read_fb(ps2gs_displayed());
			unsigned bad = compare(screens[0], (((frame - 1) % 50) >= 25) ? palB : palA, 0, 0, fw, fh, "soak_final");
			verified++;
			bad_total += bad + (r != 1);
		}
		ps2gs_get_stats(&st);
		CHECK("soak_frames", done >= 600 && done == (unsigned)frames, "frames=%u (>=600 required)", done);
		CHECK("soak_verified", bad_total == 0, "%u full-screen readbacks compared, mismatches=%u", verified, bad_total);
		CHECK("soak_no_long_block", maxd <= 1, "max vblanks spent inside one I_FinishUpdate=%u", maxd);
		CHECK("soak_timeouts", st.timeouts == 0, "bounded spins that ran out=%u", st.timeouts);
		printf("V0 timing I_FinishUpdate cycles: n=%lu min=%lu avg=%lu max=%lu (vblank=%u cycles, 35Hz tic=%u cycles); with CLUT upload: n=%lu avg=%lu max=%lu\n",
			(unsigned long)nc, (unsigned long)minc, (unsigned long)(sumc / nc), (unsigned long)maxc, 294912000u / vhz, 294912000u / 35u,
			(unsigned long)nclut, (unsigned long)(nclut ? sumc_clut / nclut : 0), (unsigned long)maxc_clut);
		printf("V0 timing internals (EE cycles): flush+build max=%lu dma_wait max=%lu present max=%lu\n", (unsigned long)st.flush_max, (unsigned long)st.dma_wait_max, (unsigned long)st.present_max);
		printf("V0 stats: frames=%u dropped=%u flips=%u vblanks=%u waited=%u timeouts=%u; soak: %u calls in %u vblanks (%u.%u calls/s at %u Hz)\n", st.frames, st.dropped, st.flips, st.vblanks, st.waited, st.timeouts,
				done, vbl() - v_start0, (done * vhz * 10u / (vbl() - v_start0)) / 10u, (done * vhz * 10u / (vbl() - v_start0)) % 10u, vhz);
	}

	I_ShutdownGraphics();
	CHECK("shutdown", graphics_started == 0 && rendermode == render_none, "started=%d rendermode=%d", graphics_started, rendermode);
	printf("V0 COMPLETE failures=%d\n", failures);
	for (;;)
		SleepThread();
	return failures;
}
