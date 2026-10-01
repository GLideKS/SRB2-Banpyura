/* Actual R_GenerateTexture/Patch_* source, extracted verbatim by the runner.
 * Compile once with PS2_PROFILE and once with the unchanged host branch. These
 * fixtures exercise Doom-patch copy/flip/clip composition; PNG/flat conversion
 * and blend drawers are deliberately fatal if accidentally reached. */
#ifndef TEXTURE_REFERENCE
#define PS2_PROFILE
#endif
#define NO_PNG_LUMPS
#include "doomdef.h"
#include "doomstat.h"
#include "r_textures.h"
#include "r_patch.h"
#include "r_data.h"
#include "r_draw.h"
#include "w_wad.h"
#include "z_zone.h"
#include <io.h>
#include <fcntl.h>
#include "blend_hosttest_alloc.h"

size_t texturememory;
static UINT8 *input;
static size_t inputsize;
static patch_t *cachedpatch;
static UINT8 *cache_slot[1];
static column_t *column_slot[1];
static texture_t *texture_slot[1];
static INT32 width_slot[1];
static FILE *snapshot_output;

#include "texture_renderer.inc"
#include "texture_patch.inc"

void *M_Memcpy(void *dest, const void *src, size_t n) { return memcpy(dest, src, n); }
void *W_CacheLumpNumPwad(UINT16 wad, UINT16 lump, INT32 tag)
{
	(void)wad; (void)lump;
	void *p = Z_Malloc(inputsize, tag, NULL);
	memcpy(p, input, inputsize);
	return p;
}
void *W_GetCachedPatchNumPwad(UINT16 wad, UINT16 lump)
{
	(void)wad; (void)lump;
	return cachedpatch;
}
void *W_CachePatchNumPwad(UINT16 wad, UINT16 lump, INT32 tag)
{
	(void)wad; (void)lump;
	host_check(tag == PU_PATCH && !cachedpatch, "persistent Doom patch conversion");
	cachedpatch = Patch_CreateFromDoomPatch((softwarepatch_t *)input);
	return cachedpatch;
}
UINT8 ASTBlendPaletteIndexes(UINT8 bg, UINT8 fg, int style, UINT8 alpha)
{
	(void)bg; (void)fg; (void)style; (void)alpha;
	host_check(0, "copy-style texture fixtures must not reach blend drawers");
	return 0;
}
void *Picture_Convert(pictureformat_t informat, void *picture, pictureformat_t outformat,
	size_t insize, size_t *outsize, INT32 width, INT32 height, INT32 left, INT32 top, pictureflags_t flags)
{
	(void)informat; (void)picture; (void)outformat; (void)insize; (void)outsize;
	(void)width; (void)height; (void)left; (void)top; (void)flags;
	host_check(0, "Doom patch fixtures must not reach flat/PNG converters");
	return NULL;
}

static UINT32 hash_byte(UINT32 h, UINT8 byte)
{
	host_check(fputc(byte, snapshot_output) != EOF, "normalized snapshot write");
	return (h ^ byte) * 16777619u;
}
static UINT32 hash_int(UINT32 h, size_t value)
{
	for (int i = 0; i < 4; i++) h = hash_byte(h, (UINT8)(value >> (8*i)));
	return h;
}

static UINT32 snapshot(size_t pixels, size_t *postcount)
{
	texture_t *t = texture_slot[0];
	UINT8 *block = cache_slot[0];
	size_t bytes = host_allocs[host_allocation(block)].size;
	column_t *columns = column_slot[0];
	UINT32 h = 2166136261u;
	*postcount = 0;
	host_check(host_allocs[host_allocation(block)].tag == PU_CACHE, "finished texture cache tag");
	host_check((UINT8 *)columns >= block + pixels && (UINT8 *)(columns + t->width) <= block + bytes,
		"column array bounds and correct pixel byte count");
#ifndef TEXTURE_REFERENCE
	host_check((uintptr_t)columns % _Alignof(column_t) == 0, "column_t alignment");
	host_check((size_t)((UINT8 *)columns - block - pixels) < _Alignof(column_t), "only necessary pixel-to-column padding");
#endif
	h = hash_int(h, t->width); h = hash_int(h, t->height); h = hash_int(h, t->transparency); h = hash_int(h, t->flip);
	h = hash_int(h, pixels);
	for (size_t i = 0; i < pixels; i++) h = hash_byte(h, block[i]);
	for (INT32 x = 0; x < t->width; x++)
	{
		column_t *c = &columns[x];
		h = hash_int(h, c->num_posts);
		if (!c->num_posts)
			continue;
#ifndef TEXTURE_REFERENCE
		host_check((uintptr_t)c->posts % _Alignof(post_t) == 0, "post_t alignment");
#endif
		host_check((UINT8 *)c->posts >= (UINT8 *)(columns + t->width)
			&& (UINT8 *)(c->posts + c->num_posts) <= block + bytes, "post array bounds");
		for (unsigned i = 0; i < c->num_posts; i++)
		{
			post_t *p = &c->posts[i];
			host_check(c->pixels >= block && c->pixels + p->data_offset + p->length <= block + pixels,
				"post data within actual pixel bytes, not padding");
			h = hash_int(h, p->topdelta); h = hash_int(h, p->length); h = hash_int(h, p->data_offset);
			for (unsigned y = 0; y < p->length; y++) h = hash_byte(h, c->pixels[p->data_offset + y]);
		}
		*postcount += c->num_posts;
	}
	return h;
}

static void run_texture(const char *name, INT16 width, INT16 height, INT16 patches, int mode, int packed)
{
	texture_t *t = calloc(1, sizeof *t + (size_t)patches * sizeof(texpatch_t));
	size_t rawpixels = 0, rawposts = 0, pixels, posts;
	host_check(t != NULL, "texture fixture descriptor allocation");
	t->width = width; t->height = height; t->type = TEXTURETYPE_SINGLEPATCH; t->patchcount = patches;
	for (INT16 i = 0; i < patches; i++)
	{
		t->patches[i].style = AST_COPY;
		t->patches[i].flip = (mode == 1) ? 1 : (i ? 3 : 0);
		if (mode == 2) { t->patches[i].originx = i; t->patches[i].originy = i; }
		if (mode == 3) { t->patches[i].originx = i-1; t->patches[i].originy = i-1; }
		if (mode == 4) t->patches[i].originx = width+1;
	}
	Patch_CalcDataSizes((softwarepatch_t *)input, &rawpixels, &rawposts);
	pixels = packed && patches == 1 ? rawpixels : (size_t)width * height;
	texture_slot[0] = t; width_slot[0] = width;
	size_t before = host_live_blocks;
	patch_t *oldpatch = cachedpatch;
	R_GenerateTexture(0);
#ifndef TEXTURE_REFERENCE
	host_check(host_live_blocks == before + 1 + (!oldpatch && cachedpatch ? 4 : 0),
		"all raw/scratch buffers freed; only texture and persistent patch remain");
#else
	(void)before; (void)oldpatch;
#endif
	UINT32 hash = snapshot(pixels, &posts);
	printf("%s: pixels=%zu posts=%zu hash=%08x\n", name, pixels, posts, hash);
	host_purge_cache();
	host_check(cache_slot[0] == NULL, "texture cache owner cleared by eviction");
#ifdef TEXTURE_REFERENCE
	R_CheckTextureCache(0); // original callers explicitly ensure the cache exists
#endif
	host_check(R_GetColumn(0, width + 1) == &column_slot[0][width == 1 ? 0 : 1], "column getter wraps and rebuilds cache");
	host_check(snapshot(pixels, &posts) == hash, "eviction/reconstruction preserves normalized pixels and posts");
	Z_Free(cache_slot[0]);
	column_slot[0] = NULL;
	free(t);
	texture_slot[0] = NULL;
}

int main(int argc, char **argv)
{
	UINT32 size, packed;
	host_check(argc == 2, "normalized snapshot output path");
	snapshot_output = fopen(argv[1], "wb");
	host_check(snapshot_output != NULL, "normalized snapshot output open");
	_setmode(_fileno(stdin), _O_BINARY);
	host_check(fread(&size, sizeof size, 1, stdin) == 1 && fread(&packed, sizeof packed, 1, stdin) == 1,
		"Doom patch input header");
	inputsize = size; input = malloc(size);
	host_check(input != NULL && fread(input, 1, size, stdin) == size, "Doom patch fixture bytes");
	softwarepatch_t *raw = (softwarepatch_t *)input;
	textures = texture_slot; texturecache = cache_slot; texturecolumns = column_slot; texturewidth = width_slot; numtextures = 1;
	run_texture("single", raw->width, raw->height, 1, 0, packed);
	run_texture("single-flip", raw->width, raw->height, 1, 1, packed);
	run_texture("composite", raw->width+1, raw->height+1, 2, 2, 0);
	run_texture("clipped", raw->width > 1 ? raw->width-1 : 1, raw->height-1, 2, 3, 0);
	run_texture("offscreen", raw->width+1, raw->height+1, 2, 4, 0);
	host_free_all();
	free(input);
	host_check(fclose(snapshot_output) == 0, "normalized snapshot output close");
	fprintf(stderr, "Texture bounds, reconstruction and red zones PASS (%zu-bit host)\n", 8 * sizeof(void *));
	return 0;
}
