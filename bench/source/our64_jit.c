#include "our64_jit.h"

#include <coreinit/cache.h>
#include <coreinit/codegen.h>
#include <stdint.h>

#define STUB_AREA   (64 * 1024)
#define STUB_WORDS  4
#define MAX_STUBS   (STUB_AREA / (STUB_WORDS * 4) - 4) /* last 64 bytes: self-test */

static uint8_t     *g_base;
static uint32_t     g_size;
static int          g_depth;
static unsigned int g_switches;
static void        *g_targets[MAX_STUBS];
static unsigned int g_stubs;

int our64_jit_init(void)
{
	void *base = 0;
	uint32_t size = 0;
	OSGetCodegenVirtAddrRange(&base, &size);
	if (!base || size <= STUB_AREA)
		return -1;
	g_base = base;
	g_size = size;
	return 0;
}

void *our64_jit_heap_start(void)        { return g_base + STUB_AREA; }
unsigned int our64_jit_heap_size(void)  { return g_size - STUB_AREA; }
unsigned int our64_jit_stub_count(void) { return g_stubs; }
unsigned int our64_jit_switches(void)   { return g_switches; }

void our64_jit_write_begin(void)
{
	if (g_depth++ == 0) {
		OSSwitchSecCodeGenMode(CODEGEN_RW_);
		g_switches++;
	}
}

void our64_jit_write_end(void)
{
	if (--g_depth == 0)
		OSSwitchSecCodeGenMode(CODEGEN_R_X);
}

void *our64_jit_call_stub(void *target)
{
	unsigned int i;
#ifdef OUR64_DIRECT_CALLS_WHEN_REACHABLE
	{
		int32_t lo = (int32_t)((uint32_t)target - (uint32_t)g_base);
		int32_t hi = (int32_t)((uint32_t)target - ((uint32_t)g_base + g_size));
		if (lo > -0x1F00000 && lo < 0x1F00000 && hi > -0x1F00000 && hi < 0x1F00000)
			return target;
	}
#endif
	for (i = 0; i < g_stubs; i++)
		if (g_targets[i] == target)
			return g_base + i * STUB_WORDS * 4;
	if (g_stubs == MAX_STUBS) {
		void abort(void);
		abort(); /* reported by the platform layer */
	}

	uint32_t *stub = (uint32_t *)(g_base + g_stubs * STUB_WORDS * 4);
	uint32_t t = (uint32_t)target;
	our64_jit_write_begin();
	stub[0] = 0x3D800000 | (t >> 16);    /* lis   r12, target@h */
	stub[1] = 0x618C0000 | (t & 0xFFFF); /* ori   r12, r12, target@l */
	stub[2] = 0x7D8903A6;                /* mtctr r12 */
	stub[3] = 0x4E800420;                /* bctr  (LR still points back into the caller) */
	DCFlushRange(stub, STUB_WORDS * 4);
	our64_jit_write_end();
	ICInvalidateRange(stub, STUB_WORDS * 4);
	g_targets[g_stubs++] = target;
	return stub;
}

/* Exit stubs share the area with call stubs, told apart by the low bit of
 * their key (MIPS addresses are word aligned, C functions too). */
void *our64_jit_exit_stub(unsigned int mips)
{
	unsigned int i;
	void *key = (void *)(uintptr_t)(mips | 1);
	for (i = 0; i < g_stubs; i++)
		if (g_targets[i] == key)
			return g_base + i * STUB_WORDS * 4;
	if (g_stubs == MAX_STUBS)
		return 0;
	uint32_t *stub = (uint32_t *)(g_base + g_stubs * STUB_WORDS * 4);
	our64_jit_write_begin();
	stub[0] = 0x3C600000 | (mips >> 16);    /* lis r3, mips@h */
	stub[1] = 0x60630000 | (mips & 0xFFFF); /* ori r3, r3, mips@l */
	stub[2] = 0x4E800020;                   /* blr: LR is the dispatcher's */
	stub[3] = 0x60000000;                   /* nop */
	DCFlushRange(stub, STUB_WORDS * 4);
	our64_jit_write_end();
	ICInvalidateRange(stub, STUB_WORDS * 4);
	g_targets[g_stubs++] = key;
	return stub;
}

/* Self-test: generated code loads six arguments and calls a C function
 * through a stub, the same way the recompiler calls its helpers. */
static unsigned selftest_helper(unsigned a, unsigned b, unsigned c, unsigned d, unsigned e, unsigned f)
{
	return a * 1 + b * 3 + c * 7 + d * 15 + e * 31 + f * 63;
}

int our64_jit_selftest(unsigned *got, unsigned *want)
{
	uint32_t *code = (uint32_t *)(g_base + STUB_AREA - 64); /* last 64 bytes of the stub area */
	void *stub = our64_jit_call_stub((void *)selftest_helper);
	int n = 0;
	our64_jit_write_begin();
	code[n++] = 0x9421FFF0;                        /* stwu  r1,-16(r1) */
	code[n++] = 0x7C0802A6;                        /* mflr  r0 */
	code[n++] = 0x90010014;                        /* stw   r0,20(r1) */
	code[n++] = 0x38600001;                        /* li    r3,1 */
	code[n++] = 0x38800002;                        /* li    r4,2 */
	code[n++] = 0x38A00003;                        /* li    r5,3 */
	code[n++] = 0x38C00004;                        /* li    r6,4 */
	code[n++] = 0x3CE08000;                        /* lis   r7,0x8000 */
	code[n++] = 0x60E71234;                        /* ori   r7,r7,0x1234 */
	code[n++] = 0x39000006;                        /* li    r8,6 */
	uint32_t *bl = &code[n++];
	*bl = 0x48000001 | (((uint8_t *)stub - (uint8_t *)bl) & 0x03FFFFFC); /* bl stub */
	code[n++] = 0x80010014;                        /* lwz   r0,20(r1) */
	code[n++] = 0x7C0803A6;                        /* mtlr  r0 */
	code[n++] = 0x38210010;                        /* addi  r1,r1,16 */
	code[n++] = 0x4E800020;                        /* blr */
	DCFlushRange(code, n * 4);
	our64_jit_write_end();
	ICInvalidateRange(code, n * 4);
	*want = selftest_helper(1, 2, 3, 4, 0x80001234, 6);
	*got = ((unsigned (*)(void))code)();
	return *got == *want ? 0 : -1;
}
