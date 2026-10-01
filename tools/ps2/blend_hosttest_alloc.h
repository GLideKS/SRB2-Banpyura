/* Engine-service doubles shared by the actual-code renderer host tests.
 * This is not a replacement zone test: track payload bounds, tags and owners so
 * renderer lifetimes and simulated PU_CACHE eviction can be checked. */
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static struct
{
	void *p, *raw, **owner;
	size_t size;
	INT32 tag;
} host_allocs[4096];
static size_t host_live_blocks, host_live_bytes, host_peak_bytes;
static UINT8 host_poison = 0xA5;

static void host_check(int condition, const char *message)
{
	if (!condition)
	{
		fprintf(stderr, "FAIL: %s\n", message);
		exit(2);
	}
}

static size_t host_allocation(void *p)
{
	for (size_t i = 0; i < sizeof host_allocs / sizeof host_allocs[0]; i++)
		if (host_allocs[i].p == p && p)
			return i;
	host_check(0, "allocation pointer is tracked");
	return 0;
}

void *Z_MallocAlign(size_t size, INT32 tag, void *owner, INT32 bits)
{
	size_t i, align = (size_t)1 << bits;
	UINT8 *raw, *p;
	if (align < 16)
		align = 16;
	for (i = 0; i < sizeof host_allocs / sizeof host_allocs[0]; i++)
		if (!host_allocs[i].p)
			break;
	host_check(i < sizeof host_allocs / sizeof host_allocs[0], "allocation tracking capacity");
	raw = malloc(size + align + 32);
	host_check(raw != NULL, "host allocation succeeded");
	p = (UINT8 *)(((uintptr_t)raw + 16 + align - 1) & ~(uintptr_t)(align - 1));
	memset(p - 16, 0xCD, 16);
	memset(p, host_poison, size);
	memset(p + size, 0xCD, 16);
	host_allocs[i].p = p;
	host_allocs[i].raw = raw;
	host_allocs[i].owner = owner;
	host_allocs[i].size = size;
	host_allocs[i].tag = tag;
	if (owner)
		*(void **)owner = p;
	host_live_blocks++;
	host_live_bytes += size;
	if (host_live_bytes > host_peak_bytes)
		host_peak_bytes = host_live_bytes;
	return p;
}

void *Z_CallocAlign(size_t size, INT32 tag, void *owner, INT32 bits)
{
	return memset(Z_MallocAlign(size, tag, owner, bits), 0, size);
}

void Z_Free(void *p)
{
	if (p)
	{
		size_t i = host_allocation(p);
		for (size_t j = 0; j < 16; j++)
		{
			host_check(((UINT8 *)p - 16)[j] == 0xCD, "leading payload red zone");
			host_check(((UINT8 *)p)[host_allocs[i].size + j] == 0xCD, "trailing payload red zone");
		}
		if (host_allocs[i].owner)
			*host_allocs[i].owner = NULL;
		host_live_blocks--;
		host_live_bytes -= host_allocs[i].size;
		free(host_allocs[i].raw);
		memset(&host_allocs[i], 0, sizeof host_allocs[i]);
	}
}

void *Z_ReallocAlign(void *p, size_t size, INT32 tag, void *owner, INT32 bits)
{
	void *q;
	if (!size)
	{
		Z_Free(p);
		return NULL;
	}
	q = Z_CallocAlign(size, tag, owner, bits);
	if (p)
	{
		size_t old = host_allocs[host_allocation(p)].size;
		memcpy(q, p, old < size ? old : size);
		Z_Free(p);
		if (owner)
			*(void **)owner = q;
	}
	return q;
}

void Z_ChangeTag(void *p, INT32 tag)
{
	host_allocs[host_allocation(p)].tag = tag;
}

static void host_purge_cache(void)
{
	for (size_t i = 0; i < sizeof host_allocs / sizeof host_allocs[0]; i++)
		if (host_allocs[i].p && host_allocs[i].tag == PU_CACHE && host_allocs[i].owner)
			Z_Free(host_allocs[i].p);
}

static void host_free_all(void)
{
	for (size_t i = 0; i < sizeof host_allocs / sizeof host_allocs[0]; i++)
		Z_Free(host_allocs[i].p);
	host_check(host_live_blocks == 0 && host_live_bytes == 0, "renderer allocations released");
}

void I_Error(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	exit(2);
}

void CONS_Printf(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vprintf(fmt, ap);
	va_end(ap);
}
