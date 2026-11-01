/* Time spent in malloc and free, per core (the Makefile links with
 * --wrap=malloc and friends). Shader compiles allocate a lot, and newlib's
 * malloc has one lock for all cores, so this shows when they wait for it. */
#include <stddef.h>
#include <coreinit/core.h>
#include <coreinit/time.h>

volatile unsigned long long our64_alloc_ticks[3];
volatile unsigned our64_alloc_calls[3];

void *__real_malloc(size_t size);
void *__real_calloc(size_t count, size_t size);
void *__real_realloc(void *ptr, size_t size);
void __real_free(void *ptr);

static inline void count(OSTime start)
{
	unsigned core = OSGetCoreId();
	if (core < 3) {
		our64_alloc_ticks[core] += (unsigned long long)(OSGetSystemTime() - start);
		our64_alloc_calls[core]++;
	}
}

void *__wrap_malloc(size_t size)
{
	OSTime t = OSGetSystemTime();
	void *p = __real_malloc(size);
	count(t);
	return p;
}

void *__wrap_calloc(size_t count_, size_t size)
{
	OSTime t = OSGetSystemTime();
	void *p = __real_calloc(count_, size);
	count(t);
	return p;
}

void *__wrap_realloc(void *ptr, size_t size)
{
	OSTime t = OSGetSystemTime();
	void *p = __real_realloc(ptr, size);
	count(t);
	return p;
}

void __wrap_free(void *ptr)
{
	OSTime t = OSGetSystemTime();
	__real_free(ptr);
	count(t);
}
