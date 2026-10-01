/* Host test of the engine's pack reader (src/w_pack.c, built unchanged with MSVC).
 * usage: pack_hosttest PACK OUT.tsv
 * For every lump: decodes it through WPack_GetLumps/WPack_ReadLump exactly like W_ReadLumpHeaderPwad does, prints
 * index, name, hash, longname, fullname, size, compression, crc32 of the decoded lump; then compares 20 partial reads
 * per lump (head, tail, block-crossing, random ranges) with the full read. Exit code != 0 if a partial read differs.
 * tools/ps2/test_pack_reader.py compares the table with the pk3 (crc32/size/names/order). */
#include "doomdef.h"
#include "w_wad.h"
#include "z_zone.h"
#include "w_pack.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* engine services the reader links to */
void *Z_MallocAlign(size_t size, INT32 tag, void *user, INT32 alignbits)
{
	(void)tag; (void)user; (void)alignbits;
	void *p = malloc(size);
	if (!p)
		I_Error("Host test: allocation failed");
	return p;
}
void Z_Free(void *p) { free(p); }
void CONS_Alert(alerttype_t level, const char *fmt, ...)
{
	va_list ap;
	(void)level;
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
}
void I_Error(const char *error, ...)
{
	va_list ap;
	va_start(ap, error);
	vfprintf(stderr, error, ap);
	va_end(ap);
	exit(2);
}

static UINT32 crctab[256];
static UINT32 crc32(const UINT8 *p, size_t n)
{
	UINT32 c = 0xFFFFFFFFu;
	while (n--)
		c = crctab[(c ^ *p++) & 0xFF] ^ (c >> 8);
	return ~c;
}

static UINT32 rngstate = 12345;
static UINT32 rnd(void)
{
	rngstate = rngstate * 1664525u + 1013904223u;
	return rngstate >> 8;
}

/* same contract as W_ReadLumpHeaderPwad after its clamping */
static size_t read_lump(FILE *f, const lumpinfo_t *l, void *dest, size_t size, size_t offset)
{
	if (!l->size || l->size < offset)
		return 0;
	if (!size || size > l->size - offset)
		size = l->size - offset;
	return WPack_ReadLump(f, l, dest, size, offset);
}

static boolean guarded(const UINT8 *buf, size_t off, size_t len, size_t capacity)
{
	size_t j;
	for (j = 0; j < off; j++)
		if (buf[j] != 0xA5)
			return false;
	for (j = off + len; j < capacity; j++)
		if (buf[j] != 0xA5)
			return false;
	return true;
}

int main(int argc, char **argv)
{
	FILE *f, *out;
	void *pool, *iobuf;
	lumpinfo_t *li;
	UINT16 n, i;
	boolean nonmusic;
	unsigned long partial = 0, bad = 0, lz4lumps = 0;
	UINT32 k, j;
	boolean rejectheader, rejectread, rejectindex;

	for (k = 0; k < 256; k++)
	{
		UINT32 c = k;
		for (j = 0; j < 8; j++)
			c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
		crctab[k] = c;
	}

	if (argc != 3)
		return 3;
	rejectheader = strcmp(argv[1], "--reject-header") == 0;
	rejectread = strcmp(argv[1], "--reject-read") == 0;
	rejectindex = strcmp(argv[1], "--reject-index") == 0;
	rejectread = rejectread || rejectindex;
	f = fopen(argv[rejectheader || rejectread ? 2 : 1], "rb");
	if (!f)
		return 3;
	iobuf = WPack_SetupHandle(f); // setvbuf must precede detection or any other stream I/O
	if (!WPack_Detect(f))
		return 4;
	li = WPack_GetLumps(f, &n, &pool, &nonmusic);
	if (rejectheader || rejectread)
	{
		boolean rejected = rejectheader && li == NULL && n == 0 && pool == NULL;
		if (rejectread && li && n == 1)
		{
			UINT8 *buf = malloc(li[0].size + 128);
			size_t got;
			if (!buf)
				return 3;
			memset(buf, 0xA5, li[0].size + 128);
			got = read_lump(f, li, buf + 1, 0, 0);
			rejected = got != li[0].size && guarded(buf, 1, li[0].size, li[0].size + 128);
			if (rejectindex)
			{
				memset(buf, 0xA5, li[0].size + 128);
				rejected = rejected && got == 0 && read_lump(f, li, buf + 1, 8, 0) == 0
					&& read_lump(f, li, buf + 3, 8, li[0].size - 8) == 0
					&& guarded(buf, 0, 0, li[0].size + 128);
			}
			free(buf);
		}
		if (li)
		{
			Z_Free(li);
			Z_Free(pool);
		}
		WPack_Shutdown();
		fclose(f);
		free(iobuf);
		fprintf(stderr, "Expected %s rejection: %s\n", rejectheader ? "metadata" : "read", rejected ? "PASS" : "FAIL");
		return rejected ? 0 : 1;
	}
	if (!li)
		return 5;
	out = fopen(argv[2], "wb");
	if (!out)
		return 3;
	fprintf(out, "nonmusic\t%d\tverify\t%d\n", nonmusic, WPack_VerifyNMUS(f));
	for (i = 0; i < n; i++)
	{
		size_t size = li[i].size, got;
		UINT8 *storage = malloc(size + 128), *part = malloc(size + 128), *buf;
		if (!storage || !part)
			return 3;
		buf = storage + 1; // deliberately unaligned, with guards before and after the destination
		memset(storage, 0xA5, size + 128);

		got = size ? read_lump(f, &li[i], buf, 0, 0) : 0;
		if (got != size || !guarded(storage, 1, size, size + 128))
		{
			fprintf(stderr, "lump %u: read %lu of %lu\n", (unsigned)i, (unsigned long)got, (unsigned long)size);
			bad++;
		}
		if (li[i].compression == CM_LZ4)
			lz4lumps++;
		fprintf(out, "%u\t%s\t%08x\t%s\t%s\t%lu\t%d\t%08x\n", (unsigned)i, li[i].name, (unsigned)li[i].hash, li[i].longname,
			li[i].fullname, (unsigned long)size, (int)li[i].compression, (unsigned)crc32(buf, size));

		for (k = 0; size && k < 20; k++)
		{
			size_t off, len, destoff = 1 + (k * 7) % 63;
			switch (k)
			{
			case 0: off = 0; len = size < 8 ? size : 8; break;                          /* patch header */
			case 1: len = size < 16 ? size : 16; off = size - len; break;                /* tail */
			case 2: off = size > 1 ? size / 2 : 0; len = 0; break;                       /* size 0 = rest of the lump */
			case 3: off = size >= WPACK_BLOCK + 5 ? WPACK_BLOCK - 3 : 0; len = 10; break; /* across a block boundary */
			case 4: off = size >= WPACK_BLOCK ? WPACK_BLOCK : 0; len = WPACK_BLOCK; break; /* exactly block 1 */
			case 5: off = size >= 2 * WPACK_BLOCK ? 5 : 0; len = 2 * WPACK_BLOCK; break;
			default: off = rnd() % size; len = 1 + rnd() % (k & 1 ? 100 : 200000); break;
			}
			if (off > size)
				off = 0;
			memset(part, 0xA5, size + 128);
			got = read_lump(f, &li[i], part + destoff, len, off);
			if (!len || len > size - off)
				len = size - off;
			partial++;
			if (got != len || memcmp(part + destoff, buf + off, len) || !guarded(part, destoff, len, size + 128))
			{
				bad++;
				if (bad < 20)
					fprintf(stderr, "lump %u (%s): partial read off=%lu len=%lu differs\n", (unsigned)i, li[i].fullname,
						(unsigned long)off, (unsigned long)len);
			}
		}
		free(storage);
		free(part);
	}
	fclose(out);
	fprintf(stderr, "%u lumps (%lu LZ4), %lu partial reads, %lu failures\n", (unsigned)n, lz4lumps, partial, bad);
	WPack_Shutdown();
	Z_Free(li);
	Z_Free(pool);
	fclose(f);
	free(iobuf);
	return bad ? 1 : 0;
}
