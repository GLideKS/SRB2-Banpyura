// PS2 profile: the Lua VM is not linked. Hooks, HUD draw lists, invalidation and the like are
// inline no-ops in lua_script.h / lua_hook.h / lua_hud*.h / deh_lua.h; this file holds the
// remaining real symbols (variables, event handlers, the save-format part of Lua).

#include "../doomdef.h"
#include "../d_player.h"
#include "../doomstat.h"
#include "../g_game.h"
#include "../p_saveg.h"
#include "../i_system.h"
#include "../lua_script.h"
#include "../lua_libs.h"
#include "../info.h"

INT32 lua_lumploading = 0; // is LUA_LoadLump being called?
INT32 lua_locallyloading = 0; // is this wad file being loaded locally?

state_t *astate; // action state (set by p_enemy.c/p_mobj.c, read only by Lua)

boolean mousegrabbedbylua = true;
boolean ignoregameinputs = false;

// XD_LUACMD / XD_LUAFILE net commands and Lua console commands: nothing can produce them
void Got_Luacmd(UINT8 **cp, INT32 playernum)
{
	(void)cp;
	(void)playernum;
}

void Got_LuaFile(UINT8 **cp, INT32 playernum);
void Got_LuaFile(UINT8 **cp, INT32 playernum)
{
	(void)cp;
	(void)playernum;
}

void COM_Lua_f(void)
{
}

// Same bytes as lua_script.c LUA_Archive/LUA_UnArchive write/read with no Lua data at all:
// one empty ext-var count (UINT16 0) per player in game (player 0 always), then the
// end-of-mobjs marker. The NetVars hook and the tables part add nothing.
void LUA_Archive(save_t *save_p)
{
	INT32 i;

	for (i = 0; i < MAXPLAYERS; i++)
	{
		if (!playeringame[i] && i > 0) // dedicated servers...
			continue;
		P_WriteUINT16(save_p, 0);
	}

	P_WriteUINT32(save_p, UINT32_MAX); // end of mobjs marker
}

void LUA_UnArchive(save_t *save_p)
{
	INT32 i;

	for (i = 0; i < MAXPLAYERS; i++)
	{
		if (!playeringame[i] && i > 0)
			continue;
		if (P_ReadUINT16(save_p) != 0)
			I_Error("Save contains Lua variables, which this build cannot load\n");
	}

	if (P_ReadUINT32(save_p) != UINT32_MAX)
		I_Error("Save contains Lua variables, which this build cannot load\n");
}
