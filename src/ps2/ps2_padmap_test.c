// Synthetic-data test for ps2_padmap.c. Linked into tools/ps2/sys_test (EE) and, with
// -DPS2PAD_HOST_TEST, built as a stand-alone host program (cl / gcc). Not part of the engine.
#include "ps2_padmap.h"

#include <stdio.h>
#include <string.h>

static int failures;
static int checks;

#define CHECK(cond, ...) \
	do { checks++; if (!(cond)) { failures++; printf("PADMAP FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static ps2pad_raw_t neutral(void)
{
	ps2pad_raw_t r;
	memset(&r, 0, sizeof r);
	r.lx = r.ly = r.rx = r.ry = 128;
	r.analog = 1;
	return r;
}

int PS2Pad_SelfTest(void)
{
	static const struct { uint16_t mask; int index; } keys[] = {
		{PS2PAD_CROSS, 0}, {PS2PAD_CIRCLE, 1}, {PS2PAD_SQUARE, 2}, {PS2PAD_TRIANGLE, 3},
		{PS2PAD_L1, 4}, {PS2PAD_R1, 5}, {PS2PAD_SELECT, 6}, {PS2PAD_START, 7},
		{PS2PAD_L3, 8}, {PS2PAD_R3, 9}, {PS2PAD_L2, 10}, {PS2PAD_R2, 11},
	};
	static const struct { uint16_t mask; int index; } hats[] = {
		{PS2PAD_UP, 0}, {PS2PAD_DOWN, 1}, {PS2PAD_LEFT, 2}, {PS2PAD_RIGHT, 3},
	};
	ps2pad_cfg_t analog = {0, 1}, digital = {1, 1}, scaled = {0, 4};
	ps2pad_event_t ev[PS2PAD_MAXEVENTS];
	ps2pad_state_t st;
	ps2pad_raw_t r;
	int i, n;

	failures = checks = 0;

	// neutral pad: nothing
	memset(&st, 0, sizeof st);
	r = neutral();
	CHECK(PS2Pad_Update(&st, &r, &analog, ev, PS2PAD_MAXEVENTS) == 0, "neutral produced events");

	// every button: one keydown on the right JOY index, held = silent, release = keyup
	for (i = 0; i < (int)(sizeof keys / sizeof keys[0]); i++)
	{
		memset(&st, 0, sizeof st);
		r = neutral();
		r.buttons = keys[i].mask;
		n = PS2Pad_Update(&st, &r, &analog, ev, PS2PAD_MAXEVENTS);
		CHECK(n == 1 && ev[0].kind == PS2PADEV_KEY && ev[0].down == 1 && ev[0].index == keys[i].index,
			"button %04x: n=%d kind=%d down=%d index=%d (want %d)", keys[i].mask, n, ev[0].kind, ev[0].down, ev[0].index, keys[i].index);
		CHECK(PS2Pad_Update(&st, &r, &analog, ev, PS2PAD_MAXEVENTS) == 0, "button %04x: held repeated an event", keys[i].mask);
		r.buttons = 0;
		n = PS2Pad_Update(&st, &r, &analog, ev, PS2PAD_MAXEVENTS);
		CHECK(n == 1 && ev[0].kind == PS2PADEV_KEY && ev[0].down == 0 && ev[0].index == keys[i].index, "button %04x: release", keys[i].mask);
	}

	// d-pad -> hat 0 (up, down, left, right)
	for (i = 0; i < (int)(sizeof hats / sizeof hats[0]); i++)
	{
		memset(&st, 0, sizeof st);
		r = neutral();
		r.buttons = hats[i].mask;
		n = PS2Pad_Update(&st, &r, &analog, ev, PS2PAD_MAXEVENTS);
		CHECK(n == 1 && ev[0].kind == PS2PADEV_HAT && ev[0].down == 1 && ev[0].index == hats[i].index, "hat %04x: n=%d index=%d", hats[i].mask, n, ev[0].index);
	}
	memset(&st, 0, sizeof st);
	r = neutral();
	r.buttons = PS2PAD_UP | PS2PAD_RIGHT | PS2PAD_CROSS;
	n = PS2Pad_Update(&st, &r, &analog, ev, PS2PAD_MAXEVENTS);
	CHECK(n == 3, "up+right+cross: %d events", n);

	// sticks: full deflection, centre, deadzone, set mapping
	memset(&st, 0, sizeof st);
	r = neutral();
	r.lx = 255;
	n = PS2Pad_Update(&st, &r, &analog, ev, PS2PAD_MAXEVENTS);
	CHECK(n == 1 && ev[0].kind == PS2PADEV_AXIS && ev[0].index == 0 && ev[0].x == 1023 && ev[0].y == PS2PAD_AXIS_NONE,
		"lx=255: n=%d set=%d x=%d y=%d", n, ev[0].index, (int)ev[0].x, (int)ev[0].y);
	r.lx = 0;
	n = PS2Pad_Update(&st, &r, &analog, ev, PS2PAD_MAXEVENTS);
	CHECK(n == 1 && ev[0].x == -1024, "lx=0: x=%d", (int)ev[0].x);
	r.lx = 128 + PS2PAD_STICK_DEAD;
	n = PS2Pad_Update(&st, &r, &analog, ev, PS2PAD_MAXEVENTS);
	CHECK(n == 1 && ev[0].x == 0, "lx in deadzone: n=%d x=%d", n, (int)ev[0].x);
	r.lx = 128;
	CHECK(PS2Pad_Update(&st, &r, &analog, ev, PS2PAD_MAXEVENTS) == 0, "centre again produced events");
	r.ly = 0; r.ry = 255;
	n = PS2Pad_Update(&st, &r, &analog, ev, PS2PAD_MAXEVENTS);
	CHECK(n == 2 && ev[0].index == 0 && ev[0].x == PS2PAD_AXIS_NONE && ev[0].y == -1024 && ev[1].index == 1 && ev[1].x == PS2PAD_AXIS_NONE && ev[1].y == 1023,
		"ly=0, ry=255: n=%d", n);
	r.rx = 255;
	n = PS2Pad_Update(&st, &r, &analog, ev, PS2PAD_MAXEVENTS);
	CHECK(n == 1 && ev[0].index == 1 && ev[0].x == 1023 && ev[0].y == PS2PAD_AXIS_NONE, "rx=255: n=%d", n);

	// gamepad style: -1/0/1 with a half-range threshold
	memset(&st, 0, sizeof st);
	r = neutral();
	r.lx = 255;
	n = PS2Pad_Update(&st, &r, &digital, ev, PS2PAD_MAXEVENTS);
	CHECK(n == 1 && ev[0].x == 1, "gamepad style lx=255: x=%d", n ? (int)ev[0].x : 99);
	r.lx = 170; // raxis 336 < 511
	n = PS2Pad_Update(&st, &r, &digital, ev, PS2PAD_MAXEVENTS);
	CHECK(n == 1 && ev[0].x == 0, "gamepad style lx=170: n=%d", n);
	r.lx = 0;
	n = PS2Pad_Update(&st, &r, &digital, ev, PS2PAD_MAXEVENTS);
	CHECK(n == 1 && ev[0].x == -1, "gamepad style lx=0: n=%d", n);

	// padscale: values are quantised to multiples of the scale
	memset(&st, 0, sizeof st);
	r = neutral();
	r.lx = 200; // (200*257-32768)/32 = 582; 582/4*4 = 580
	n = PS2Pad_Update(&st, &r, &scaled, ev, PS2PAD_MAXEVENTS);
	CHECK(n == 1 && ev[0].x == 580, "scale 4 lx=200: x=%d", n ? (int)ev[0].x : 0);

	// digital pad: stick bytes are garbage and must be ignored
	memset(&st, 0, sizeof st);
	r = neutral();
	r.analog = 0;
	r.lx = 255; r.ly = 0; r.rx = 0; r.ry = 255;
	CHECK(PS2Pad_Update(&st, &r, &analog, ev, PS2PAD_MAXEVENTS) == 0, "digital pad produced axis events");

	// pad removed: every held thing is released, axes go to zero
	memset(&st, 0, sizeof st);
	r = neutral();
	r.buttons = PS2PAD_CROSS | PS2PAD_START | PS2PAD_LEFT;
	r.lx = 255;
	PS2Pad_Update(&st, &r, &analog, ev, PS2PAD_MAXEVENTS);
	n = PS2Pad_Release(&st, ev, PS2PAD_MAXEVENTS);
	CHECK(n == 4 && ev[0].kind == PS2PADEV_KEY && !ev[0].down && ev[2].kind == PS2PADEV_HAT && !ev[2].down
		&& ev[3].kind == PS2PADEV_AXIS && ev[3].x == 0 && ev[3].y == PS2PAD_AXIS_NONE, "release: n=%d", n);
	CHECK(PS2Pad_Release(&st, ev, PS2PAD_MAXEVENTS) == 0, "second release not empty");

	// worst case: all keys, all hats and both axis sets in one update fits the event buffer
	memset(&st, 0, sizeof st);
	r = neutral();
	r.buttons = 0xffff;
	r.lx = 255; r.ly = 255; r.rx = 0; r.ry = 0;
	n = PS2Pad_Update(&st, &r, &analog, ev, PS2PAD_MAXEVENTS);
	CHECK(n == 18, "worst case: %d events", n);

	printf("PADMAP selftest checks=%d failures=%d\n", checks, failures);
	return failures;
}

#ifdef PS2PAD_HOST_TEST
int main(void)
{
	return PS2Pad_SelfTest() != 0;
}
#endif
