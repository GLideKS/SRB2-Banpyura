/* Build three translation units from this file. The reference compiles the
 * actual non-profile eager renderer and owns a different LUT from the candidate.
 * Source sections are copied verbatim to build output by blend_hosttest.py;
 * there are no copied/reimplemented blend or nearest-colour algorithms here. */
#ifdef BLEND_REFERENCE
#define R_InitTranslucencyTables Ref_InitTranslucencyTables
#define R_GenerateBlendTables Ref_GenerateBlendTables
#define R_GetTranslucencyTable Ref_GetTranslucencyTable
#define R_GetBlendTable Ref_GetBlendTable
#define R_BlendLevelVisible Ref_BlendLevelVisible
#define transtables ref_transtables
#define blendtables ref_blendtables
#elif !defined(BLEND_SUPPORT)
#define PS2_PROFILE
#endif

#include "doomdef.h"
#include "doomstat.h"
#include "r_draw.h"
#include "r_data.h"
#include "v_video.h"
#include "w_wad.h"
#include "z_zone.h"
#include "m_argv.h"
#include <io.h>
#include <fcntl.h>

#ifdef BLEND_SUPPORT
#include "blend_support.inc"
#else
#include "blend_renderer.inc"

#ifndef BLEND_REFERENCE
#include "blend_hosttest_alloc.h"

RGBA_t *pMasterPalette;
static UINT8 trans_fixture[9][65536];
static RGBA_t palette[256], later_palette[256];
static size_t failed_tables;

void Ref_InitTranslucencyTables(void);
UINT8 *Ref_GetBlendTable(int style, INT32 level);
UINT8 *Ref_GetTranslucencyTable(INT32 level);

INT32 M_CheckParm(const char *name) { (void)name; return 0; }
lumpnum_t W_GetNumForName(const char *name)
{
	host_check(strlen(name) == 7 && !strncmp(name, "TRANS", 5) && name[6] == '0'
		&& name[5] >= '1' && name[5] <= '9', "TRANS lump name");
	return (lumpnum_t)(name[5] - '1');
}
void W_ReadLump(lumpnum_t lump, void *dest)
{
	host_check(lump < 9, "TRANS fixture index");
	memcpy(dest, trans_fixture[lump], 65536);
}

static void reset_candidate(void)
{
	for (INT32 tab = 0; tab < NUMBLENDMAPS; tab++)
		for (INT32 i = 0; i < BlendTab_Count[tab]; i++)
		{
			Z_Free(blendtab_lazy[tab][i]);
			blendtab_lazy[tab][i] = NULL;
		}
	Z_Free(transtab_lutp); // explicit loss: the next request must reconstruct canonical state
	host_check(transtab_lutp == NULL, "LUT owner cleared");
}

static void scenario(const char *name, INT32 order[31], int purge, int reconstruct)
{
	const int styles[4] = {AST_ADD, AST_SUBTRACT, AST_REVERSESUBTRACT, AST_MODULATE};
	size_t differences = 0, edges = 0, bad = 0;
	reset_candidate();
	host_poison = 0x5A; // deliberately different from reference allocator contents
	for (INT32 k = 0; k < 31; k++)
	{
		INT32 index = order[k], tab = index < 30 ? index / 10 : 3, level = index < 30 ? index % 10 : 0;
		if (reconstruct)
			Z_Free(transtab_lutp);
		if (purge)
		{
			colorlookup_t *before = transtab_lutp;
			host_purge_cache();
			host_check(transtab_lutp == before, "canonical LUT survives cache purge");
		}
		UINT8 *candidate = R_GetBlendTable(styles[tab], level);
		UINT8 *reference = Ref_GetBlendTable(styles[tab], level);
		size_t table_diffs = 0;
		for (INT32 bg = 0; bg < 256; bg++)
			for (INT32 fg = 0; fg < 256; fg++)
			{
				size_t offset = (size_t)bg * 256 + fg;
				if (bg == 255 || fg == 255)
				{
					// Measure inherited unwritten bytes, do not give them new semantics.
					host_check(reference[offset] == 0xA5 && candidate[offset] == 0x5A,
						"only inherited index-255 edges remain allocation poison");
					edges++;
				}
				else if (candidate[offset] != reference[offset])
					table_diffs++;
			}
		differences += table_diffs;
		bad += table_diffs != 0;
		host_check(pMasterPalette == later_palette, "lazy generation restores current palette");
	}
	host_check(edges == 31 * 511, "exactly 511 inherited bytes excluded per table");
	printf("%s: 31 tables, %zu defined-byte differences in %zu tables; %zu measured undefined edge bytes\n",
		name, differences, bad, edges);
	failed_tables += bad;
}

int main(void)
{
	UINT8 rgb[768];
	INT32 order[31];
	_setmode(_fileno(stdin), _O_BINARY);
	host_check(fread(rgb, 1, sizeof rgb, stdin) == sizeof rgb, "vanilla PLAYPAL input");
	host_check(fread(trans_fixture, 1, sizeof trans_fixture, stdin) == sizeof trans_fixture, "nine vanilla TRANS inputs");
	for (INT32 i = 0; i < 256; i++)
	{
		palette[i].s.red = rgb[3*i]; palette[i].s.green = rgb[3*i+1]; palette[i].s.blue = rgb[3*i+2];
		palette[i].s.alpha = 255;
	}
	for (INT32 i = 0; i < 256; i++)
		later_palette[i] = palette[255-i];
	pMasterPalette = palette;
	host_poison = 0xA5;
	Ref_InitTranslucencyTables();
	R_InitTranslucencyTables();
	pMasterPalette = later_palette; // lazy tables must use the init-time palette, not this one
	for (INT32 i = 0; i < 31; i++) order[i] = i;
	scenario("forward/cold", order, 0, 0);
	for (INT32 i = 0; i < 31; i++) order[i] = 30-i;
	scenario("reverse/cold", order, 0, 0);
	UINT32 seed = 0x12345678;
	for (INT32 i = 30; i > 0; i--)
	{
		seed = seed * 1664525u + 1013904223u;
		INT32 j = (INT32)(seed % (UINT32)(i+1)), swap = order[i];
		order[i] = order[j]; order[j] = swap;
	}
	scenario("shuffled/cold", order, 0, 0);
	if (failed_tables)
	{
		host_free_all();
		printf("Blend equivalence: FAIL\n");
		return 1;
	}
	scenario("shuffled/cache-purge", order, 1, 0);
	scenario("shuffled/LUT-reconstruction-before-every-table", order, 0, 1);
	for (INT32 i = 1; i <= 9; i++)
	{
		UINT8 *table = R_GetTranslucencyTable(i);
		host_check(!memcmp(table, Ref_GetTranslucencyTable(i), 65536), "all nine TRANS tables equal eager reference");
		host_purge_cache();
		host_check(R_GetTranslucencyTable(i) == table, "TRANS table retained across cache purge");
	}
	host_check(R_GetTranslucencyTable(-3) == R_GetTranslucencyTable(1), "low translucency clipping");
	host_check(R_GetTranslucencyTable(99) == R_GetTranslucencyTable(9), "high translucency clipping");
	host_check(R_GetBlendTable(AST_ADD, -3) == R_GetBlendTable(AST_ADD, 0), "low blend clipping");
	host_check(R_GetBlendTable(AST_SUBTRACT, 99) == R_GetBlendTable(AST_SUBTRACT, 9), "high blend clipping");
	host_check(!R_GetBlendTable(AST_COPY, 1) && !R_GetBlendTable(AST_OVERLAY, 1)
		&& !R_GetBlendTable(AST_TRANSLUCENT, 0), "unsupported/copy blend results");
	host_check(R_GetBlendTable(AST_TRANSLUCENT, 3) == R_GetTranslucencyTable(3), "translucent getter fallback");
	printf("TRANS tables, getter clipping, init-palette capture and payload red zones PASS\n");
	if (!failed_tables)
		R_BlendSelfTest();
	host_free_all();
	printf("Blend equivalence: %s\n", failed_tables ? "FAIL" : "PASS");
	return failed_tables ? 1 : 0;
}
#endif
#endif
