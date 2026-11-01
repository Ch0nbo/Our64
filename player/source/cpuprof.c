/* sd:/wiiu/our64/profile-cpu: the CPU core's PC and LR, sampled every 250 us
 * while a game runs and no menu is up (an alarm on the CPU core), for
 * tools/profile-cpu.py. When the game stops, the last 64K samples (about the
 * last 16 s) go to profile/player-cpu.prof, followed by every compiled
 * function: its MIPS range, the generated code of each MIPS instruction that
 * starts a mapping, and the MIPS words, so samples in generated code map back
 * to N64 code (Battle for Naboo at 40 VI/s, 2026-10-26). */
#include <gctypes.h>
#include "r4300/r4300.h"
#include "r4300/ppc/Recompile.h"
#include "gc_memory/memory.h"

#include "our64_jit.h"
#include "report.h"
#include "player.h"

#include <coreinit/alarm.h>
#include <coreinit/context.h>
#include <coreinit/thread.h>
#include <coreinit/time.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define PROF_MAX (64 * 1024)

/* PC, LR, the thread that ran, and in the CPU thread the first return address
 * on its stack outside the system's code (who called the system). */
static uint32_t g_samples[PROF_MAX][4];
static OSThread *g_thread;
static uint32_t g_stack_lo, g_stack_hi;
static volatile uint32_t g_n;
static volatile int g_active;
static volatile int *g_paused; /* main.c's: a menu or a missing controller holds the game */
static int g_on, g_alarm_set;
static OSAlarm g_alarm;

static void sample(OSAlarm *a, OSContext *c)
{
	(void)a;
	const uint32_t n = g_n;
	if (g_active && !*g_paused && c) {
		uint32_t *s = g_samples[n % PROF_MAX];
		OSThread *t = OSGetCurrentThread();
		s[0] = c->srr0;
		s[1] = c->lr;
		s[2] = (uint32_t)(uintptr_t)t;
		s[3] = 0;
		if (t == g_thread && c->srr0 >= 0x01000000 && c->srr0 < 0x01800000) {
			uint32_t sp = c->gpr[1];
			for (int k = 0; k < 8 && sp >= g_stack_lo && sp < g_stack_hi && !(sp & 3); k++) {
				const uint32_t back = *(const uint32_t *)(uintptr_t)sp;
				if (back <= sp || back >= g_stack_hi)
					break;
				const uint32_t ra = *(const uint32_t *)(uintptr_t)(back + 4);
				if (ra < 0x01000000 || ra >= 0x01800000) {
					s[3] = ra;
					break;
				}
				sp = back;
			}
		}
		g_n = n + 1;
	}
}

/* On the CPU core, when a game starts: the alarm fires on the core that set it. */
void cpuprof_start(const char *root, volatile int *paused)
{
	struct stat st;
	char path[256];
	snprintf(path, sizeof(path), "%s/profile-cpu", root);
	g_on = stat(path, &st) == 0;
	if (!g_on)
		return;
	g_n = 0;
	g_paused = paused;
	g_thread = OSGetCurrentThread();
	g_stack_lo = (uint32_t)(uintptr_t)g_thread->stackEnd;
	g_stack_hi = (uint32_t)(uintptr_t)g_thread->stackStart;
	if (!g_alarm_set) {
		g_alarm_set = 1;
		OSCreateAlarm(&g_alarm);
		const OSTime period = OSMicrosecondsToTicks(250);
		OSSetPeriodicAlarm(&g_alarm, OSGetTime() + period, period, sample);
	}
	g_active = 1;
	report_line("PROFILE: sampling the CPU core every 250 us (profile-cpu)");
}

static FILE *g_out;
static uint32_t g_funcs;

static void put(uint32_t w)
{
	fwrite(&w, 4, 1, g_out);
}

static void put_func(const PowerPC_block *b, const PowerPC_func *f)
{
	const uint32_t n = (f->end_addr - f->start_addr) >> 2;
	put(f->start_addr);
	put(n);
	put((uint32_t)(uintptr_t)f->code);
	for (uint32_t i = 0; i < n; i++)
		put(f->code_addr ? (uint32_t)(uintptr_t)f->code_addr[i] : 0);
	const uint32_t at = (f->start_addr - b->start_address) >> 2;
	for (uint32_t i = 0; i < n; i++)
		put(f->our64_mips && i < f->our64_count ? f->our64_mips[i]
		    : b->mips_code && at + i < 1024 ? b->mips_code[at + i] : 0);
	g_funcs++;
}

static void put_tree(const PowerPC_block *b, const PowerPC_func_node *node)
{
	if (!node)
		return;
	put_tree(b, node->left);
	if (node->function && node->function->code)
		put_func(b, node->function);
	put_tree(b, node->right);
}

/* A reset: cpu_deinit frees the code the samples so far point at. */
void cpuprof_restart(void)
{
	g_n = 0;
}

#ifdef OUR64_PROFILE_CODE
/* The flush-measurement build (Not64 with OUR64_PROFILE_CODE): after the
 * functions, "CODE", the offset of reg[] from r13 (generated code keeps the
 * N64 registers there: stw/lwz rN, offset(r13)), then each function in the
 * code cache: its code address, size in bytes and the code itself, ended by
 * 0xFFFFFFFF. tools/profile-cpu.py --flush counts the samples on reg[] stores
 * and loads (register flushes and reloads) against the rest. The code is read
 * with the JIT area writable: only the codegen core can switch it (this one). */
extern long long int reg[34];
extern char *our64_sda_base;
void our64_cache_each(void (*fn)(PowerPC_func *func, unsigned int size));

static void put_code(PowerPC_func *f, unsigned int size)
{
	static uint32_t tmp[64 * 1024];
	if (size > sizeof(tmp))
		size = sizeof(tmp);
	our64_jit_write_begin();
	memcpy(tmp, f->code, size);
	our64_jit_write_end();
	put((uint32_t)(uintptr_t)f->code);
	put(size);
	fwrite(tmp, 1, size & ~3u, g_out);
}
#endif

/* On the CPU core, before cpu_deinit frees the compiled code. The alarm goes:
 * a periodic one left behind hung the next launches (replay app). */
void cpuprof_save(void)
{
	g_active = 0;
	if (g_alarm_set) {
		OSCancelAlarm(&g_alarm);
		g_alarm_set = 0;
	}
	if (!g_on || g_n == 0)
		return;
	char path[256];
	const char *root = report_sd_root();
	snprintf(path, sizeof(path), "%s/profile", root ? root : "");
	mkdir(path, 0777);
	snprintf(path, sizeof(path), "%s/profile/player-cpu.prof", root ? root : "");
	g_out = fopen(path, "wb");
	if (!g_out)
		return;
	static char buf[256 * 1024];
	setvbuf(g_out, buf, _IOFBF, sizeof(buf));
	const uint32_t n = g_n, kept = n < PROF_MAX ? n : PROF_MAX, first = n - kept;
	put(0x4F363444); /* "O64D" */
	put(kept);
	put((uint32_t)(uintptr_t)our64_jit_heap_start());
	put(our64_jit_heap_size());
	put((uint32_t)(uintptr_t)&cpuprof_save); /* where the app's code was loaded */
	put((uint32_t)(uintptr_t)g_thread);
	for (uint32_t i = first; i < n; i++)
		fwrite(g_samples[i % PROF_MAX], sizeof(g_samples[0]), 1, g_out);
	g_funcs = 0;
	for (uint32_t i = 0; i < 0x100000; i++)
		if (blocks[i])
			put_tree(blocks[i], blocks[i]->funcs);
	put(0xFFFFFFFF); /* the end of the functions */
#ifdef OUR64_PROFILE_CODE
	put(0x434F4445); /* "CODE" */
	put((uint32_t)((char *)reg - our64_sda_base));
	our64_cache_each(put_code);
	put(0xFFFFFFFF);
#endif
	fclose(g_out);
	g_out = NULL;
	report_line("PROFILE: the last %u of %u CPU core samples and %u compiled functions written to %s",
	            (unsigned)kept, (unsigned)n, (unsigned)g_funcs, path);
	g_n = 0;
}
