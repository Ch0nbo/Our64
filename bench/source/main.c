/* Our64 stage-1 headless CPU benchmark.
 *
 * Runs Not64's CPU core (recompiler in the Wii U JIT area) on every ROM in
 * sd:/wiiu/our64/roms with no graphics and no sound output. RSP tasks go to
 * Not64's RSP HLE plugin, so audio processing cost is included; graphics tasks
 * are marked done instantly. Per ROM:
 *   1. speed run: 40 s, speed measured over the last 30 s
 *   2. correctness: the first 300 VIs under the recompiler and under the pure
 *      interpreter, comparing an RDRAM hash every 30 VIs
 * Results: screen, UDP port 4405 and sd:/wiiu/our64/bench-results.txt.
 *
 * With sd:/wiiu/our64/diverge.txt ("<rom file> [VIs] [every]") the bench only
 * looks for the first point where the recompiler and the interpreter differ
 * in that ROM, and logs the memory words and registers that differ there. */
#include <gctypes.h>
#include "main/rom.h"
#include "r4300/r4300.h"
#include "r4300/ppc/Wrappers.h"
#include "r4300/ppc/Recompile.h"
#include "gc_memory/memory.h"
#include "gc_memory/tlb.h"
#include "gc_memory/pif.h"
#include "main/ROM-Cache.h"

#include "bench.h"
#include "our64_jit.h"
#include "report.h"
#include "input.h"
#include "capture.h"

#include <coreinit/codegen.h>
#include <coreinit/context.h>
#include <coreinit/exception.h>
#include <coreinit/memdefaultheap.h>
#include <coreinit/thread.h>
#include <coreinit/time.h>
#include <whb/log_cafe.h>
#include <whb/log_console.h>
#include <whb/log_udp.h>
#include <whb/proc.h>
#include <sysapp/launch.h>

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#define SPEED_SECONDS   40
#define SPEED_WINDOW_S  10   /* steady state measured from here to the end */
#define COMPARE_VIS     300
#define COMPARE_EVERY   30
#define INTERP_LIMIT_S  120
#define CAPTURE_AT_VI   1500 /* 25 s of game time */
#define CAPTURE_TASKS   8

/* ---- storage the Not64 core expects from its platform layer ---- */

char our64_mem2[OUR64_MEM2_SIZE] __attribute__((aligned(64)));
char *our64_sda_base;
char txtbuffer[1024];
bench_counters g_bench;

extern unsigned long dynacore;
extern long long int reg_cop1_fgr_64[32];
extern unsigned int nextLRU;
extern int noCheckInterrupt;
extern const float two16;
extern double our64_cvt_tmp;
extern const double our64_cvt_magic;
extern unsigned long rdram[0x800000 / 4];
void tlb_mem2_init(void);
void format_mempacks(void);
int  rom_read(fileBrowser_file *file);
void cpu_init(void);
void cpu_deinit(void);
void go(void);
int  init_memory(void);
void free_memory(void);
void bench_plugins_init(void);

void DEBUG_print(char *string, int pos) { (void)string; (void)pos; }
void DEBUG_stats(int id, char *info, unsigned int type, unsigned int adj) { (void)id; (void)info; (void)type; (void)adj; }
void InitTimer(void) {}
void new_frame(void) {}

/* ---- profiling sections (Not64's PROFILE hooks) ---- */

/* Timer ticks, not microseconds: the dispatcher enters and leaves a section
 * hundreds of thousands of times a second, and converting each reading took a
 * 64-bit division. */
static OSTime g_section_start[16];
static double g_us_per_tick;
static int g_jr_ab;

void start_section(int s)
{
	if (s == AUDIO_SECTION) g_bench.audio_tasks++;
	g_bench.section_calls[s]++;
	g_section_start[s] = OSGetSystemTime();
}

void end_section(int s)
{
	if (g_us_per_tick == 0) g_us_per_tick = 1e6 / OSTimerClockSpeed;
	g_bench.section_us[s] += (double)(OSGetSystemTime() - g_section_start[s]) * g_us_per_tick;
}

void refresh_stat(void) {}

/* ---- divergence search (diverge.txt) ---- */

#define DIV_PAGES  (0x800000 / 4096)
#define DIV_SLOTS  (DIV_PAGES + 2)    /* 4 KB pages of RDRAM, then the GPRs, then the FPRs */
#define DIV_MAX_PAGES 64

extern long long int reg_cop1_fgr_64[32];
static int g_div_mode;             /* 1 record (recompiler), 2 compare (interpreter), 3 collect (recompiler) */
static uint32_t g_div_every, g_div_vis;
static uint32_t *g_div_table;      /* the recompiler's hashes, DIV_SLOTS per checkpoint */
static int g_div_first = -1;       /* first checkpoint that differs */
static uint16_t g_div_slot[DIV_MAX_PAGES];
static int g_div_nslots;
static uint32_t *g_div_saved;      /* the interpreter's copy of each differing slot (1024 words) */

static uint32_t fnv_words(const uint32_t *w, unsigned n)
{
	uint32_t h = 2166136261u;
	for (unsigned i = 0; i < n; i++)
		h = (h ^ w[i]) * 16777619u;
	return h;
}

/* A slot's words: an RDRAM page, or the registers as 32-bit words. */
static const uint32_t *div_slot_words(int slot, unsigned *n)
{
	if (slot < DIV_PAGES) { *n = 1024; return (const uint32_t *)rdram + slot * 1024; }
	if (slot == DIV_PAGES) { *n = 64; return (const uint32_t *)reg; }
	*n = 64;
	return (const uint32_t *)reg_cop1_fgr_64;
}

static void div_hash(uint32_t *out)
{
	for (int k = 0; k < DIV_SLOTS; k++) {
		unsigned n;
		const uint32_t *w = div_slot_words(k, &n);
		out[k] = fnv_words(w, n);
	}
}

static void div_checkpoint(void)
{
	int k = g_bench.vis / g_div_every - 1;
	if (k < 0 || (uint32_t)k >= g_div_vis / g_div_every)
		return;
	uint32_t *row = g_div_table + k * DIV_SLOTS;
	if (g_div_mode == 1) {
		div_hash(row);
		return;
	}
	if (g_div_mode == 2) {
		static uint32_t now[DIV_SLOTS];
		div_hash(now);
		g_div_nslots = 0;
		for (int i = 0; i < DIV_SLOTS; i++)
			if (now[i] != row[i] && g_div_nslots < DIV_MAX_PAGES) {
				unsigned n;
				const uint32_t *w = div_slot_words(i, &n);
				memcpy(g_div_saved + g_div_nslots * 1024, w, n * 4);
				g_div_slot[g_div_nslots++] = i;
			}
		if (g_div_nslots) {
			g_div_first = k;
			stop = 1;
		}
		return;
	}
	if (g_div_mode == 3 && k == g_div_first) {
		int words = 0;
		for (int j = 0; j < g_div_nslots; j++) {
			unsigned n;
			const uint32_t *w = div_slot_words(g_div_slot[j], &n);
			const uint32_t *iw = g_div_saved + j * 1024;
			for (unsigned i = 0; i < n; i++) {
				if (w[i] == iw[i]) continue;
				if (g_div_slot[j] < DIV_PAGES) {
					if (words++ < 256)
						report_line("   word 0x%08X: recompiler 0x%08X, interpreter 0x%08X",
						            0x80000000u + g_div_slot[j] * 4096 + i * 4, (unsigned)w[i], (unsigned)iw[i]);
				} else {
					report_line("   %s %u (%s word): recompiler 0x%08X, interpreter 0x%08X",
					            g_div_slot[j] == DIV_PAGES ? "GPR" : "FPR", i / 2, i & 1 ? "low" : "high",
					            (unsigned)w[i], (unsigned)iw[i]);
				}
			}
		}
		if (words > 256)
			report_line("   ... %d more RDRAM words differ", words - 256);
		stop = 1;
	}
}

/* ---- per-VI hook ---- */


uint32_t bench_hash_rdram(void)
{
	uint32_t h = 2166136261u;
	for (unsigned i = 0; i < 0x800000 / 4; i++)
		h = (h ^ (uint32_t)rdram[i]) * 16777619u;
	return h;
}

/* Called at the start of each graphics task, before the counter moves. */
void bench_on_gfx_task(void)
{
	if (!g_bench.capture_left || g_bench.vis < g_bench.capture_from_vi)
		return;
	if (capture_frame(g_bench.vis, g_bench.gfx_tasks))
		g_bench.capture_errors++;
	else
		g_bench.captured++;
	if (--g_bench.capture_left == 0) {
		/* the next capture point in the ROM's script, if any */
		const uint32_t next = input_capture_vi(++g_bench.capture_point);
		if (next) {
			g_bench.capture_from_vi = next;
			g_bench.capture_left = CAPTURE_TASKS;
			capture_group(next);
		} else {
			stop = 1;
		}
	}
}

void new_vi(void)
{
	g_bench.vis++;
	double now = report_now_us();

	if (g_bench.window_from_us && !g_bench.window_start_us && now >= g_bench.window_from_us) {
		g_bench.window_start_us = now;
		g_bench.window_start_vis = g_bench.vis;
		g_bench.window_start_gfx = g_bench.gfx_tasks;
	}
	if (g_div_mode && g_bench.vis % g_div_every == 0)
		div_checkpoint();
	if (g_bench.hash_every && g_bench.vis % g_bench.hash_every == 0 && g_bench.nhashes < 64)
		g_bench.hashes[g_bench.nhashes++] = bench_hash_rdram();
	if ((g_bench.stop_after_vis && g_bench.vis >= g_bench.stop_after_vis) ||
	    (g_bench.deadline_us && now >= g_bench.deadline_us))
		stop = 1;
}

/* ---- small data base for generated code ---- */

static int sda_setup(void)
{
	struct { const void *p; unsigned n; } v[] = {
		{ reg, sizeof(long long) * 34 }, { reg_cop0, 4 * 32 },
		{ reg_cop1_simple, 4 * 32 }, { reg_cop1_double, 4 * 32 },
		{ reg_cop1_fgr_64, 8 * 32 }, { &llbit, 4 }, { &local_rs32, 4 },
		{ &FCR31, 4 }, { &last_addr, 4 }, { &next_interupt, 4 },
		{ &nextLRU, 4 }, { &noCheckInterrupt, 4 }, { &two16, 4 },
		{ &our64_cvt_tmp, 8 }, { &our64_cvt_magic, 8 },
	};
	/* (not "lo"/"hi": the core defines those as macros) */
	uintptr_t first = ~(uintptr_t)0, end = 0;
	for (unsigned i = 0; i < sizeof(v) / sizeof(v[0]); i++) {
		uintptr_t a = (uintptr_t)v[i].p;
		if (a < first) first = a;
		if (a + v[i].n > end) end = a + v[i].n;
	}
	report_line("small data: %u bytes at 0x%08X", (unsigned)(end - first), (unsigned)first);
	if (end - first > 0xFFF0)
		return -1;
	our64_sda_base = (char *)first + 0x8000;
	return 0;
}

/* ---- crash report: send the faulting address before Cafe OS stops the app ---- */

static const char *where(uint32_t a)
{
	uint32_t base = (uint32_t)our64_jit_heap_start() - 64 * 1024;
	if (a >= base && a < base + our64_jit_heap_size() + 64 * 1024) return "generated code";
	if (a >= 0x02000000 && a < 0x10000000) return "app code";
	return "elsewhere";
}

static BOOL crash_report(const char *what, OSContext *c)
{
	report_line("CRASH: %s at 0x%08X (%s), lr 0x%08X (%s), dar 0x%08X, dsisr 0x%08X", what, c->srr0,
	            where(c->srr0), c->lr, where(c->lr), c->dar, c->dsisr);
	report_line("CRASH: r1 0x%08X r3 0x%08X r4 0x%08X r13 0x%08X r29 0x%08X r30 0x%08X r31 0x%08X; N64 PC 0x%08X",
	            c->gpr[1], c->gpr[3], c->gpr[4], c->gpr[13], c->gpr[29], c->gpr[30], c->gpr[31],
	            (unsigned)interp_addr);
	report_save();
	return FALSE; /* let Cafe OS handle it as before */
}

static BOOL on_dsi(OSContext *c)     { return crash_report("DSI (bad data access)", c); }
static BOOL on_isi(OSContext *c)     { return crash_report("ISI (bad code address)", c); }
static BOOL on_program(OSContext *c) { return crash_report("program exception (illegal or trap instruction)", c); }
static BOOL on_align(OSContext *c)   { return crash_report("alignment exception", c); }
static BOOL on_fp(OSContext *c)      { return crash_report("floating-point exception", c); }

static void crash_handlers_install(void)
{
	OSSetExceptionCallbackEx(OS_EXCEPTION_MODE_GLOBAL_ALL_CORES, OS_EXCEPTION_TYPE_DSI, on_dsi);
	OSSetExceptionCallbackEx(OS_EXCEPTION_MODE_GLOBAL_ALL_CORES, OS_EXCEPTION_TYPE_ISI, on_isi);
	OSSetExceptionCallbackEx(OS_EXCEPTION_MODE_GLOBAL_ALL_CORES, OS_EXCEPTION_TYPE_PROGRAM, on_program);
	OSSetExceptionCallbackEx(OS_EXCEPTION_MODE_GLOBAL_ALL_CORES, OS_EXCEPTION_TYPE_ALIGNMENT, on_align);
	OSSetExceptionCallbackEx(OS_EXCEPTION_MODE_GLOBAL_ALL_CORES, OS_EXCEPTION_TYPE_FLOATING_POINT, on_fp);
}

/* ---- watchdog: reports progress from another core, ends runs that stop producing VIs ---- */

static volatile int g_watch_on;
/* tlb-count and count-run go on after a hang by starting the app again: the
 * ROM named in the -current file is skipped from then on. Set by the watchdog
 * when a run stays stuck 40 s after it was told to end (generated code looping
 * where stop is never read: Army Men - Sarge's Heroes 2), and by halt() (an
 * assert or abort); the main thread relaunches. */
static volatile int g_relaunch;
static int g_tlb_count;
static OSThread g_watch_thread __attribute__((aligned(16)));
static uint8_t g_watch_stack[64 * 1024] __attribute__((aligned(16)));

static int watchdog(int argc, const char **argv)
{
	(void)argc; (void)argv;
	uint32_t last_vis = 0;
	int stalled = 0;
	int tick = 0;
	while (g_watch_on) {
		/* first report after 2 s, then every 10 s */
		OSSleepTicks(tick++ == 0 ? OSSecondsToTicks(2) : OSSecondsToTicks(10));
		if (!g_watch_on) break;
		uint32_t v = g_bench.vis;
		extern unsigned our64_heap_allocs, our64_heap_failures;
		report_line("    [watchdog] %u VIs, PC 0x%08X, last_addr 0x%08X, Count 0x%08X, next int 0x%08X, "
		            "dispatches %u, compiles %u, interp %u, code allocs %u (failed %u)",
		            v, (unsigned)interp_addr, (unsigned)last_addr, (unsigned)reg_cop0[9], (unsigned)next_interupt,
		            g_bench.section_calls[TRAMP_SECTION], g_bench.section_calls[COMPILER_SECTION],
		            g_bench.section_calls[INTERP_SECTION], our64_heap_allocs, our64_heap_failures);
		stalled = v == last_vis ? stalled + 1 : 0;
		last_vis = v;
		report_save(); /* a later hard crash still leaves this on the SD card */
		if (stalled >= 2 && !stop) {
			report_line("    [watchdog] no VI for 20 s: ending this run");
			stop = 1;
		}
		if (stalled >= 6 && g_tlb_count && !g_relaunch) {
			report_line("    [watchdog] still stuck 40 s later: starting the bench again (this ROM is skipped)");
			report_save();
			g_relaunch = 1;
		}
	}
	return 0;
}

static void watchdog_start(void)
{
	g_watch_on = 1;
	if (OSCreateThread(&g_watch_thread, watchdog, 0, NULL, g_watch_stack + sizeof(g_watch_stack),
	                   sizeof(g_watch_stack), 20, OS_THREAD_ATTRIB_AFFINITY_CPU2))
		OSResumeThread(&g_watch_thread);
}

/* tlb-count and count-run: the watchdog also covers loading the ROM (the
 * count bench hung there, before Bomberman 64, with no watchdog running). */
static int g_watch_held;

static void watchdog_stop(void)
{
	g_watch_on = 0;
	int ret;
	OSJoinThread(&g_watch_thread, &ret);
}

/* ---- one run ---- */

static int g_loaded;

static int load_rom(const char *path, unsigned size)
{
	static fileBrowser_file f;
	memset(&f, 0, sizeof(f));
	snprintf(f.name, sizeof(f.name), "%s", path);
	f.size = size;

	if (g_loaded) {
		cpu_deinit();
		ROMCache_deinit();
		free_memory();
	}
	g_loaded = 1;
	format_mempacks();
	tlb_mem2_init();
	if (rom_read(&f))
		return -1;
	init_memory();
	bench_plugins_init();
	cpu_init();
	return 0;
}

static uint32_t g_capture_from_vi, g_capture_tasks; /* for the next run only */
static int g_capture_only; /* sd:/wiiu/our64/capture-only exists: skip speed and compare */
static char g_capture_roms[8192]; /* ROM files named inside capture-only, one per line; empty: every ROM (tlb-count: the ROMs to skip) */

/* Whether capture-only names this ROM file. */
static int capture_wanted(const char *file)
{
	if (!g_capture_roms[0]) return 1;
	size_t n = strlen(file);
	for (const char *p = g_capture_roms; (p = strstr(p, file)); p += n)
		if ((p == g_capture_roms || p[-1] == '\n' || p[-1] == ' ') &&
		    (p[n] == 0 || p[n] == '\n' || p[n] == '\r' || p[n] == ' '))
			return 1;
	return 0;
}

static void run(int core_mode, double seconds, uint32_t vis, int hash_every)
{
	memset(&g_bench, 0, sizeof(g_bench));
	g_bench.capture_from_vi = g_capture_from_vi;
	g_bench.capture_left = g_capture_tasks;
	g_capture_from_vi = g_capture_tasks = 0;
	double now = report_now_us();
	g_bench.deadline_us = seconds > 0 ? now + seconds * 1e6 : 0;
	g_bench.stop_after_vis = vis;
	g_bench.hash_every = hash_every;
	g_bench.window_from_us = now + SPEED_WINDOW_S * 1e6;
	dynacore = core_mode;
	stop = 0;
	g_section_start[0] = now;
	if (!g_watch_held) watchdog_start();
	go();
	if (!g_watch_held) watchdog_stop();
	g_bench.section_us[0] = report_now_us() - now;
}

/* Runs until CAPTURE_TASKS graphics tasks after the given VI are saved. */
static void run_capture(uint32_t from_vi, uint32_t tasks)
{
	g_capture_from_vi = from_vi;
	g_capture_tasks = tasks;
	run(1, 600, 0, 0);
}

static int is_pal(void)
{
	switch (ROM_HEADER.Country_code) {
	case 'D': case 'F': case 'I': case 'P': case 'S': case 'U': case 'X': case 'Y':
		return 1;
	}
	return 0;
}

static void capture_rom(const char *path, unsigned size);

/* One speed run, reported under a label (the bench-jr comparison). */
static void speed_run(int hz, const char *label)
{
	unsigned clears0 = jr_cache_clears;
	run(1, SPEED_SECONDS, 0, 0);
	bench_counters s = g_bench;
	double total = s.section_us[0];
	double win_s = (report_now_us() - s.window_start_us) / 1e6;
	double vis_s = s.window_start_us ? (s.vis - s.window_start_vis) / win_s : 0;
	double comp = s.section_us[COMPILER_SECTION], tramp = s.section_us[TRAMP_SECTION];
	report_line("   %s: %.1f VI/s = %.0f%% of full speed; dispatches %.0f per VI; block dispatch %.1f%%; "
	            "%u cache clears", label, vis_s, 100.0 * vis_s / hz, s.vis ? (double)s.section_calls[TRAMP_SECTION] / s.vis : 0,
	            100 * (tramp - comp) / total, jr_cache_clears - clears0);
	report_save();
}

static void bench_rom(const char *path, unsigned size)
{
	report_line(" ");
	report_line("== %s (%.1f MB)", strrchr(path, '/') + 1, size / 1048576.0);
	input_load(report_sd_root(), path, CAPTURE_AT_VI); /* scripted input applies to every run */
	if (load_rom(path, size)) {
		report_line("   could not load (not an N64 ROM, or unreadable)");
		return;
	}
	int hz = is_pal() ? 50 : 60;
	report_line("   header name \"%s\", region %c (%d Hz)", ROM_SETTINGS.goodname, ROM_HEADER.Country_code, hz);
	report_save();

	/* sd:/wiiu/our64/bench-jr: speed only, with the register-jump cache off, then on */
	if (g_jr_ab) {
		for (int on = 0; on < 2; on++) {
			jr_cache_on = on;
			if (on) load_rom(path, size);
			speed_run(hz, on ? "register-jump cache on" : "register-jump cache off");
		}
		jr_cache_on = 1;
		return;
	}

	/* 1. speed */
	unsigned sw0 = our64_jit_switches();
#ifdef OUR64_SPEED_RUN_CORE
	run(OUR64_SPEED_RUN_CORE, SPEED_SECONDS, 0, 0); /* diagnostic builds: 2 = pure interpreter */
#else
	run(1, SPEED_SECONDS, 0, 0);
#endif
	bench_counters s = g_bench;
	double total = s.section_us[0];
	double win_s = (report_now_us() - s.window_start_us) / 1e6;
	double vis_s = s.window_start_us ? (s.vis - s.window_start_vis) / win_s : 0;
	double fps = s.window_start_us ? (s.gfx_tasks - s.window_start_gfx) / win_s : 0;
	report_line("   speed: %.1f VI/s = %.0f%% of full speed; game frames %.1f/s (last %.0f s)",
	            vis_s, 100.0 * vis_s / hz, fps, win_s);
	report_line("   whole run: %u VIs, %u frames, %u audio tasks in %.1f s", s.vis, s.gfx_tasks, s.audio_tasks,
	            total / 1e6);
	double comp = s.section_us[COMPILER_SECTION], tramp = s.section_us[TRAMP_SECTION];
	double audio = s.section_us[AUDIO_SECTION], gfx = s.section_us[GFX_SECTION];
	double interp = s.section_us[INTERP_SECTION];
	report_line("   time: compiling %.1f%%, block dispatch %.1f%%, audio HLE %.1f%%, interpreter fallback %.1f%%, "
	            "graphics stub %.1f%%, rest (generated code + memory) %.1f%%",
	            100 * comp / total, 100 * (tramp - comp) / total, 100 * audio / total, 100 * interp / total,
	            100 * gfx / total, 100 * (total - tramp - audio - interp - gfx) / total);
	report_line("   JIT: %u write windows, %u call stubs", our64_jit_switches() - sw0, our64_jit_stub_count());
	report_save();

	/* 2. correctness: recompiler vs pure interpreter over the same VIs */
	load_rom(path, size);
	run(1, 120, COMPARE_VIS, COMPARE_EVERY);
	bench_counters d = g_bench;
	load_rom(path, size);
	run(2, INTERP_LIMIT_S, COMPARE_VIS, COMPARE_EVERY);
	bench_counters i = g_bench;
	double ivis_s = i.vis / (i.section_us[0] / 1e6);
	int n = d.nhashes < i.nhashes ? d.nhashes : i.nhashes, first_diff = -1;
	for (int k = 0; k < n; k++)
		if (d.hashes[k] != i.hashes[k]) { first_diff = k; break; }
	report_line("   pure interpreter: %.1f VI/s; the recompiler runs at %.1fx that", ivis_s, ivis_s > 0 ? vis_s / ivis_s : 0);
	if (n == 0)
		report_line("   compare: no checkpoints reached");
	else if (first_diff < 0)
		report_line("   compare: RDRAM identical at all %d checkpoints (every %d VIs)", n, COMPARE_EVERY);
	else
		report_line("   compare: RDRAM first differs at VI %d (%d of %d checkpoints matched)",
		            (first_diff + 1) * COMPARE_EVERY, first_diff, n);
	report_save();

	capture_rom(path, size);
}

/* sd:/wiiu/our64/tlb-count: each ROM runs TLB_COUNT_S seconds, counting the
 * loads and stores that leave generated code for dyna_mem (Not64 built with
 * OUR64_COUNT_MEM): at TLB addresses (below 0x80000000 or from 0xC0000000),
 * and the rest (uncached memory, I/O registers). Pages of code compiled at TLB
 * addresses are counted at the end. Which games a TLB fast path in generated
 * code would touch (Battle for Naboo, 2026-10-26). The ROM files named inside
 * tlb-count, one per line, are skipped. Each finished ROM's line is added to
 * tlb-results.txt and its file name to tlb-count, and the ROM running is named
 * in tlb-current: after a hang the watchdog cannot end (Army Men - Sarge's
 * Heroes 2), the next start skips it and goes on from there. */
#define TLB_COUNT_S 8
#ifdef OUR64_COUNT_INTERP
/* The count build (Our64 Count Bench, Not64 built with OUR64_COUNT_INTERP) keeps
 * its own switch, progress and results, so the two benches never skip each
 * other's ROMs: count-run, count-current, count-results.txt. It adds what
 * generated code sends to the interpreter, per VI: MTC0 Status (libultra's
 * __osDisableInt/__osRestoreInt), MTC0 Count/Compare, and the top opcodes. */
#define TLB_SWITCH  "count-run"
#define TLB_CURRENT "count-current"
#define TLB_RESULTS "count-results.txt"
unsigned int our64_interp_ops[64], our64_interp_status, our64_interp_count_compare;
#else
#define TLB_SWITCH  "tlb-count"
#define TLB_CURRENT "tlb-current"
#define TLB_RESULTS "tlb-results.txt"
#endif
unsigned int our64_mem_tlb, our64_mem_other;

static void tlb_note(const char *file, const char *mode, const char *line)
{
	char p[256];
	snprintf(p, sizeof(p), "%s/%s", report_sd_root(), file);
	FILE *f = fopen(p, mode);
	if (f) {
		fprintf(f, "%s\n", line);
		fclose(f);
	}
}

static void tlb_count_rom(const char *path, unsigned size)
{
	const char *name = strrchr(path, '/') + 1;
	char line[512];
	tlb_note(TLB_CURRENT, "w", name);
	watchdog_start();
	g_watch_held = 1;
	const int failed = load_rom(path, size);
	const unsigned clears0 = jr_cache_clears;
	if (!failed) {
		our64_mem_tlb = our64_mem_other = 0;
#ifdef OUR64_COUNT_INTERP
		memset(our64_interp_ops, 0, sizeof(our64_interp_ops));
		our64_interp_status = our64_interp_count_compare = 0;
#endif
		run(1, TLB_COUNT_S, 0, 0);
	}
	g_watch_held = 0;
	watchdog_stop();
	if (failed) {
		snprintf(line, sizeof(line), "TLB: %s: could not load", name);
		report_line("%s", line);
		tlb_note(TLB_RESULTS, "a", line);
		tlb_note(TLB_SWITCH, "a", name);
		return;
	}
	const double s = g_bench.section_us[0] / 1e6;
	unsigned code_pages = 0;
	for (unsigned i = 0; i < 0x100000; i++)
		if ((i < 0x80000 || i >= 0xC0000) && blocks[i] && blocks[i]->funcs)
			code_pages++;
	snprintf(line, sizeof(line), "TLB: %s: %u VIs in %.0f s; TLB loads and stores %.0f/s; other slow ones %.0f/s; "
	         "TLB code pages %u; register-jump table emptied %.0f/s", name, g_bench.vis, s, our64_mem_tlb / s,
	         our64_mem_other / s, code_pages, (jr_cache_clears - clears0) / s);
#ifdef OUR64_COUNT_INTERP
	{
		const double vis = g_bench.vis ? g_bench.vis : 1;
		unsigned total = 0, top[3] = {0, 0, 0}, topn[3] = {0, 0, 0};
		for (unsigned op = 0; op < 64; op++) {
			const unsigned c = our64_interp_ops[op];
			total += c;
			for (int k = 0; k < 3; k++)
				if (c > topn[k]) {
					for (int j = 2; j > k; j--) { top[j] = top[j - 1]; topn[j] = topn[j - 1]; }
					top[k] = op; topn[k] = c;
					break;
				}
		}
		const size_t len = strlen(line);
		snprintf(line + len, sizeof(line) - len, "; per VI: slow memory %.0f, interpreted %.0f (MTC0 Status %.0f, "
		         "Count/Compare %.0f; opcodes 0x%02X %.0f, 0x%02X %.0f, 0x%02X %.0f)",
		         (our64_mem_tlb + our64_mem_other) / vis, total / vis, our64_interp_status / vis,
		         our64_interp_count_compare / vis, top[0], topn[0] / vis, top[1], topn[1] / vis, top[2], topn[2] / vis);
	}
#endif
	report_line("%s", line);
	tlb_note(TLB_RESULTS, "a", line);
	tlb_note(TLB_SWITCH, "a", name);
	report_save();
}

/* Recompiler vs interpreter over `vis` VIs, checked every `every` VIs. */
static void diverge_rom(const char *path, unsigned size, uint32_t vis, uint32_t every)
{
	report_line(" ");
	report_line("== %s: looking for the first difference, recompiler vs interpreter, %u VIs, every %u",
	            strrchr(path, '/') + 1, (unsigned)vis, (unsigned)every);
	input_load(report_sd_root(), path, CAPTURE_AT_VI);
	g_div_every = every;
	g_div_vis = vis;
	g_div_table = malloc((size_t)(vis / every) * DIV_SLOTS * 4);
	g_div_saved = malloc(DIV_MAX_PAGES * 1024 * 4);
	if (!g_div_table || !g_div_saved) {
		report_line("   not enough memory");
		return;
	}
	if (load_rom(path, size)) {
		report_line("   could not load");
		return;
	}
	g_div_mode = 1;
	run(1, 900, vis, 0);
	report_line("   recompiler: %u VIs in %.0f s", g_bench.vis, g_bench.section_us[0] / 1e6);
	report_save();

	load_rom(path, size);
	g_div_mode = 2;
	run(2, 3600, vis, 0);
	report_line("   interpreter: %u VIs in %.0f s", g_bench.vis, g_bench.section_us[0] / 1e6);
	if (g_div_first < 0) {
		report_line("   no difference in %u VIs", (unsigned)vis);
		g_div_mode = 0;
		return;
	}
	report_line("   first difference at VI %u: %d slot(s) differ (4 KB RDRAM pages, GPRs, FPRs)",
	            (unsigned)((g_div_first + 1) * every), g_div_nslots);
	for (int j = 0; j < g_div_nslots; j++)
		report_line("   slot %u: %s", g_div_slot[j],
		            g_div_slot[j] < DIV_PAGES ? "RDRAM page" : g_div_slot[j] == DIV_PAGES ? "GPRs" : "FPRs");
	report_save();

	load_rom(path, size);
	g_div_mode = 3;
	run(1, 900, (g_div_first + 1) * every, 0);
	g_div_mode = 0;
	report_save();
}

/* Frames for the stage-2 replay benchmark, at each capture point of the
 * ROM's input script. */
static void capture_rom(const char *path, unsigned size)
{
	const int points = input_load(report_sd_root(), path, CAPTURE_AT_VI);
	load_rom(path, size);
	capture_begin(report_sd_root(), strrchr(path, '/') + 1);
	capture_group(input_capture_vi(0));
	run_capture(input_capture_vi(0), CAPTURE_TASKS);
	report_line("   capture: %u graphics tasks at %d point(s) up to VI %u saved to sd:/wiiu/our64/frames (%u failed)",
	            g_bench.captured, points, input_capture_vi(points - 1), g_bench.capture_errors);
	report_save();
}

/* ---- ROM list ---- */

typedef struct { char path[256]; unsigned size; } rom_entry;

static int cmp_rom(const void *a, const void *b)
{
	return strcasecmp(((const rom_entry *)a)->path, ((const rom_entry *)b)->path);
}

static int list_roms(const char *dir, rom_entry *out, int max)
{
	DIR *d = opendir(dir);
	if (!d) return 0;
	int n = 0;
	struct dirent *e;
	while ((e = readdir(d)) && n < max) {
		const char *dot = strrchr(e->d_name, '.');
		if (!dot || (strcasecmp(dot, ".z64") && strcasecmp(dot, ".n64") && strcasecmp(dot, ".v64")))
			continue;
		snprintf(out[n].path, sizeof(out[n].path), "%s/%s", dir, e->d_name);
		struct stat st;
		if (stat(out[n].path, &st) == 0 && st.st_size > 0x1000) {
			out[n].size = (unsigned)st.st_size;
			n++;
		}
	}
	closedir(d);
	qsort(out, n, sizeof(out[0]), cmp_rom);
	return n;
}

/* ---- emulation thread on the codegen core ---- */

static rom_entry g_roms[256];
static int g_nroms;
static char g_diverge[128];        /* diverge.txt: the one ROM to search, else empty */
static unsigned g_diverge_vis = 3600, g_diverge_every = 5;

static int emu_thread(int argc, const char **argv)
{
	(void)argc; (void)argv;
	/* Generated code reads core state through r13; nothing else in this app uses it. */
	__asm__ volatile("mr 13,%0" : : "r"(our64_sda_base));
	unsigned got, want;
	int st = our64_jit_selftest(&got, &want);
	report_line("JIT self-test (generated code calls C through a stub): %s (got 0x%08X, want 0x%08X)",
	            st ? "FAILED" : "OK", got, want);
	if (st)
		return 0;
	for (int k = 0; k < g_nroms; k++) {
		if (strlen(g_roms[k].path) >= FILE_BROWSER_MAX_PATH_LEN) {
			report_line(" ");
			report_line("== %s: path too long (max %d characters), skipped", g_roms[k].path, FILE_BROWSER_MAX_PATH_LEN - 1);
			continue;
		}
		if (g_jr_ab && !capture_wanted(strrchr(g_roms[k].path, '/') + 1))
			continue;
		if (g_tlb_count) {
			if (!g_capture_roms[0] || !capture_wanted(strrchr(g_roms[k].path, '/') + 1))
				tlb_count_rom(g_roms[k].path, g_roms[k].size);
		} else if (g_diverge[0]) {
			if (!strcasecmp(strrchr(g_roms[k].path, '/') + 1, g_diverge))
				diverge_rom(g_roms[k].path, g_roms[k].size, g_diverge_vis, g_diverge_every);
		} else if (g_capture_only) {
			if (!capture_wanted(strrchr(g_roms[k].path, '/') + 1))
				continue;
			report_line(" ");
			report_line("== %s", strrchr(g_roms[k].path, '/') + 1);
			capture_rom(g_roms[k].path, g_roms[k].size);
		} else {
			bench_rom(g_roms[k].path, g_roms[k].size);
		}
	}
	return 0;
}

static OSThread g_thread __attribute__((aligned(16)));
static uint8_t g_stack[1024 * 1024] __attribute__((aligned(16)));

int main(int argc, char **argv)
{
	(void)argc; (void)argv;
	WHBProcInit();
	WHBLogCafeInit();
	WHBLogUdpInit();
	WHBLogConsoleInit();

#ifdef OUR64_CEMU
	report_line("Our64 stage-1 CPU benchmark (" __DATE__ " " __TIME__ "), CEMU BUILD: no rounding modes, not for consoles");
#else
	report_line("Our64 stage-1 CPU benchmark (" __DATE__ " " __TIME__ ")");
#endif
	const char *root = report_sd_root();
	crash_handlers_install();
	if (our64_jit_init()) {
		report_line("No JIT area: launch from the Wii U Menu under Aroma.");
	} else if (sda_setup()) {
		report_line("Core variables too far apart for r13 addressing: build problem.");
	} else if (!root) {
		report_line("No SD card.");
	} else {
		char dir[256];
		snprintf(dir, sizeof(dir), "%s/roms", root);
		mkdir(dir, 0777);
		g_nroms = list_roms(dir, g_roms, 256);
		struct stat st;
		snprintf(dir, sizeof(dir), "%s/capture-only", root);
		g_capture_only = stat(dir, &st) == 0;
		if (g_capture_only) {
			FILE *cf = fopen(dir, "r");
			if (cf) {
				size_t got = fread(g_capture_roms, 1, sizeof(g_capture_roms) - 1, cf);
				g_capture_roms[got] = 0;
				fclose(cf);
			}
			report_line("capture-only: sd:/wiiu/our64/capture-only exists, speed and compare skipped%s",
			            g_capture_roms[0] ? "; only the ROMs it names" : "");
		}
		snprintf(dir, sizeof(dir), "%s/bench-jr", root);
		g_jr_ab = stat(dir, &st) == 0;
		if (g_jr_ab) {
			/* like capture-only: the ROM files it names, one per line; empty: every ROM */
			FILE *jf = fopen(dir, "r");
			if (jf) {
				size_t got = fread(g_capture_roms, 1, sizeof(g_capture_roms) - 1, jf);
				g_capture_roms[got] = 0;
				fclose(jf);
			}
			report_line("bench-jr: speed only, register-jump cache off then on%s",
			            g_capture_roms[0] ? "; only the ROMs it names" : "");
		}
		snprintf(dir, sizeof(dir), "%s/" TLB_SWITCH, root);
		g_tlb_count = stat(dir, &st) == 0;
		if (g_tlb_count) {
			FILE *tf = fopen(dir, "r");
			if (tf) {
				size_t got = fread(g_capture_roms, 1, sizeof(g_capture_roms) - 1, tf);
				g_capture_roms[got] = 0;
				fclose(tf);
			}
			/* the ROM that was running when the last start stopped: it hung */
			char cur[256], hung[200];
			snprintf(cur, sizeof(cur), "%s/" TLB_CURRENT, root);
			if ((tf = fopen(cur, "r"))) {
				if (fgets(hung, sizeof(hung), tf)) {
					hung[strcspn(hung, "\r\n")] = 0;
					char line[300];
					snprintf(line, sizeof(line), "TLB: %s: hung (skipped from now on)", hung);
					report_line("%s", line);
					tlb_note(TLB_RESULTS, "a", line);
					tlb_note(TLB_SWITCH, "a", hung);
					size_t len = strlen(g_capture_roms);
					snprintf(g_capture_roms + len, sizeof(g_capture_roms) - len, "\n%s\n", hung);
				}
				fclose(tf);
			}
		}
		if (g_tlb_count)
			report_line(TLB_SWITCH ": every ROM %d s, counting loads and stores at TLB addresses", TLB_COUNT_S);
		snprintf(dir, sizeof(dir), "%s/diverge.txt", root);
		FILE *df = fopen(dir, "r");
		if (df) {
			if (fscanf(df, "%127s %u %u", g_diverge, &g_diverge_vis, &g_diverge_every) < 1) g_diverge[0] = 0;
			fclose(df);
			if (g_diverge_every == 0) g_diverge_every = 5;
			if (g_diverge[0]) report_line("diverge.txt: only searching %s for a recompiler difference", g_diverge);
		}
		report_line("JIT area %u KB for code; %d ROM(s) in sd:/wiiu/our64/roms", our64_jit_heap_size() / 1024, g_nroms);
		if (g_nroms == 0)
			report_line("Copy .z64, .n64 or .v64 files there and start again.");
		report_save();

		uint32_t core = OSGetCodegenCore();
		if (core > 2) core = 1;
		/* tlb-count and count-run: below this thread's priority on the same core,
		 * so a game looping in generated code cannot keep it from relaunching. */
		if (g_nroms && OSCreateThread(&g_thread, emu_thread, 0, NULL, g_stack + sizeof(g_stack), sizeof(g_stack),
		                              g_tlb_count ? 17 : 16,
		                              (OSThreadAttributes)(1u << core))) {
			OSResumeThread(&g_thread);
			while (!OSIsThreadTerminated(&g_thread) && !g_relaunch)
				OSSleepTicks(OSMillisecondsToTicks(100));
			if (g_relaunch) {
				SYSRelaunchTitle(0, NULL);
				while (WHBProcIsRunning())
					OSSleepTicks(OSMillisecondsToTicks(100));
				WHBProcShutdown();
				return 0;
			}
			int ret;
			OSJoinThread(&g_thread, &ret);
		}
	}

	if (g_tlb_count) {
		char cur[256];
		snprintf(cur, sizeof(cur), "%s/" TLB_CURRENT, root);
		remove(cur);
	}
	report_line(" ");
	report_line("Done. Press HOME to exit.");
	if (!report_save())
		report_line("Results were not saved to SD.");
	while (WHBProcIsRunning()) {
		WHBLogConsoleDraw();
		OSSleepTicks(OSMillisecondsToTicks(100));
	}

	WHBLogConsoleFree();
	WHBLogUdpDeinit();
	WHBLogCafeDeinit();
	WHBProcShutdown();
	return 0;
}

/* ---- failure reporting: Cemu does not implement OSFatal, so report and stop here ---- */

static void __attribute__((noreturn)) halt(void)
{
	report_save();
	if (g_tlb_count)
		g_relaunch = 1;
	for (;;)
		OSSleepTicks(OSSecondsToTicks(1));
}

void abort(void)
{
	report_line("FAILED: abort() called from %p, N64 PC 0x%08X", __builtin_return_address(0), (unsigned)interp_addr);
	halt();
}

void __assert_func(const char *file, int line, const char *func, const char *expr)
{
	report_line("FAILED: assert \"%s\" in %s (%s:%d), N64 PC 0x%08X", expr, func, file, line, (unsigned)interp_addr);
	halt();
}

void our64_bad_mem_call(unsigned addr, unsigned value, int count, int type, unsigned pc, int delay)
{
	report_line("FAILED: memory helper got addr 0x%08X reg %u count %d type %d pc 0x%08X delay %d",
	            addr, value, count, type, pc, delay);
}
