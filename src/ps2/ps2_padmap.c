// DualShock 2 -> SRB2 joystick events. Layout: PLAN section 9 (Cross=JOY1, Circle=JOY2, Square=JOY3,
// Triangle=JOY4, L1/R1=JOY5/6, Select=JOY7, Start=JOY8, L3/R3=JOY9/10, D-pad = hat 0,
// left stick = axis set 0, right stick = axis set 1). L2/R2 (no PC counterpart in the plan) = JOY11/12.
#include "ps2_padmap.h"

#define AXISRANGE 1023 // JOYAXISRANGE (i_joy.h)

static const struct { uint16_t mask; uint8_t index; } keymap[PS2PAD_NUMKEYS] =
{
	{PS2PAD_CROSS,    0},
	{PS2PAD_CIRCLE,   1},
	{PS2PAD_SQUARE,   2},
	{PS2PAD_TRIANGLE, 3},
	{PS2PAD_L1,       4},
	{PS2PAD_R1,       5},
	{PS2PAD_SELECT,   6},
	{PS2PAD_START,    7},
	{PS2PAD_L3,       8},
	{PS2PAD_R3,       9},
	{PS2PAD_L2,      10},
	{PS2PAD_R2,      11},
};

// KEY_HAT1+0..3 are up, down, left, right
static const uint16_t hatmap[PS2PAD_NUMHATS] = {PS2PAD_UP, PS2PAD_DOWN, PS2PAD_LEFT, PS2PAD_RIGHT};

int32_t PS2Pad_Axis(uint8_t v, const ps2pad_cfg_t *cfg)
{
	int32_t raxis;
	int d = (int)v - 128;

	if (d >= -PS2PAD_STICK_DEAD && d <= PS2PAD_STICK_DEAD)
		return 0;

	// 0..255 -> -32768..32767 (the SDL range), then /32 as SDLJoyAxis does: -1024..1023
	raxis = ((int32_t)v * 257 - 32768) / 32;

	if (cfg->gamepadstyle)
	{
		// gamepad control type, on or off
		if (raxis < -(AXISRANGE/2))
			return -1;
		if (raxis > (AXISRANGE/2))
			return 1;
		return 0;
	}

	if (cfg->scale > 1)
		raxis = (raxis / cfg->scale) * cfg->scale;
	return raxis;
}

static int PutKey(ps2pad_event_t *out, int n, int cap, uint8_t kind, uint8_t index, uint8_t down)
{
	if (n >= cap)
		return n;
	out[n].kind = kind;
	out[n].down = down;
	out[n].index = index;
	out[n].x = out[n].y = PS2PAD_AXIS_NONE;
	return n + 1;
}

static int PutAxis(ps2pad_state_t *st, const int32_t nv[4], ps2pad_event_t *out, int n, int cap)
{
	int set;
	for (set = 0; set < 2; set++)
	{
		const int32_t nx = nv[set*2], ny = nv[set*2 + 1];
		const int cx = nx != st->axis[set*2], cy = ny != st->axis[set*2 + 1];
		if (!cx && !cy)
			continue;
		st->axis[set*2] = nx;
		st->axis[set*2 + 1] = ny;
		if (n >= cap)
			continue;
		out[n].kind = PS2PADEV_AXIS;
		out[n].down = 0;
		out[n].index = (uint8_t)set;
		out[n].x = cx ? nx : PS2PAD_AXIS_NONE;
		out[n].y = cy ? ny : PS2PAD_AXIS_NONE;
		n++;
	}
	return n;
}

int PS2Pad_Update(ps2pad_state_t *st, const ps2pad_raw_t *raw, const ps2pad_cfg_t *cfg, ps2pad_event_t *out, int cap)
{
	uint32_t keys = 0, changed;
	uint8_t hats = 0, hchanged;
	int32_t nv[4];
	int i, n = 0;

	for (i = 0; i < PS2PAD_NUMKEYS; i++)
		if (raw->buttons & keymap[i].mask)
			keys |= 1u << keymap[i].index;
	for (i = 0; i < PS2PAD_NUMHATS; i++)
		if (raw->buttons & hatmap[i])
			hats |= (uint8_t)(1u << i);

	changed = keys ^ st->keys;
	st->keys = keys;
	for (i = 0; i < PS2PAD_NUMKEYS; i++)
		if (changed & (1u << i))
			n = PutKey(out, n, cap, PS2PADEV_KEY, (uint8_t)i, (keys >> i) & 1);

	hchanged = (uint8_t)(hats ^ st->hats);
	st->hats = hats;
	for (i = 0; i < PS2PAD_NUMHATS; i++)
		if (hchanged & (1u << i))
			n = PutKey(out, n, cap, PS2PADEV_HAT, (uint8_t)i, (hats >> i) & 1);

	if (raw->analog)
	{
		nv[0] = PS2Pad_Axis(raw->lx, cfg);
		nv[1] = PS2Pad_Axis(raw->ly, cfg);
		nv[2] = PS2Pad_Axis(raw->rx, cfg);
		nv[3] = PS2Pad_Axis(raw->ry, cfg);
	}
	else
		nv[0] = nv[1] = nv[2] = nv[3] = 0;
	return PutAxis(st, nv, out, n, cap);
}

int PS2Pad_Release(ps2pad_state_t *st, ps2pad_event_t *out, int cap)
{
	static const int32_t zero[4] = {0, 0, 0, 0};
	int i, n = 0;

	for (i = 0; i < PS2PAD_NUMKEYS; i++)
		if (st->keys & (1u << i))
			n = PutKey(out, n, cap, PS2PADEV_KEY, (uint8_t)i, 0);
	for (i = 0; i < PS2PAD_NUMHATS; i++)
		if (st->hats & (1u << i))
			n = PutKey(out, n, cap, PS2PADEV_HAT, (uint8_t)i, 0);
	st->keys = 0;
	st->hats = 0;
	return PutAxis(st, zero, out, n, cap);
}
