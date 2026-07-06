// SONIC ROBO BLAST 2
//-----------------------------------------------------------------------------
// Copyright (C) 1998-2000 by DooM Legacy Team.
// Copyright (C) 1999-2023 by Sonic Team Junior.
//
// This program is free software distributed under the
// terms of the GNU General Public License, version 2.
// See the 'LICENSE' file for more details.
//-----------------------------------------------------------------------------
/// \file  tic_command.h
/// \brief Tic command handling

#ifndef __D_TIC_COMMAND__
#define __D_TIC_COMMAND__

#define GENTLEMANSMOOTHING (TICRATE)
#define MAXGENTLEMENDELAY (TICRATE)

#include "d_clisrv.h"
#include "../d_ticcmd.h"
#include "../doomdef.h"
#include "../doomtype.h"

extern tic_t firstticstosend; // min of the nettics
extern tic_t tictoclear; // optimize d_clearticcmd

extern ticcmd_t localcmds[MAXGENTLEMENDELAY];
extern ticcmd_t localcmds2[MAXGENTLEMENDELAY];
extern boolean cl_packetmissed;

extern ticcmd_t netcmds[BACKUPTICS][MAXPLAYERS];

extern tic_t lowest_lag;
extern tic_t simulated_lag;

tic_t ExpandTics(INT32 low, INT32 node);
void D_Clearticcmd(tic_t tic);
void D_ResetTiccmds(void);
boolean D_UseLocalDelay(void);

void PT_ClientCmd(SINT8 nodenum, INT32 netconsole);
void PT_ServerTics(SINT8 node, INT32 netconsole);

// send the client packet to the server
void CL_SendClientCmd(void);

void SV_SendTics(void);
void Local_Maketic(INT32 realtics);
void SV_Maketic(void);

#endif
