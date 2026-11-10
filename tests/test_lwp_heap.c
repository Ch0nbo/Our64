/* The JIT code heap's bookkeeping (bench/source/lwp_heap.c): blocks come and
 * go as Not64 compiles and frees code, as in an hour of play, and the
 * heap's memory has to follow the code alive, not every block ever made. */
#include "../bench/source/lwp_heap.c"

#include <stdio.h>

static int fails;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

#define AREA_START 0x10000000u /* never written: the code heap keeps its books elsewhere */
#define AREA_SIZE  (16u * 1024 * 1024)
#define LIVE       3000

int main(void)
{
	heap_cntrl heap = { .in_jit = 1 };
	__lwp_heap_init(&heap, (void *)(uintptr_t)AREA_START, AREA_SIZE, 32);
	code_heap *h = heap.handle;

	/* Churn: 3,000 blocks alive at any time, 2 million made and freed (a game
	 * that recompiles code it loads again, for an hour or so). */
	static void *live[LIVE];
	unsigned seed = 1, failed = 0;
	for (unsigned n = 0; n < 2000000; n++) {
		unsigned k = n % LIVE;
		if (live[k])
			__lwp_heap_free(&heap, live[k]);
		seed = seed * 1103515245u + 12345u;
		live[k] = __lwp_heap_allocate(&heap, 64 + (seed >> 16) % 2048);
		failed += live[k] == NULL;
	}
	CHECK(failed == 0, "%u allocations failed with %u blocks alive in 16 MB", failed, LIVE);
	CHECK(h->cap <= 4 * 4096, "the block table holds %u slots (%u KB) for %u blocks alive",
	      h->cap, h->cap * 8 / 1024, LIVE);

	/* Every block alive is still found and freed: the area is one free extent again. */
	for (unsigned k = 0; k < LIVE; k++)
		__lwp_heap_free(&heap, live[k]);
	CHECK(h->free_list && !h->free_list->next && h->free_list->size == AREA_SIZE,
	      "after freeing everything the free list is not the whole area");

	printf(fails ? "test_lwp_heap: %d failed\n" : "test_lwp_heap: all passed\n", fails);
	return fails != 0;
}
