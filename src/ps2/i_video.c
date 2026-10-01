// SONIC ROBO BLAST 2 - PS2 port
//-----------------------------------------------------------------------------
// Copyright (C) 1993-1996 by id Software, Inc.
// Copyright (C) 1998-2000 by DooM Legacy Team.
// Copyright (C) 1999-2024 by Sonic Team Junior.
//
// This program is free software distributed under the
// terms of the GNU General Public License, version 2.
// See the 'LICENSE' file for more details.
//-----------------------------------------------------------------------------
/// \file  i_video.c
/// \brief PS2 video interface: the software renderer draws the 320x200x8 index frame, the GS only displays it.

#include <malloc.h>
#include <string.h>

#include "../doomdef.h"
#include "../doomstat.h"
#include "../i_system.h"
#include "../v_video.h"
#include "../m_argv.h"
#include "../s_sound.h"
#include "../g_game.h"
#include "../i_video.h"
#include "../console.h"
#include "../command.h"
#include "../netcode/d_netcmd.h" // cv_showping
#include "../netcode/tic_command.h" // simulated_lag

#include "ps2_gs.h"

rendermode_t rendermode = render_none;
rendermode_t chosenrendermode = render_none;

boolean allow_fullscreen = false;

// Off by default: D_SRB2Loop skips its frame-rate sleep when vid_wait is On and fpscap is "Match refresh rate", counting
// on the buffer swap to block. Here nothing blocks (35 Hz tics, no interpolation), so On would spin the main loop.
consvar_t cv_vidwait = CVAR_INIT ("vid_wait", "Off", CV_SAVE, CV_OnOff, NULL);

UINT8 graphics_started = 0; // Is used in console.c and screen.c

// The engine runs its logic on a 35 Hz timer; the display runs on vblank. Interpolation is off: 35 means "no frame
// cap above the tic rate" to R_GetFramerateCap(), so the renderer only draws on a new tic.
#define PS2_REFRESH_RATE 35

#define SCREENS_BYTES ((size_t)NUMSCREENS * PS2GS_FRAME_BYTES)

static void Impl_VideoSetupBuffer(void)
{
	// vid.buffer is the only place the engine takes screens[] from (V_Init), so the alignment is decided here:
	// 64-byte base and a 64000-byte slice, every screens[i] is aligned for DMA. Kept across mode changes so that
	// screens[] never dangle.
	if (vid.buffer)
		return;
	vid.buffer = memalign(64, SCREENS_BYTES);
	if (!vid.buffer)
		I_Error("%s", M_GetText("Not enough memory for video buffer\n"));
	memset(vid.buffer, 0, SCREENS_BYTES);
}

void I_StartupGraphics(void)
{
	int gsmode = PS2GS_MODE_AUTO;

	if (dedicated)
	{
		rendermode = render_none;
		return;
	}
	if (graphics_started)
		return;

	CV_RegisterVar(&cv_vidwait);

	// Software is the only renderer there is
	chosenrendermode = render_soft;
	rendermode = render_soft;

	if (M_CheckParm("-ntsc"))
		gsmode = PS2GS_MODE_NTSC;
	else if (M_CheckParm("-pal"))
		gsmode = PS2GS_MODE_PAL;
	else if (M_CheckParm("-480p"))
		gsmode = PS2GS_MODE_480P;

	vid.modenum = 0;
	vid.width = BASEVIDWIDTH;
	vid.height = BASEVIDHEIGHT;
	vid.bpp = 1;
	vid.rowbytes = vid.width * vid.bpp;
	vid.recalc = true;
	vid.direct = NULL;
	vid.WndParent = NULL;

	Impl_VideoSetupBuffer();

	if (ps2gs_init(gsmode) != 0)
		I_Error("PS2 GS init failed\n");
	if (M_CheckParm("-linear"))
		ps2gs_set_filter(1);

	// as the SDL path: sets vid.*, the buffer and the drawer function pointers right away
	VID_SetMode(VID_GetModeForSize(BASEVIDWIDTH, BASEVIDHEIGHT));

	graphics_started = true;
}

void I_ShutdownGraphics(void)
{
	rendermode = render_none;

	// was graphics initialized anyway?
	if (!graphics_started)
		return;
	graphics_started = false;

	// vid.buffer stays: the engine may still touch screens[] while it quits
	ps2gs_shutdown();
}

void VID_StartupOpenGL(void)
{
}

void VID_CheckGLLoaded(rendermode_t oldrender)
{
	(void)oldrender;
}

void I_SetPalette(RGBA_t *palette)
{
	UINT32 rgb[256];
	size_t i;

	for (i = 0; i < 256; i++)
		rgb[i] = palette[i].s.red | ((UINT32)palette[i].s.green << 8) | ((UINT32)palette[i].s.blue << 16);
	ps2gs_set_palette(rgb);
}

INT32 VID_NumModes(void)
{
	return 1;
}

const char *VID_GetModeName(INT32 modeNum)
{
	if (modeNum == 0 || modeNum == -1)
		return "320x200";
	return NULL;
}

INT32 VID_GetModeForSize(INT32 w, INT32 h)
{
	// the only mode is the closest one to anything
	(void)w;
	(void)h;
	return 0;
}

void VID_PrepareModeList(void)
{
}

boolean VID_CheckRenderer(void)
{
	boolean rendererchanged = false;

	if (dedicated)
		return false;

	if (setrenderneeded)
	{
		rendererchanged = ((rendermode_t)setrenderneeded != render_soft || rendermode != render_soft);
		setrenderneeded = 0; // there is no other renderer to switch to
		rendermode = render_soft;
	}

	Impl_VideoSetupBuffer();
	SCR_SetDrawFuncs();

	return rendererchanged;
}

INT32 VID_SetMode(INT32 modeNum)
{
	(void)modeNum; // 320x200 only

	vid.recalc = 1;
	vid.bpp = 1;
	vid.width = BASEVIDWIDTH;
	vid.height = BASEVIDHEIGHT;
	vid.rowbytes = vid.width * vid.bpp;
	vid.modenum = 0;

	VID_CheckRenderer();
	return 1;
}

UINT32 I_GetRefreshRate(void)
{
	return PS2_REFRESH_RATE;
}

void I_UpdateNoBlit(void)
{
}

void I_FinishUpdate(void)
{
	if (rendermode == render_none || !graphics_started)
		return;

	SCR_CalculateFPS();

	if (marathonmode)
		SCR_DisplayMarathonInfo();

	// draw captions if enabled
	if (cv_closedcaptioning.value)
		SCR_ClosedCaptions();

	if (cv_ticrate.value)
		SCR_DisplayTicRate();

	if (cv_showping.value && (
		(netgame && consoleplayer != serverplayer)
		|| (simulated_lag != 0 && consoleplayer == serverplayer && Playing())
	))
		SCR_DisplayLocalPing();

	if (screens[0])
		ps2gs_present(screens[0], cv_vidwait.value);
}

void I_UpdateNoVsync(void)
{
	INT32 real_vidwait = cv_vidwait.value;
	cv_vidwait.value = 0;
	I_FinishUpdate();
	cv_vidwait.value = real_vidwait;
}

void I_ReadScreen(UINT8 *scr)
{
	if (rendermode != render_soft)
		I_Error("I_ReadScreen: called while in non-software mode");
	else
		VID_BlitLinearScreen(screens[0], scr,
			vid.width*vid.bpp, vid.height,
			vid.rowbytes, vid.rowbytes);
}

// I_WaitVBL, I_BeginRead and I_EndRead live in i_system.c
