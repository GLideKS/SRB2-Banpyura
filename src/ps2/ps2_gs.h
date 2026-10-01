// PS2 GS output for the 320x200x8 software renderer.
// The index frame is uploaded as PSMT8 (+ CLUT, CT32, CSM1) and drawn as one textured sprite
// into the back frame buffer; the flip happens in the vblank interrupt (no busy-wait on vsync).
// Independent of the engine headers so that the hardware test can link it alone.

#ifndef __PS2_GS_H__
#define __PS2_GS_H__


#define PS2GS_FRAME_W 320
#define PS2GS_FRAME_H 200
#define PS2GS_FRAME_BYTES (PS2GS_FRAME_W * PS2GS_FRAME_H) // multiple of 64: every screens[] slice is DMA aligned

enum
{
	PS2GS_MODE_AUTO = -1, // NTSC unless rom0:ROMVER says Europe
	PS2GS_MODE_NTSC = 0, // 640x448 interlaced (field mode)
	PS2GS_MODE_PAL = 1, // 640x512 interlaced (field mode)
	PS2GS_MODE_480P = 2, // 640x480 progressive (component cable)
};

typedef struct
{
	unsigned int frames; // frames submitted
	unsigned int dropped; // frames replaced before they were ever shown
	unsigned int flips; // flips done by the vblank handler
	unsigned int vblanks; // vblank interrupts seen
	unsigned int waited; // present() calls that slept on a vblank
	unsigned int timeouts; // bounded spins that ran out (DMA or GS FINISH)
	unsigned int dma_wait_max; // longest wait for the GIF DMA to drain, EE cycles
	unsigned int flush_max; // longest FlushCache + chain build, EE cycles
	unsigned int present_max; // longest ps2gs_present call, EE cycles
} ps2gs_stats_t;

// Initialise the GS and the vblank handler. Returns 0 on success.
int ps2gs_init(int mode);
void ps2gs_shutdown(void);
int ps2gs_is_up(void);

// 256 entries 0x00BBGGRR (alpha is forced to 0x80). The index bits 3/4 swap of the CT32 CLUT layout is done here.
void ps2gs_set_palette(const unsigned int *rgb);

// Upload and draw one 320x200 index frame (64-byte aligned, 64000 bytes). Never waits for a vblank, except
// when wait_flip is set and an older frame is still waiting for its flip: then it sleeps at most one vblank.
// The frame buffer may be reused by the caller as soon as this returns.
void ps2gs_present(const unsigned char *frame, int wait_flip);

// Sleep (semaphore, no spinning) until count more vblanks passed.
void ps2gs_wait_vblank(int count);

// Destination rectangle on the screen for the frame (default: whole frame buffer); takes effect on the next present.
void ps2gs_set_dest(int x, int y, int w, int h);
// 0 = nearest (default), 1 = bilinear
void ps2gs_set_filter(int linear);

int ps2gs_fb_width(void);
int ps2gs_fb_height(void);
int ps2gs_mode(void);
unsigned int ps2gs_fb_block(int index); // VRAM address of frame buffer index in 256-byte blocks (for GS readback)
int ps2gs_displayed(void); // index of the frame buffer the CRTC reads right now
int ps2gs_flip_pending(void);
void ps2gs_get_stats(ps2gs_stats_t *out);

// Test-only: skip the bit 3/4 swap (negative control).
extern int ps2gs_dbg_noswap;

#endif
