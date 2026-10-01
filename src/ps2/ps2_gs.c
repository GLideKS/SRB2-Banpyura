// PS2 GS output for the 320x200x8 software renderer (see ps2_gs.h).
//
// Frame path: screens[0] (PSMT8, TBW=6) --GIF path 3 IMAGE, own DMA chain with a REF tag straight from the
// 64-byte aligned buffer--> VRAM texture; CLUT (CT32, CSM1, bit 3/4 swapped) only when the palette changed;
// one textured sprite into the back frame buffer; FINISH. The CRTC flip (DISPFB2) is done by the vblank
// interrupt handler once the GS reports FINISH, so the main thread never spins on vsync.
// gsKit is used only for mode setup and VRAM bookkeeping; none of its queues or blocking transfers.

#include <tamtypes.h>
#include <kernel.h>
#include <malloc.h>
#include <stdint.h>
#include <string.h>
#include <gsKit.h>
#include <dmaKit.h>
#include <rom0_info.h>

#include "ps2_gs.h"

// PSMT8 pages are 128x64 px (8 KB). TBW counts 64 px but an 8-bit buffer is addressed in whole pages: 320 px -> 384 -> TBW=6
// (TBW=5 puts the third page column of each row on the first page of the next one: the right 64 px are overwritten).
#define TEX_TBW 6
#define TEX_PAGES 12 // 3 page columns x 4 page rows (200 px)
#define TEX_BYTES (TEX_PAGES * 8192)
#define CLUT_BYTES 8192 // 1 KB used, a whole page so nothing else shares it

// GS registers (A+D addresses)
#define R_PRIM 0x00
#define R_RGBAQ 0x01
#define R_UV 0x03
#define R_XYZ2 0x05
#define R_TEX0_1 0x06
#define R_CLAMP_1 0x08
#define R_XYOFFSET_1 0x18
#define R_TEX1_1 0x14
#define R_TEXFLUSH 0x3f
#define R_SCISSOR_1 0x40
#define R_DTHE 0x45
#define R_COLCLAMP 0x46
#define R_TEST_1 0x47
#define R_PABE 0x49
#define R_FBA_1 0x4a
#define R_FRAME_1 0x4c
#define R_ZBUF_1 0x4e
#define R_BITBLTBUF 0x50
#define R_TRXPOS 0x51
#define R_TRXREG 0x52
#define R_TRXDIR 0x53
#define R_FINISH 0x61

// DMA channel 2 (GIF)
#define D2_CHCR ((volatile u32 *)0x1000A000)
#define D2_MADR ((volatile u32 *)0x1000A010)
#define D2_QWC ((volatile u32 *)0x1000A020)
#define D2_TADR ((volatile u32 *)0x1000A030)
#define CHCR_START_CHAIN 0x105 // DIR=1 (from memory), MOD=chain, STR
#define CHCR_STR 0x100

#define TAG_CNT 1
#define TAG_REF 3
#define TAG_END 7

#define SPIN_LIMIT (294912000u / 20u) // 50 ms of EE cycles; bounded spins never hang the game

#define CHAIN_U64 256

static u64 chain[CHAIN_U64] __attribute__((aligned(64)));
static u32 clut[256] __attribute__((aligned(64)));
static u8 *staging; // only used when the caller's buffer is not 64-byte aligned

static struct
{
	GSGLOBAL *gs;
	int up;
	int mode, fbw, fbh;
	u32 fb[2];
	u32 tex, clut_vram;
	int sema, intc;
	int dx, dy, dw, dh, linear;
	int clut_dirty;
	volatile int disp, pend, pend_buf;
	volatile u32 vbl, flips;
	ps2gs_stats_t st;
} g;

int ps2gs_dbg_noswap;

static inline u32 count(void)
{
	u32 v;
	__asm__ volatile("mfc0 %0,$9" : "=r"(v));
	return v;
}

static inline u32 phys(const void *p)
{
	return (u32)(uintptr_t)p & 0x0FFFFFFFu;
}

static u64 *ad(u64 *p, u64 data, u32 reg)
{
	*p++ = data;
	*p++ = reg;
	return p;
}

static u64 *giftag(u64 *p, u32 nloop, u32 eop, u32 flg, u32 nreg, u64 regs)
{
	*p++ = (u64)nloop | ((u64)eop << 15) | ((u64)flg << 58) | ((u64)nreg << 60);
	*p++ = regs;
	return p;
}

static void set_dmatag(u64 *t, u32 id, u32 qwc, u32 addr)
{
	t[0] = (u64)qwc | ((u64)id << 28) | ((u64)(addr & 0x7FFFFFFFu) << 32);
	t[1] = 0;
}

static u64 *dmatag(u64 *p, u32 id, u32 qwc, u32 addr)
{
	set_dmatag(p, id, qwc, addr);
	return p + 2;
}

// NLOOP of a PACKED A+D giftag = the A+D entries written after it
static void fix_nloop(u64 *gt, const u64 *end)
{
	gt[0] |= (u64)((end - gt - 2) / 2);
}

static u64 vertex(int x, int y)
{
	return (u64)((2048 + x) * 16) | ((u64)((2048 + y) * 16) << 16);
}

// state that must be valid for every draw (cheap, so it is resent with each frame)
static u64 *draw_env(u64 *p, u32 fb)
{
	p = ad(p, (u64)(2048 * 16) | ((u64)(2048 * 16) << 32), R_XYOFFSET_1);
	p = ad(p, 0 | ((u64)(g.fbw - 1) << 16) | ((u64)0 << 32) | ((u64)(g.fbh - 1) << 48), R_SCISSOR_1);
	p = ad(p, (u64)1 << 32, R_ZBUF_1); // ZMSK=1: the Z buffer is never written
	p = ad(p, (1u << 16) | (1u << 17), R_TEST_1); // ZTE=1, ZTST=ALWAYS, no alpha test
	p = ad(p, 1, R_COLCLAMP);
	p = ad(p, 0, R_DTHE);
	p = ad(p, 0, R_PABE);
	p = ad(p, 0, R_FBA_1);
	p = ad(p, (u64)(fb / 8192) | ((u64)(g.fbw / 64) << 16) | ((u64)GS_PSM_CT32 << 24), R_FRAME_1);
	return p;
}

#define BITBLTBUF(dbp, dbw, dpsm) (((u64)((dbp) / 256) << 32) | ((u64)(dbw) << 48) | ((u64)(dpsm) << 56))

// host -> local upload header: PACKED A+D block + IMAGE giftag, as one CNT segment
static u64 *upload_header(u64 *p, u32 vram, u32 tbw, u32 psm, u32 w, u32 h, u32 qwords)
{
	u64 *t = p;
	p += 2;
	p = giftag(p, 4, 0, 0, 1, 0xE);
	p = ad(p, BITBLTBUF(vram, tbw, psm), R_BITBLTBUF);
	p = ad(p, 0, R_TRXPOS);
	p = ad(p, (u64)w | ((u64)h << 32), R_TRXREG);
	p = ad(p, 0, R_TRXDIR);
	p = giftag(p, qwords, 0, 2, 0, 0);
	set_dmatag(t, TAG_CNT, (u32)(p - t - 2) / 2, 0);
	return p;
}

static void dma_start(void)
{
	*D2_QWC = 0;
	*D2_TADR = phys(chain);
	__asm__ volatile("sync.l");
	*D2_CHCR = CHCR_START_CHAIN;
	__asm__ volatile("sync.p");
}

// true when the GIF DMA drained (bounded)
static int dma_wait(void)
{
	u32 t0 = count();
	while (*D2_CHCR & CHCR_STR)
	{
		if ((u32)(count() - t0) > SPIN_LIMIT)
		{
			g.st.timeouts++;
			return 0;
		}
	}
	return 1;
}

static int finish_wait(void)
{
	u32 t0 = count();
	while (!(*GS_CSR & 2))
	{
		if ((u32)(count() - t0) > SPIN_LIMIT)
		{
			g.st.timeouts++;
			return 0;
		}
	}
	return 1;
}

// Used only while bringing the GS up: fill a frame buffer with black and wait for the GS.
static void clear_fb(u32 fb)
{
	u64 *p = chain + 2;
	u64 *t = chain;
	u64 *gt = p;
	p = giftag(p, 0, 1, 0, 1, 0xE);
	p = draw_env(p, fb);
	p = ad(p, 6, R_PRIM); // sprite, no texture
	p = ad(p, 0x80000000u | ((u64)0x3f800000 << 32), R_RGBAQ);
	p = ad(p, vertex(0, 0), R_XYZ2);
	p = ad(p, vertex(g.fbw, g.fbh), R_XYZ2);
	p = ad(p, 0, R_FINISH);
	fix_nloop(gt, p);
	set_dmatag(t, TAG_END, (u32)(p - t - 2) / 2, 0);
	FlushCache(0);
	dma_wait();
	*GS_CSR = 2;
	dma_start();
	dma_wait();
	finish_wait();
}

static int vblank_handler(int cause)
{
	(void)cause;
	g.vbl++;
	if (g.pend && (*GS_CSR & 2)) // the frame is completely drawn: show it
	{
		int b = g.pend_buf;
		GS_SET_DISPFB2(g.fb[b] / 8192, g.fbw / 64, GS_PSM_CT32, 0, 0);
		g.disp = b;
		g.pend = 0;
		g.flips++;
	}
	iSignalSema(g.sema);
	ExitHandler();
	return 0;
}

static int detect_pal(void)
{
	char rom[16];

	memset(rom, 0, sizeof rom);
	GetRomName(rom); // "0200EC20040614": index 4 is the region letter, 'E' = Europe
	return rom[4] == 'E';
}

int ps2gs_init(int mode)
{
	GSGLOBAL *gs;
	ee_sema_t sema;
	int i;

	if (g.up)
		return 0;
	if (mode == PS2GS_MODE_AUTO)
		mode = detect_pal() ? PS2GS_MODE_PAL : PS2GS_MODE_NTSC;

	memset(&g, 0, sizeof g);
	g.mode = mode;
	g.fbw = 640;
	g.linear = 0;

	dmaKit_init(D_CTRL_RELE_OFF, D_CTRL_MFD_OFF, D_CTRL_STS_UNSPEC, D_CTRL_STD_OFF, D_CTRL_RCYC_8, 1 << DMA_CHANNEL_GIF);
	dmaKit_chan_init(DMA_CHANNEL_GIF);

	gs = gsKit_init_global_custom(32 * 1024, 32 * 1024); // gsKit's own draw queues are never used
	if (!gs)
		return -1;
	switch (mode)
	{
		case PS2GS_MODE_PAL:
			gs->Mode = GS_MODE_PAL;
			gs->Interlace = GS_INTERLACED;
			gs->Field = GS_FIELD;
			g.fbh = 512;
			break;
		case PS2GS_MODE_480P:
			gs->Mode = GS_MODE_DTV_480P;
			gs->Interlace = GS_NONINTERLACED;
			gs->Field = GS_FRAME;
			g.fbh = 480;
			break;
		default:
			gs->Mode = GS_MODE_NTSC;
			gs->Interlace = GS_INTERLACED;
			gs->Field = GS_FIELD;
			g.fbh = 448;
			break;
	}
	gs->Width = g.fbw;
	gs->Height = g.fbh;
	gs->PSM = GS_PSM_CT32;
	gs->DoubleBuffering = GS_SETTING_ON;
	gs->ZBuffering = GS_SETTING_OFF;
	gs->Dithering = GS_SETTING_OFF;
	gsKit_init_screen(gs);
	gsKit_mode_switch(gs, GS_ONESHOT);
	g.gs = gs;

	g.fb[0] = gs->ScreenBuffer[0];
	g.fb[1] = gs->ScreenBuffer[1];
	g.tex = gsKit_vram_alloc(gs, TEX_BYTES, GSKIT_ALLOC_SYSBUFFER);
	g.clut_vram = gsKit_vram_alloc(gs, CLUT_BYTES, GSKIT_ALLOC_SYSBUFFER);
	if (g.fb[1] == 0 || g.fb[1] == g.fb[0] || g.tex == GSKIT_ALLOC_ERROR || g.clut_vram == GSKIT_ALLOC_ERROR
		|| (g.tex & 8191) || (g.clut_vram & 8191))
		return -2; // VRAM did not fit or is not page aligned

	g.dx = 0;
	g.dy = 0;
	g.dw = g.fbw;
	g.dh = g.fbh;

	*GS_IMR = 0xFF00; // no GS interrupts, only vblank is used
	clear_fb(g.fb[1]);
	clear_fb(g.fb[0]);
	GS_SET_DISPFB2(g.fb[0] / 8192, g.fbw / 64, GS_PSM_CT32, 0, 0);
	g.disp = 0;

	sema.init_count = 0;
	sema.max_count = 1;
	sema.option = 0;
	g.sema = CreateSema(&sema);
	if (g.sema < 0)
		return -3;

	for (i = 0; i < 256; i++)
		clut[i] = 0x80000000u;
	g.clut_dirty = 1;

	g.intc = AddIntcHandler(INTC_VBLANK_S, vblank_handler, 0);
	EnableIntc(INTC_VBLANK_S);
	g.up = 1;
	return 0;
}

void ps2gs_shutdown(void)
{
	if (!g.up)
		return;
	g.up = 0;
	dma_wait();
	DisableIntc(INTC_VBLANK_S);
	RemoveIntcHandler(INTC_VBLANK_S, g.intc);
	DeleteSema(g.sema);
}

int ps2gs_is_up(void)
{
	return g.up;
}

void ps2gs_set_palette(const unsigned int *rgb)
{
	int i;

	for (i = 0; i < 256; i++)
	{
		// CT32 CLUT in CSM1 swaps bits 3 and 4 of the index (a PSMT8 index addresses blocks of 8 entries)
		unsigned idx = ps2gs_dbg_noswap ? (unsigned)i : (((unsigned)i & ~0x18u) | (((unsigned)i & 0x08u) << 1) | (((unsigned)i & 0x10u) >> 1));
		clut[idx] = (rgb[i] & 0x00FFFFFFu) | 0x80000000u;
	}
	g.clut_dirty = 1;
}

void ps2gs_set_dest(int x, int y, int w, int h)
{
	g.dx = x;
	g.dy = y;
	g.dw = w;
	g.dh = h;
}

void ps2gs_set_filter(int linear)
{
	g.linear = linear ? 1 : 0;
}

void ps2gs_wait_vblank(int n)
{
	if (!g.up)
		return;
	while (n-- > 0)
	{
		while (PollSema(g.sema) == g.sema)
			; // drop vblanks that passed already
		WaitSema(g.sema);
	}
}

// Everything the GS needs for one frame, in one chain:
//   [CLUT image] [index image] state, TEXFLUSH, sprite, FINISH
static void build_frame(const u8 *frame, u32 fb, int with_clut)
{
	u64 *p = chain;
	u64 *t, *gt;
	u32 clut_tex = g.clut_vram / 256;

	if (with_clut)
	{
		p = upload_header(p, g.clut_vram, 1, GS_PSM_CT32, 16, 16, 64);
		p = dmatag(p, TAG_REF, 64, phys(clut));
	}
	p = upload_header(p, g.tex, TEX_TBW, GS_PSM_T8, PS2GS_FRAME_W, PS2GS_FRAME_H, PS2GS_FRAME_BYTES / 16);
	p = dmatag(p, TAG_REF, PS2GS_FRAME_BYTES / 16, phys(frame));

	t = p;
	p += 2;
	gt = p;
	p = giftag(p, 0, 1, 0, 1, 0xE);
	p = ad(p, 0, R_TEXFLUSH);
	p = draw_env(p, fb);
	p = ad(p, (u64)(g.tex / 256) | ((u64)TEX_TBW << 14) | ((u64)GS_PSM_T8 << 20) | ((u64)9 << 26) | ((u64)8 << 30) | ((u64)1 << 34) | ((u64)1 << 35)
		| ((u64)clut_tex << 37) | ((u64)GS_PSM_CT32 << 51) | ((u64)1 << 61), R_TEX0_1); // TW=512 TH=256, DECAL, load CLUT
	p = ad(p, g.linear ? ((1u << 5) | (1u << 6)) : 0, R_TEX1_1);
	p = ad(p, 2 | (2 << 2) | ((u64)0 << 4) | ((u64)(PS2GS_FRAME_W - 1) << 14) | ((u64)0 << 24) | ((u64)(PS2GS_FRAME_H - 1) << 34), R_CLAMP_1); // region clamp
	p = ad(p, 6 | (1 << 4) | (1 << 8), R_PRIM); // sprite, TME, FST
	p = ad(p, 0x80808080u | ((u64)0x3f800000 << 32), R_RGBAQ);
	p = ad(p, 0, R_UV);
	p = ad(p, vertex(g.dx, g.dy), R_XYZ2);
	p = ad(p, (u64)(PS2GS_FRAME_W * 16) | ((u64)(PS2GS_FRAME_H * 16) << 16), R_UV);
	p = ad(p, vertex(g.dx + g.dw, g.dy + g.dh), R_XYZ2);
	p = ad(p, 0, R_FINISH);
	fix_nloop(gt, p);
	set_dmatag(t, TAG_END, (u32)(p - t - 2) / 2, 0);
}

void ps2gs_present(const unsigned char *frame, int wait_flip)
{
	u32 t0, t1, tb;
	int b, with_clut;

	if (!g.up)
		return;
	t0 = count();

	if (!frame)
		return;
	if ((uintptr_t)frame & 63) // DMA wants 16, we promise 64
	{
		if (!staging)
			staging = memalign(64, PS2GS_FRAME_BYTES);
		if (!staging)
			return;
		memcpy(staging, frame, PS2GS_FRAME_BYTES);
		frame = staging;
	}

	if (wait_flip && g.pend)
	{
		g.st.waited++;
		ps2gs_wait_vblank(1); // at most one vblank, then the frame is replaced if it still waits
	}
	if (g.pend) // its buffer is about to be reused: the previous draw has to be complete
		finish_wait();

	with_clut = g.clut_dirty;
	g.clut_dirty = 0;
	dma_wait();
	tb = count();
	for (;;)
	{
		u32 irq;

		b = g.disp ^ 1; // the buffer the CRTC is not reading
		build_frame(frame, g.fb[b], with_clut);
		FlushCache(0); // chain, CLUT and frame to RAM: dirty D-cache lines are what the DMA must see
		t1 = count();
		irq = DIntr();
		if ((g.disp ^ 1) == b)
		{
			if (g.pend)
				g.st.dropped++;
			*GS_CSR = 2; // clear FINISH; the handler flips only after this frame's FINISH
			g.pend_buf = b;
			g.pend = 1;
			dma_start();
			if (irq)
				EIntr();
			break;
		}
		if (irq)
			EIntr(); // the flip happened while the chain was built: rebuild for the other buffer
	}
	if ((u32)(t1 - tb) > g.st.flush_max)
		g.st.flush_max = t1 - tb;
	g.st.frames++;

	t1 = count();
	dma_wait(); // the caller will draw into the frame again: wait until the GIF has read it
	if ((u32)(count() - t1) > g.st.dma_wait_max)
		g.st.dma_wait_max = count() - t1;
	if ((u32)(count() - t0) > g.st.present_max)
		g.st.present_max = count() - t0;
}

int ps2gs_fb_width(void)
{
	return g.fbw;
}

int ps2gs_fb_height(void)
{
	return g.fbh;
}

int ps2gs_mode(void)
{
	return g.mode;
}

unsigned int ps2gs_fb_block(int index)
{
	return g.fb[index & 1] / 256;
}

int ps2gs_displayed(void)
{
	return g.disp;
}

int ps2gs_flip_pending(void)
{
	return g.pend;
}

void ps2gs_get_stats(ps2gs_stats_t *out)
{
	*out = g.st;
	out->flips = g.flips;
	out->vblanks = g.vbl;
}
