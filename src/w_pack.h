// SONIC ROBO BLAST 2 (PS2 port)
//-----------------------------------------------------------------------------
// This program is free software distributed under the
// terms of the GNU General Public License, version 2.
// See the 'LICENSE' file for more details.
//-----------------------------------------------------------------------------
/// \file  w_pack.h
/// \brief SRP2 cooked packs (docs/PACK_FORMAT.md): lumpinfo table straight from the pack, LZ4 lump reader

#ifndef __W_PACK__
#define __W_PACK__

#include "doomtype.h"
#include "w_wad.h"

#define WPACK_BLOCK 65536 // decoded block size of lumps bigger than this

// True if the file starts with the pack signature. This performs I/O: set up buffering before calling it.
boolean WPack_Detect(FILE *handle);

// Gives the stream a 64 KiB buffer (64-byte aligned on PS2). Returns the buffer, free() it after fclose().
// Call before ANY I/O, including WPack_Detect; NULL if it could not be allocated. Does not replace driver cache sync.
void *WPack_SetupHandle(FILE *handle);

// Builds lumpinfo_t[] exactly as ResGetLumpsZip does for the pk3 the pack was cooked from (same type RET_PK3,
// same fields; name/longname/fullname point into one string pool, *pool, which is a single Z_Malloc block).
// nonmusic is the cooker's W_VerifyNMUSlumps result (true = the pack has other than music/sound lumps).
// Returns NULL (after a console alert) if the pack is damaged.
lumpinfo_t *WPack_GetLumps(FILE *handle, UINT16 *nlmp, void **pool, boolean *nonmusic);

// W_VerifyNMUSlumps for a pack: 1 = only music/sound lumps, 0 = other lumps, -1 = not a pack / unreadable.
int WPack_VerifyNMUS(FILE *handle);

// Reads size bytes at offset of the (decoded) lump into dest; same contract as the CM_NOCOMPRESSION read of
// W_ReadLumpHeaderPwad: size and offset are already clamped to the lump. Handles raw and CM_LZ4 pack lumps.
// File reads use a 64-byte aligned bounce buffer on PS2 and sector-aligned, sector-multiple requests; dest need not
// be aligned. Valid packs are sector-padded. Returns bytes delivered (== size unless damaged). Not reentrant.
size_t WPack_ReadLump(FILE *handle, const lumpinfo_t *l, void *dest, size_t size, size_t offset);

// Frees the I/O and decode buffers (W_Shutdown).
void WPack_Shutdown(void);

#endif // __W_PACK__
