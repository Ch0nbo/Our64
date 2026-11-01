/* Our64: the part of libogc's lwp_heap that Not64 uses, backed by a coreinit
 * expanded heap. A heap created over the JIT area switches the area to
 * writable around every call (see jit.h). */
#ifndef OUR64_LWP_HEAP_H
#define OUR64_LWP_HEAP_H
#include <gctypes.h>

typedef struct {
	void *handle;   /* MEMHeapHandle */
	int   in_jit;
} heap_cntrl;

u32   __lwp_heap_init(heap_cntrl *heap, void *start, u32 size, u32 page_size);
void *__lwp_heap_allocate(heap_cntrl *heap, u32 size);
BOOL  __lwp_heap_free(heap_cntrl *heap, void *ptr);
#endif
