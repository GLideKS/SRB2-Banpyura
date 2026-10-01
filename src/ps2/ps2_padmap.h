// DualShock 2 -> SRB2 joystick events. Pure logic: no libpad, no engine headers (host-testable).
#ifndef PS2_PADMAP_H
#define PS2_PADMAP_H

#include <stdint.h>

// libpad button bits after inversion (1 = pressed)
#define PS2PAD_SELECT   0x0001
#define PS2PAD_L3       0x0002
#define PS2PAD_R3       0x0004
#define PS2PAD_START    0x0008
#define PS2PAD_UP       0x0010
#define PS2PAD_RIGHT    0x0020
#define PS2PAD_DOWN     0x0040
#define PS2PAD_LEFT     0x0080
#define PS2PAD_L2       0x0100
#define PS2PAD_R2       0x0200
#define PS2PAD_L1       0x0400
#define PS2PAD_R1       0x0800
#define PS2PAD_TRIANGLE 0x1000
#define PS2PAD_CIRCLE   0x2000
#define PS2PAD_CROSS    0x4000
#define PS2PAD_SQUARE   0x8000

#define PS2PAD_NUMKEYS  12          // JOY1..JOY12
#define PS2PAD_NUMHATS  4           // hat 0: up, down, left, right (KEY_HAT1+0..3)
#define PS2PAD_AXIS_NONE 0x7fffffff // INT32_MAX: "this axis did not change" in event_t
#define PS2PAD_MAXEVENTS 24         // worst case per update is 18
#define PS2PAD_STICK_DEAD 4         // bytes around 128 forced to 0 (stick centre noise)

typedef struct
{
	uint16_t buttons;               // 1 = pressed (libpad value inverted)
	uint8_t lx, ly, rx, ry;         // 0..255, centre 128
	uint8_t analog;                 // 0: digital pad, stick bytes are meaningless
} ps2pad_raw_t;

typedef struct
{
	int gamepadstyle;               // Joystick.bGamepadStyle: sticks become -1/0/1
	int scale;                      // JoyInfo.scale (>= 1)
} ps2pad_cfg_t;

typedef struct
{
	uint32_t keys;                  // bit n = JOY(n+1) down
	uint8_t hats;                   // bit n = KEY_HAT1+n down
	int32_t axis[4];                // lx, ly, rx, ry as last sent
} ps2pad_state_t;

typedef enum { PS2PADEV_KEY, PS2PADEV_HAT, PS2PADEV_AXIS } ps2pad_evkind_t;

typedef struct
{
	uint8_t kind;                   // ps2pad_evkind_t
	uint8_t down;                   // KEY/HAT: 1 = keydown, 0 = keyup
	uint8_t index;                  // KEY: 0.. (KEY_JOY1+index); HAT: 0..3; AXIS: set 0 (left stick) or 1 (right)
	int32_t x, y;                   // AXIS only; PS2PAD_AXIS_NONE = unchanged
} ps2pad_event_t;

// Stick byte -> SRB2 axis value, same scaling as sdl/i_video.c SDLJoyAxis.
int32_t PS2Pad_Axis(uint8_t v, const ps2pad_cfg_t *cfg);

// Diff the new pad state against `st`, update `st`, write events to `out`. Returns the number of events.
int PS2Pad_Update(ps2pad_state_t *st, const ps2pad_raw_t *raw, const ps2pad_cfg_t *cfg, ps2pad_event_t *out, int cap);

// Pad gone: key-ups for everything held, zeroed axes.
int PS2Pad_Release(ps2pad_state_t *st, ps2pad_event_t *out, int cap);

// Self-test on synthetic data; prints failures with printf and returns their number.
int PS2Pad_SelfTest(void);

#endif
