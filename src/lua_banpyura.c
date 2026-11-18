// SONIC ROBO BLAST 2
//-----------------------------------------------------------------------------
// Copyright (C) 2026 by GLide KS, romoney5.
//
// This program is free software distributed under the
// terms of the GNU General Public License, version 2.
// See the 'LICENSE' file for more details.
//-----------------------------------------------------------------------------
/// \file  lua_banpyura.c
/// \brief Custom Lua functions for SRB2 Banpyura

#include "lua_script.h"
#include "lua_libs.h"

angle_t Banpyura_SpriteShadow_Angle = 0;
boolean Banpyura_SpriteShadow_SnapToCamera = true;

///////////////
// FUNCTIONS //
///////////////

// Banpyura.SpriteShadow_SetAngle(angle_t angle?)
static int lib_SpriteShadow_SetAngle(lua_State *L)
{
	if (!lua_isnoneornil(L, 1))
	{
		Banpyura_SpriteShadow_Angle = luaL_checkangle(L, 1);
		Banpyura_SpriteShadow_SnapToCamera = false;
	}
	else
	{
		Banpyura_SpriteShadow_Angle = 0;
		Banpyura_SpriteShadow_SnapToCamera = true;
	}

	return 0;
}


static luaL_Reg Banpyura[] = {
	{"SpriteShadow_SetAngle", lib_SpriteShadow_SetAngle},
	{NULL, NULL}
};

int LUA_BanpyuraLib(lua_State *L)
{
	luaL_register(L, "Banpyura", Banpyura);

	return 0;
}
