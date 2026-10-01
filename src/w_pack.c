// SONIC ROBO BLAST 2 (PS2 port)
//-----------------------------------------------------------------------------
// This program is free software distributed under the
// terms of the GNU General Public License, version 2.
// See the 'LICENSE' file for more details.
//-----------------------------------------------------------------------------
/// \file  w_pack.c
/// \brief SRP2 cooked packs: lumpinfo_t[] straight from the pack, LZ4 lump reader (format: docs/PACK_FORMAT.md)

#include "doomdef.h"
#include "w_wad.h"
#include "z_zone.h"
#include "w_pack.h"

#include <lz4.h>
#include <limits.h>

#ifdef PS2
#include <malloc.h>
#define WPACK_ALIGNED_ALLOC(size) memalign(64, (size))
#else
#define WPACK_ALIGNED_ALLOC(size) malloc(size)
#endif

#define SRP2_VERSION 1
#define SRP2_HEADER_SIZE 64
#define SRP2_FLAG_NONMUSIC 1
#define SRP2_CODEC_RAW 0
#define SRP2_CODEC_LZ4 1
#define SRP2_RAWBLOCK 0x80000000u // block index: this block is stored raw

typedef struct
{
	char magic[4];
	UINT32 version;
	UINT32 headersize;
	UINT32 flags;
	UINT32 numlumps;
	UINT32 tableoffset;
	UINT32 pooloffset;
	UINT32 poolsize;
	UINT32 dataoffset;
	UINT32 filesize;
	UINT32 blocksize;
	UINT32 reserved[5];
} srp2_header_t;

typedef struct
{
	UINT32 position;
	UINT32 disksize;
	UINT32 size;
	UINT32 fullname;
	UINT32 longname;
	UINT32 codec;
} srp2_entry_t;

#define ENTRY_CHUNK 256 // 256 * 24 = 6144 bytes = 3 sectors per read
#define WPACK_SECTOR 2048

// All file reads bounce through aligned, whole-sector requests. Caller buffers may be unaligned.
static UINT8 *iobuf, *cbuf, *sbuf;

static boolean ReadBytes(FILE *handle, void *dest, size_t size)
{
	UINT8 *out = dest;
	long pos = ftell(handle);
	size_t done = 0;

	if (pos < 0 || size > (size_t)(LONG_MAX - pos))
		return false;
	if (!iobuf)
		iobuf = WPACK_ALIGNED_ALLOC(WPACK_BLOCK);
	if (!iobuf)
		return false;
	while (done < size)
	{
		size_t skip = (size_t)pos % WPACK_SECTOR;
		size_t want = min(size - done, WPACK_BLOCK - skip);
		size_t request = (skip + want + WPACK_SECTOR - 1) & ~(size_t)(WPACK_SECTOR - 1);
		size_t got;

		if (fseek(handle, pos - (long)skip, SEEK_SET) != 0)
			return false;
		got = fread(iobuf, 1, request, handle);
		if (got < skip + want) // never copy bytes not actually read (signature probes may see short files)
			return false;
		memcpy(out + done, iobuf + skip, want);
		done += want;
		pos += (long)want;
	}
	return fseek(handle, pos, SEEK_SET) == 0;
}

boolean WPack_Detect(FILE *handle)
{
	char magic[4];

	if (fseek(handle, 0, SEEK_SET) != 0 || !ReadBytes(handle, magic, sizeof magic))
		return false;
	return memcmp(magic, "SRP2", 4) == 0;
}

void *WPack_SetupHandle(FILE *handle)
{
	void *buf = WPACK_ALIGNED_ALLOC(65536);

	if (buf && setvbuf(handle, buf, _IOFBF, 65536) != 0)
	{
		free(buf);
		buf = NULL;
	}
	return buf;
}

static boolean ReadHeader(FILE *handle, srp2_header_t *h)
{
	long end;

	if (fseek(handle, 0, SEEK_SET) != 0 || !ReadBytes(handle, h, sizeof *h))
		return false;
	if (memcmp(h->magic, "SRP2", 4) != 0)
		return false;
	if (fseek(handle, 0, SEEK_END) != 0)
		return false;
	end = ftell(handle);
	h->version = LONG(h->version);
	h->headersize = LONG(h->headersize);
	h->flags = LONG(h->flags);
	h->numlumps = LONG(h->numlumps);
	h->tableoffset = LONG(h->tableoffset);
	h->pooloffset = LONG(h->pooloffset);
	h->poolsize = LONG(h->poolsize);
	h->dataoffset = LONG(h->dataoffset);
	h->filesize = LONG(h->filesize);
	h->blocksize = LONG(h->blocksize);
	if (h->version != SRP2_VERSION || h->headersize != SRP2_HEADER_SIZE || h->blocksize != WPACK_BLOCK)
	{
		CONS_Alert(CONS_ERROR, "Unsupported pack version\n");
		return false;
	}
	if (end < 0 || h->filesize != (UINT32)end)
	{
		CONS_Alert(CONS_ERROR, "Pack is truncated or damaged (size %ld, expected %u)\n", end, (unsigned)h->filesize);
		return false;
	}
	if (h->numlumps == 0 || h->numlumps > UINT16_MAX || h->poolsize == 0
		|| (h->flags & ~SRP2_FLAG_NONMUSIC) != 0
		|| h->filesize % WPACK_SECTOR != 0
		|| h->tableoffset < SRP2_HEADER_SIZE || h->tableoffset % WPACK_SECTOR != 0
		|| h->tableoffset > h->filesize
		|| h->numlumps * sizeof(srp2_entry_t) > h->filesize - h->tableoffset
		|| h->pooloffset < h->tableoffset + h->numlumps * sizeof(srp2_entry_t)
		|| h->pooloffset % WPACK_SECTOR != 0 || h->pooloffset > h->filesize
		|| h->poolsize > h->filesize - h->pooloffset
		|| h->dataoffset < h->pooloffset + h->poolsize
		|| h->dataoffset % WPACK_SECTOR != 0 || h->dataoffset > h->filesize)
	{
		CONS_Alert(CONS_ERROR, "Pack header layout is corrupt\n");
		return false;
	}
	return true;
}

int WPack_VerifyNMUS(FILE *handle)
{
	srp2_header_t h;

	if (!ReadHeader(handle, &h))
		return -1;
	return (h.flags & SRP2_FLAG_NONMUSIC) ? 0 : 1;
}

lumpinfo_t *WPack_GetLumps(FILE *handle, UINT16 *nlmp, void **poolp, boolean *nonmusic)
{
	srp2_header_t h;
	srp2_entry_t chunk[ENTRY_CHUNK];
	lumpinfo_t *lumpinfo, *lump_p;
	char *pool;
	UINT32 i, n, done, prevend;

	*nlmp = 0;
	*poolp = NULL;
	*nonmusic = false;

	if (!ReadHeader(handle, &h))
		return NULL;

	n = h.numlumps;
	prevend = h.dataoffset;

	// one block for every name of the pack (instead of two mallocs per lump)
	pool = Z_Malloc(h.poolsize, PU_STATIC, NULL);
	if (fseek(handle, h.pooloffset, SEEK_SET) != 0 || !ReadBytes(handle, pool, h.poolsize) || pool[h.poolsize - 1] != '\0')
	{
		CONS_Alert(CONS_ERROR, "Pack string pool is corrupt\n");
		Z_Free(pool);
		return NULL;
	}

	lump_p = lumpinfo = Z_Malloc(n * sizeof (*lumpinfo), PU_STATIC, NULL);
	if (fseek(handle, h.tableoffset, SEEK_SET) != 0)
	{
		Z_Free(lumpinfo);
		Z_Free(pool);
		return NULL;
	}

	for (done = 0; done < n; done += ENTRY_CHUNK)
	{
		UINT32 count = min(n - done, ENTRY_CHUNK);

		if (!ReadBytes(handle, chunk, sizeof (srp2_entry_t) * count))
		{
			CONS_Alert(CONS_ERROR, "Failed to read pack table\n");
			Z_Free(lumpinfo);
			Z_Free(pool);
			return NULL;
		}

		for (i = 0; i < count; i++, lump_p++)
		{
			const srp2_entry_t *e = &chunk[i];
			UINT32 position = LONG(e->position), disksize = LONG(e->disksize), size = LONG(e->size);
			UINT32 fullname = LONG(e->fullname), longname = LONG(e->longname), codec = LONG(e->codec);
			const char *full;
			const char *trimname, *dotpos;

			if (fullname >= h.poolsize || longname >= h.poolsize
				|| codec > SRP2_CODEC_LZ4
				|| (size == 0 && (disksize != 0 || codec != SRP2_CODEC_RAW))
				|| (size != 0 && (disksize == 0 || position < prevend || position > h.filesize
					|| disksize > h.filesize - position
					|| position % (size >= WPACK_BLOCK ? WPACK_SECTOR : 64) != 0))
				|| (codec == SRP2_CODEC_RAW && disksize != size)
				|| (codec == SRP2_CODEC_LZ4 && size <= WPACK_BLOCK && disksize > WPACK_BLOCK)
				|| (codec == SRP2_CODEC_LZ4 && size > WPACK_BLOCK
					&& disksize < (1 + (size - 1) / WPACK_BLOCK) * sizeof(UINT32)))
			{
				CONS_Alert(CONS_ERROR, "Pack table entry %u is corrupt\n", (unsigned)(done + i));
				Z_Free(lumpinfo);
				Z_Free(pool);
				return NULL;
			}
			if (size)
				prevend = position + disksize;

			lump_p->position = position; // final position of the lump data
			lump_p->disksize = disksize;
			lump_p->diskpath = NULL;
			lump_p->size = size;

			full = pool + fullname;
			lump_p->fullname = (char *)full;
			lump_p->longname = pool + longname;

			// the 8 character name and its hash: same code as ResGetLumpsZip
			if ((trimname = strrchr(full, '/')) != 0)
				trimname++;
			else
				trimname = full;

			if ((dotpos = strrchr(trimname, '.')) == 0)
				dotpos = full + strlen(full);
			if (strlen(lump_p->longname) != (size_t)(dotpos - trimname)
				|| memcmp(lump_p->longname, trimname, (size_t)(dotpos - trimname)) != 0)
			{
				CONS_Alert(CONS_ERROR, "Pack entry %u has a corrupt longname\n", (unsigned)(done + i));
				Z_Free(lumpinfo);
				Z_Free(pool);
				return NULL;
			}

			memset(lump_p->name, '\0', 9);
			strncpy(lump_p->name, trimname, min(8, dotpos - trimname));
			lump_p->hash = quickncasehash(lump_p->name, 8);

			switch (codec)
			{
			case SRP2_CODEC_RAW:
				lump_p->compression = CM_NOCOMPRESSION;
				break;
			case SRP2_CODEC_LZ4:
				lump_p->compression = CM_LZ4;
				break;
			default:
				CONS_Alert(CONS_WARNING, "%s: Unsupported compression method\n", full);
				lump_p->compression = CM_UNSUPPORTED;
				break;
			}
		}
	}

	*nlmp = (UINT16)n;
	*poolp = pool;
	*nonmusic = (h.flags & SRP2_FLAG_NONMUSIC) != 0;
	return lumpinfo;
}

static boolean AllocBuffers(void)
{
	if (!cbuf)
		cbuf = WPACK_ALIGNED_ALLOC(WPACK_BLOCK);
	if (!sbuf)
		sbuf = WPACK_ALIGNED_ALLOC(WPACK_BLOCK);
	return cbuf && sbuf;
}

void WPack_Shutdown(void)
{
	free(iobuf);
	free(cbuf);
	free(sbuf);
	iobuf = cbuf = sbuf = NULL;
}

// Delivers decoded bytes [lo, hi) of one block (decoded size bsize, stored in csize bytes at the current file position)
// to dst (which receives byte lo). Returns false on a read/decode error.
static boolean ReadBlock(FILE *handle, boolean raw, UINT32 csize, UINT32 bsize, UINT32 lo, UINT32 hi, UINT8 *dst)
{
	UINT32 want;

	if (csize == 0 || csize > WPACK_BLOCK || bsize > WPACK_BLOCK || lo > hi || hi > bsize)
		return false;
	want = hi - lo;

	if (raw)
	{
		if (csize != bsize)
			return false;
		if (!ReadBytes(handle, cbuf, csize))
			return false;
		memcpy(dst, cbuf + lo, want);
		return true;
	}

	if (!ReadBytes(handle, cbuf, csize))
		return false;
	if (lo == 0 && want == bsize) // whole block straight into the destination
		return LZ4_decompress_safe((const char *)cbuf, (char *)dst, (int)csize, (int)bsize) == (int)bsize;
	if (lo == 0) // head of the block (patch headers etc): stop as soon as enough is decoded
		return LZ4_decompress_safe_partial((const char *)cbuf, (char *)dst, (int)csize, (int)want, (int)want) == (int)want;
	if (LZ4_decompress_safe((const char *)cbuf, (char *)sbuf, (int)csize, (int)bsize) != (int)bsize)
		return false;
	memcpy(dst, sbuf + lo, want);
	return true;
}

size_t WPack_ReadLump(FILE *handle, const lumpinfo_t *l, void *dest, size_t size, size_t offset)
{
	UINT8 *out = dest;
	UINT32 usize = (UINT32)l->size;
	UINT32 index[64];
	UINT32 *idx = index;
	UINT32 nb, first, last, b, remaining;
	long pos;
	size_t done = 0;

	if (l->size > UINT32_MAX || l->position > LONG_MAX || l->disksize > (unsigned long)LONG_MAX - l->position
		|| !dest || !size || offset >= usize || size > usize - offset)
		return 0;
	if (l->compression == CM_NOCOMPRESSION)
	{
		if (l->disksize != usize || fseek(handle, (long)(l->position + offset), SEEK_SET) != 0)
			return 0;
		return ReadBytes(handle, dest, size) ? size : 0;
	}
	if (l->compression != CM_LZ4 || !AllocBuffers())
		return 0;

	if (usize <= WPACK_BLOCK) // one LZ4 block, no index
	{
		if (fseek(handle, (long)l->position, SEEK_SET) != 0)
			return 0;
		return ReadBlock(handle, false, (UINT32)l->disksize, usize, (UINT32)offset, (UINT32)(offset + size), out) ? size : 0;
	}

	// u32 index[nb] (bit 31: stored raw) followed by the blocks
	nb = 1 + (usize - 1) / WPACK_BLOCK;
	if (nb * sizeof *idx > l->disksize)
		return 0;
	first = (UINT32)(offset / WPACK_BLOCK);
	last = (UINT32)((offset + size - 1) / WPACK_BLOCK);
	if (nb > sizeof index / sizeof index[0])
	{
		idx = malloc(nb * sizeof *idx);
		if (!idx)
			return 0;
	}

	if (fseek(handle, (long)l->position, SEEK_SET) != 0 || !ReadBytes(handle, idx, nb * sizeof *idx))
		goto end;

	pos = (long)(l->position + nb * sizeof *idx);
	remaining = (UINT32)(l->disksize - nb * sizeof *idx);
	for (b = 0; b < nb; b++) // validate the entire index before delivering even a partial read
	{
		UINT32 entry = LONG(idx[b]), csize = entry & ~SRP2_RAWBLOCK;
		UINT32 bsize = min(WPACK_BLOCK, usize - b * WPACK_BLOCK);

		if (!csize || csize > WPACK_BLOCK || csize > remaining
			|| ((entry & SRP2_RAWBLOCK) && csize != bsize))
			goto end;
		remaining -= csize;
		if (b < first)
			pos += (long)csize;
	}
	if (remaining != 0)
		goto end;

	if (fseek(handle, pos, SEEK_SET) != 0) // blocks first..last are contiguous: no seeks in between
		goto end;

	for (b = first; b <= last; b++)
	{
		UINT32 entry = LONG(idx[b]);
		UINT32 bstart = b * WPACK_BLOCK;
		UINT32 bsize = min(WPACK_BLOCK, usize - bstart);
		UINT32 lo = (UINT32)max(offset, bstart) - bstart;
		UINT32 hi = (UINT32)min(offset + size, (size_t)bstart + bsize) - bstart;

		if (!ReadBlock(handle, (entry & SRP2_RAWBLOCK) != 0, entry & ~SRP2_RAWBLOCK, bsize, lo, hi, out + done))
			break;
		done += hi - lo;
	}

end:
	if (idx != index)
		free(idx);
	return done;
}
