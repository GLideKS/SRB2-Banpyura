/* Execute the real src/z_zone.c PS2 allocator with only engine/platform services stubbed.
 * Include it in this translation unit so budget/list invariants can be checked without test APIs.
 * ZONE_HOST_NATIVE instead tests the unchanged host allocator and PS2_PROFILE lock no-op.
 * The PS2 path runs on a 32-bit host (matching EE pointer/size_t widths); the native profile
 * runs on x64. Neither emulates EE DMA/cache behavior. */
#include <assert.h>
#include <limits.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Skip unrelated engine headers, but use the actual zone header, tags and allocator source. */
#define __DOOMDEF__
#define __DOOMTYPE__
#define __DOOMSTAT__
#define __R_PATCH__
#define __R_PICFORMATS__
#define __I_SYSTEM__
#define __I_VIDEO__
#define __M_MISC__
#define LUA_SCRIPT_H
#define PS2_PROFILE
#ifndef ZONE_HOST_NATIVE
#define PS2
#endif
#ifdef _MSC_VER
/* Map the allocator's sole GNU type attribute to MSVC's equivalent. */
#define __attribute__(attrs) __declspec(align(16))
#endif
typedef int INT32;
typedef unsigned int UINT32;
typedef int boolean;
#define false 0
#define true 1
#define COM_LUA 0
#define M_GetText(s) (s)
#define M_Memcpy memcpy
#define ZZ_Alloc(s) Z_Malloc(s, PU_STATIC, NULL)
#define I_Assert(e) assert(e)

static jmp_buf error_jump;
static int expecting_error;
static char error_text[256];
static unsigned int oom_reports, checks;
static const size_t heap_capacity = 11u << 20; /* unchanged 3 MiB reserve leaves 8 MiB */

static void I_Error(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(error_text, sizeof error_text, fmt, ap);
	va_end(ap);
	if (expecting_error)
		longjmp(error_jump, 1);
	fprintf(stderr, "Unexpected I_Error: %s\n", error_text);
	exit(2);
}
static void check(int condition, const char *what)
{
	checks++;
	if (!condition)
	{
		fprintf(stderr, "FAIL: %s\n", what);
		exit(1);
	}
}
static void CONS_Printf(const char *fmt, ...) { (void)fmt; }
static void COM_AddCommand(const char *name, void (*fn)(void), int flags)
{ (void)name; (void)fn; (void)flags; }
static void LUA_InvalidateUserdata(void *p) { (void)p; }
static size_t I_GetFreeMem(size_t *total) { *total = heap_capacity; return heap_capacity; }
size_t PS2_HeapCapacity(void) { return heap_capacity; }
void PS2_ReportOOM(void) { oom_reports++; }
static const char *sizeu1(size_t n)
{
	static char s[32];
	snprintf(s, sizeof s, "%zu", n);
	return s;
}
static const char *sizeu2(size_t n) { return sizeu1(n); }

/* Track actual raw allocation lengths independently of allocator metadata, with red zones. */
static struct { void *p, *storage; size_t bytes; } allocations[4096];
static size_t live_bytes, live_blocks;
static int refuse_malloc;
static void *checked_malloc(size_t bytes)
{
	size_t i;
	unsigned char *storage;
	if (refuse_malloc)
		return NULL;
	for (i = 0; i < sizeof allocations / sizeof allocations[0]; i++)
		if (!allocations[i].p)
			break;
	check(i < sizeof allocations / sizeof allocations[0], "raw tracking capacity");
	check(bytes <= SIZE_MAX - 64, "raw size does not overflow");
	storage = malloc(bytes + 64);
	if (!storage)
		return NULL;
	memset(storage, 0xA5, 32);
	memset(storage + 32 + bytes, 0xA5, 32);
	allocations[i].p = storage + 32;
	allocations[i].storage = storage;
	allocations[i].bytes = bytes;
	live_bytes += bytes;
	live_blocks++;
	return allocations[i].p;
}
static void checked_free(void *p)
{
	size_t i, j;
	unsigned char *storage;
	if (!p)
		return;
	for (i = 0; i < sizeof allocations / sizeof allocations[0]; i++)
		if (allocations[i].p == p)
			break;
	check(i < sizeof allocations / sizeof allocations[0], "free uses an original raw pointer");
	storage = allocations[i].storage;
	for (j = 0; j < 32; j++)
	{
		check(storage[j] == 0xA5, "raw leading red zone");
		check(storage[32 + allocations[i].bytes + j] == 0xA5, "raw trailing red zone");
	}
	live_bytes -= allocations[i].bytes;
	live_blocks--;
	free(storage);
	memset(&allocations[i], 0, sizeof allocations[i]);
}

#define malloc checked_malloc
#define free checked_free
#ifdef ZONE_HOST_SOURCE
#include ZONE_HOST_SOURCE
#else
#include "../../src/z_zone.c"
#endif
#undef malloc
#undef free

static void consistent(void)
{
	memblock_t *b;
	size_t blocks = 0;
	Z_CheckHeap(123);
	for (b = head.next; b != &head; b = b->next)
		blocks++;
	check(blocks == live_blocks, "zone list matches live raw allocations");
#ifdef PS2
	check(zused == live_bytes + 16 * live_blocks, "budget equals actual raw bytes plus unchanged libc overhead");
#endif
}
static void empty_zone(void)
{
	Z_FreeTags(0, INT32_MAX);
	consistent();
	check(live_bytes == 0 && live_blocks == 0, "all raw allocations released");
#ifdef PS2
	check(zused == 0, "budget returns to zero");
#endif
}

#ifdef PS2
static void expect_alloc_error(size_t size, INT32 bits, const char *message)
{
	size_t before_bytes = live_bytes, before_blocks = live_blocks, before_used = zused;
	expecting_error = 1;
	if (!setjmp(error_jump))
	{
		(void)Z_MallocAlign(size, PU_STATIC, NULL, bits);
		check(0, "allocation should have refused");
	}
	expecting_error = 0;
	check(strstr(error_text, message) != NULL, "expected error reason");
	check(live_bytes == before_bytes && live_blocks == before_blocks && zused == before_used,
		"refusal leaves live allocations and accounting unchanged");
	consistent();
}
static void alignment_and_cycles(void)
{
	static const size_t sizes[] = {0, 1, 2047, 2048, 4097};
	size_t i, j;
	INT32 bits;
	for (bits = 0; bits <= 16; bits++)
		for (i = 0; i < sizeof sizes / sizeof sizes[0]; i++)
		{
			unsigned char *owner = NULL;
			unsigned char *p = Z_CallocAlign(sizes[i], PU_STATIC, &owner, bits);
			check(owner == p, "calloc sets owner");
			check((uintptr_t)p % ((uintptr_t)1 << bits) == 0, "requested alignment up to 65536");
			check((uintptr_t)p % (sizes[i] >= 2048 ? 64 : 16) == 0, "minimum payload alignment");
			check((uintptr_t)MEMBLOCK(p) % 16 == 0, "header address is 16-byte aligned");
			for (j = 0; j < sizes[i]; j++)
				check(p[j] == 0, "calloc zeros the entire payload");
			memset(p, 0x3C, sizes[i]);
			consistent();
			Z_Free(p);
			check(owner == NULL, "free clears owner");
			consistent();
		}
	for (i = 0; i < 20000; i++)
	{
		unsigned char *owner = NULL;
		size_t n = i % 8192;
		unsigned char *p = Z_MallocAlign(n, PU_STATIC, &owner, (INT32)(i % 17));
		memset(p, 0x5A, n);
		Z_Free(p);
		check(owner == NULL, "cycle owner cleared");
		consistent();
		check(zused == 0, "repeated alloc/free does not drift");
	}
	for (i = 0; i < 200; i++)
	{
		unsigned char *owner = NULL;
		unsigned char *p = Z_CallocAlign(257, PU_STATIC, &owner, 8);
		memset(p, 0x62, 257);
		p = Z_ReallocAlign(p, 4097, PU_STATIC, &owner, 16);
		check(p == owner && (uintptr_t)p % 65536 == 0, "realloc restores owner and requested alignment");
		for (j = 0; j < 4097; j++)
			check(p[j] == (j < 257 ? 0x62 : 0), "realloc preserves data and zeros extension");
		consistent();
		p = Z_Realloc(p, 0, PU_STATIC, &owner);
		check(p == NULL && owner == NULL, "zero-size realloc frees and clears owner");
		consistent();
		check(zused == 0, "realloc/free does not drift");
	}
	expect_alloc_error(1, -1, "invalid alignment");
	expect_alloc_error(1, 32, "invalid alignment");
	expect_alloc_error(1, INT32_MAX, "invalid alignment");
	expect_alloc_error(SIZE_MAX, 0, "too large");
	expect_alloc_error(SIZE_MAX - sizeof(memblock_t), 16, "too large");
	expect_alloc_error(1, 31, "Out of memory");
	check(zlimit == heap_capacity - Z_RESERVE, "unchanged reserve and budget");
	printf("PASS alignment 0..16, invalid shifts/size overflow, 20000 alloc/free cycles, 200 realloc cycles\n");
}
static void pressure_and_lock(void)
{
	void *old = NULL, *recent = NULL, *unlocked = NULL;
	void *fixed, *scratch, *request;
	Z_Malloc(2u << 20, PU_CACHE, &old);
	Z_Malloc(2u << 20, PU_CACHE, &recent);
	Z_Malloc(65536, PU_CACHE_UNLOCKED, &unlocked);
	fixed = Z_Malloc(3u << 20, PU_STATIC, NULL);
	scratch = Z_Malloc(131072, PU_CACHE, NULL);
	memset(fixed, 0x4B, 3u << 20);
	request = Z_Malloc(2u << 20, PU_STATIC, NULL);
	check(old == NULL && unlocked == NULL, "pressure purges oldest cache and unlocked tag");
	check(recent != NULL, "pressure preserves newer cache when enough was freed");
	check(((unsigned char *)fixed)[(3u << 20) - 1] == 0x4B, "pressure preserves static payload");
	check((MEMBLOCK(scratch))->id == ZONEID, "pressure preserves ownerless scratch");
	(void)request;
	consistent();
	empty_zone();
	check(recent == NULL, "bulk free clears surviving cache owner");

	Z_Malloc(2u << 20, PU_CACHE, &old);
	Z_Malloc(65536, PU_CACHE_UNLOCKED, &unlocked);
	fixed = Z_Malloc(5u << 20, PU_STATIC, NULL);
	Z_PurgeLock(true);
	Z_PurgeLock(true);
	request = Z_Malloc(2u << 20, PU_STATIC, NULL); /* inside the existing 2 MiB slack */
	check(zused > zlimit && zused < zlimit + Z_LOCK_SLACK, "locked allocation uses existing slack");
	check(old != NULL && unlocked != NULL, "slack does not evict either cache class");
	expect_alloc_error(2u << 20, 6, "Out of memory");
	check(old != NULL && unlocked != NULL, "over-slack refusal does not evict");
	refuse_malloc = 1;
	expect_alloc_error(16, 4, "Out of memory");
	refuse_malloc = 0;
	check(old != NULL && unlocked != NULL, "libc failure while locked does not evict");
	Z_PurgeLock(false);
	for (size_t i = 0; i < 2001; i++)
		Z_CheckMemCleanup();
	check(old != NULL && unlocked != NULL, "nested lock defers automatic cleanup");
	expect_alloc_error(2u << 20, 6, "Out of memory");
	Z_PurgeLock(false);
	Z_Malloc(1, PU_STATIC, NULL);
	check(old == NULL && unlocked == NULL, "first post-unlock allocation purges accumulated pressure");
	check((MEMBLOCK(fixed))->id == ZONEID && (MEMBLOCK(request))->id == ZONEID, "post-unlock purge retains static blocks");
	consistent();
	empty_zone();
	Z_Malloc(16, PU_CACHE_UNLOCKED, &unlocked);
	for (size_t i = 0; i < 2001; i++)
		Z_CheckMemCleanup();
	check(unlocked == NULL, "automatic cleanup resumes after unlock");
	empty_zone();
	expecting_error = 1;
	if (!setjmp(error_jump))
	{
		Z_PurgeLock(false);
		check(0, "unbalanced unlock should fail");
	}
	expecting_error = 0;
	check(zpurgelock == 0, "unbalanced unlock does not corrupt lock state");
	printf("PASS pressure eviction, owner clearing, nested lock/slack, budget and libc refusal, deferred cleanup\n");
}
#endif

int main(void)
{
	Z_Init();
#ifdef PS2
	printf("PS2 allocator host: pointer=%zu header=%zu reserve=%zu slack=%zu\n",
		sizeof(void *), sizeof(memblock_t), (size_t)Z_RESERVE, (size_t)Z_LOCK_SLACK);
	alignment_and_cycles();
	pressure_and_lock();
#else
	void *owner = NULL;
	/* An unmatched unlock and requests above the fake PS2 budget remain harmless on the host. */
	Z_PurgeLock(false);
	Z_PurgeLock(true);
	void *p = Z_Malloc(9u << 20, PU_STATIC, &owner);
	check(p == owner, "host profile still uses the original unbudgeted allocator");
	Z_PurgeLock(false);
	consistent();
	Z_Free(p);
	check(owner == NULL, "host owner clearing unchanged");
	printf("PASS host PS2_PROFILE lock no-op and unchanged malloc semantics\n");
#endif
	empty_zone();
	printf("PASS all %u checks, %u expected OOM reports, zero live allocations\n", checks, oom_reports);
	return 0;
}
