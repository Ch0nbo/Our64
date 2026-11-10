/* Our64: libogc lwp_heap calls used by Not64's recompile cache.
 *
 * The code heap sits in the Wii U JIT area, which takes stores from this app's
 * own code but faults on stores from coreinit (seen on hardware: a coreinit
 * expanded heap faulted writing its header there). So the code heap keeps all
 * its bookkeeping in ordinary memory and never writes into the area itself:
 * a first-fit free list sorted by address, plus a table of live blocks.
 * Other heaps (the recompiler's metadata) use a coreinit expanded heap. */
#include <ogc/lwp_heap.h>
#include <coreinit/memexpheap.h>

#include <stdint.h>
#include <stdlib.h>

unsigned our64_heap_allocs, our64_heap_failures;
static void *g_code_heap; /* the one heap in the JIT area, for our64_code_heap_stats */

typedef struct extent { uint32_t start, size; struct extent *next; } extent;

typedef struct {
	extent   *free_list;
	uint32_t *keys, *sizes;  /* open addressing: block start -> size; 0 = empty, 1 = deleted */
	uint32_t  cap, used;     /* used: blocks alive and deleted slots */
	uint32_t  live;          /* blocks alive */
	uint32_t  align;
} code_heap;

static uint32_t slot(code_heap *h, uint32_t key)
{
	return (key >> 5) * 2654435761u & (h->cap - 1);
}

static void table_put(code_heap *h, uint32_t key, uint32_t size);

/* Rebuilt when blocks alive and deleted slots fill half the table: twice
 * the size only when the blocks alive fill a quarter, else the same size
 * with the deleted slots gone. It doubled every time, so it followed every
 * block ever made, not the code alive: 2 million compiles with 3,000 blocks
 * alive left 2 million slots, 16 MB (tests/test_lwp_heap.c). */
static void table_grow(code_heap *h)
{
	uint32_t old_cap = h->cap, *old_keys = h->keys, *old_sizes = h->sizes;
	h->cap = !old_cap ? 4096 : (h->live + 1) * 4 > old_cap ? old_cap * 2 : old_cap;
	h->keys = calloc(h->cap, 4);
	h->sizes = calloc(h->cap, 4);
	h->used = h->live = 0;
	for (uint32_t i = 0; i < old_cap; i++)
		if (old_keys[i] > 1)
			table_put(h, old_keys[i], old_sizes[i]);
	free(old_keys);
	free(old_sizes);
}

static void table_put(code_heap *h, uint32_t key, uint32_t size)
{
	if ((h->used + 1) * 2 > h->cap)
		table_grow(h);
	uint32_t i = slot(h, key);
	while (h->keys[i] > 1)
		i = (i + 1) & (h->cap - 1);
	h->keys[i] = key;
	h->sizes[i] = size;
	h->used++;
	h->live++;
}

static uint32_t table_take(code_heap *h, uint32_t key)
{
	uint32_t i = slot(h, key);
	while (h->keys[i]) {
		if (h->keys[i] == key) {
			h->keys[i] = 1;
			h->live--;
			return h->sizes[i];
		}
		i = (i + 1) & (h->cap - 1);
	}
	return 0;
}

static void *code_alloc(code_heap *h, uint32_t size)
{
	size = (size + h->align - 1) & ~(h->align - 1);
	for (extent **pp = &h->free_list; *pp; pp = &(*pp)->next) {
		extent *e = *pp;
		if (e->size < size)
			continue;
		uint32_t p = e->start;
		e->start += size;
		e->size -= size;
		if (!e->size) {
			*pp = e->next;
			free(e);
		}
		table_put(h, p, size);
		return (void *)p;
	}
	return NULL;
}

static void code_free(code_heap *h, uint32_t p)
{
	uint32_t size = table_take(h, p);
	if (!size)
		return;
	extent **pp = &h->free_list, *prev = NULL;
	while (*pp && (*pp)->start < p) {
		prev = *pp;
		pp = &(*pp)->next;
	}
	extent *next = *pp;
	if (prev && prev->start + prev->size == p) {
		prev->size += size;              /* joins the block before */
		if (next && prev->start + prev->size == next->start) {
			prev->size += next->size;    /* and the block after */
			prev->next = next->next;
			free(next);
		}
	} else if (next && p + size == next->start) {
		next->start = p;                 /* joins the block after */
		next->size += size;
	} else {
		extent *e = malloc(sizeof(extent));
		e->start = p;
		e->size = size;
		e->next = next;
		*pp = e;
	}
}

u32 __lwp_heap_init(heap_cntrl *heap, void *start, u32 size, u32 page_size)
{
	if (heap->in_jit) {
		code_heap *h = calloc(1, sizeof(code_heap));
		h->align = page_size < 32 ? 32 : page_size;
		uint32_t s = ((uint32_t)start + h->align - 1) & ~(h->align - 1);
		h->free_list = malloc(sizeof(extent));
		h->free_list->start = s;
		h->free_list->size = (uint32_t)start + size - s;
		h->free_list->next = NULL;
		table_grow(h);
		heap->handle = h;
		g_code_heap = h;
		return size;
	}
	heap->handle = MEMCreateExpHeapEx(start, size, MEM_HEAP_FLAG_USE_LOCK);
	return heap->handle ? size : 0;
}

void *__lwp_heap_allocate(heap_cntrl *heap, u32 size)
{
	if (heap->in_jit) {
		void *p = code_alloc((code_heap *)heap->handle, size);
		our64_heap_allocs++;
		if (!p) our64_heap_failures++;
		return p;
	}
	return MEMAllocFromExpHeapEx((MEMHeapHandle)heap->handle, size, 32);
}

BOOL __lwp_heap_free(heap_cntrl *heap, void *ptr)
{
	if (!ptr) return FALSE;
	if (heap->in_jit)
		code_free((code_heap *)heap->handle, (uint32_t)ptr);
	else
		MEMFreeToExpHeap((MEMHeapHandle)heap->handle, ptr);
	return TRUE;
}

/* The JIT area's heap: blocks alive, the block table's slots, and the free
 * space with the pieces it is in (each allocation walks them). Call it on the
 * thread that compiles. */
void our64_code_heap_stats(unsigned *live, unsigned *slots, unsigned *free_kb, unsigned *pieces)
{
	code_heap *h = g_code_heap;
	*live = *slots = *free_kb = *pieces = 0;
	if (!h)
		return;
	*live = h->live;
	*slots = h->cap;
	uint32_t bytes = 0;
	for (extent *e = h->free_list; e; e = e->next) {
		bytes += e->size;
		(*pieces)++;
	}
	*free_kb = bytes / 1024;
}
