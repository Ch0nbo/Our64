/* PC tests: coreinit's expanded heap on malloc (lwp_heap.c's metadata heaps). */
#ifndef OUR64_TEST_MEMEXPHEAP_H
#define OUR64_TEST_MEMEXPHEAP_H
#include <stdlib.h>
#include <stdint.h>
typedef void *MEMHeapHandle;
#define MEM_HEAP_FLAG_USE_LOCK 4
static inline MEMHeapHandle MEMCreateExpHeapEx(void *start, uint32_t size, uint32_t flags)
{ (void)size; (void)flags; return start; }
static inline void *MEMAllocFromExpHeapEx(MEMHeapHandle h, uint32_t size, int align)
{ (void)h; return aligned_alloc((size_t)align, ((size_t)size + (size_t)align - 1) & ~((size_t)align - 1)); }
static inline void MEMFreeToExpHeap(MEMHeapHandle h, void *p) { (void)h; free(p); }
#endif
