/* PC tests for gb_tower_hle.c: Pokemon Stadium 2's three GB Tower RSP
 * programs, from the ROM, run on cxd4 (rsp_lle.c) and in C on the same
 * random input; all 8 MB of RDRAM and the 4 KB of DMEM must come out the same
 * byte for byte. Skipped without the ROM (OUR64_PS2_ROM, or the default path).
 *
 * Each task's input is random: the 0x6000 bytes of video memory, the line
 * registers and sprites (all random, or lines that repeat and sprites on the
 * screen), the palettes, the lines' 160 bytes, the border stream and
 * ucode_data's rows (half their pixels 0), DMEM before the task; brightness
 * and scale go through all 256 values, 0xFDD's correction too; the rows'
 * bytes, offsets, counts, data_size's top byte and rows are random or values
 * at the program's edges. An eighth of the tasks put the buffers anywhere in
 * RDRAM (overlapping, unaligned, past 8 MB); one whose first copy lands on
 * the program's tables the HLE must leave to cxd4. Then the tasks it must
 * refuse, and its time against cxd4's on a game's task.
 *
 *   test_gb_tower [tasks a variant (1000)] [seed] */
#include "gb_tower_hle.h"
#include "rsp_lle.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int g_fail;
#define CHECK(c) do { if (!(c)) { printf("  FAIL line %d: %s\n", __LINE__, #c); g_fail = 1; } } while (0)

#define RDRAM_SIZE 0x800000

#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
#define BYTE_X 3u
#else
#define BYTE_X 0u
#endif

static uint32_t *g_ram, *g_hle, *g_init; /* RDRAM for cxd4, for the HLE, and before either */
static uint32_t g_spmem[0x2000 / 4], g_dmem_init[0x1000 / 4], g_dmem_hle[0x1000 / 4];
static uint32_t g_mi_intr, g_sp[8], g_sp_pc, g_dpc[8];
static rsp_lle_regs g_regs;

static void check_interrupts(void) {}

static void put8(void *mem, uint32_t a, uint8_t v) { ((uint8_t *)mem)[a ^ BYTE_X] = v; }
static uint8_t get8(const void *mem, uint32_t a) { return ((const uint8_t *)mem)[a ^ BYTE_X]; }
static void put32(void *mem, uint32_t a, uint32_t v) { ((uint32_t *)mem)[a / 4] = v; }
static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }

static uint32_t g_seed = 0x2468ACE1u;
static uint32_t rnd(void)
{
	g_seed ^= g_seed << 13;
	g_seed ^= g_seed >> 17;
	g_seed ^= g_seed << 5;
	return g_seed;
}
static uint32_t below(uint32_t n) { return (uint32_t)(((uint64_t)rnd() * n) >> 32); }

/* Random bytes over [a, a + n) of RDRAM, clipped to it. */
static void fill(uint32_t a, uint32_t n)
{
	for (uint32_t i = 0; i < n && a + i < RDRAM_SIZE; i++)
		put8(g_init, a + i, (uint8_t)rnd());
}

static uint8_t *read_file(const char *path, long *size)
{
	FILE *f = fopen(path, "rb");
	if (!f)
		return NULL;
	fseek(f, 0, SEEK_END);
	*size = ftell(f);
	fseek(f, 0, SEEK_SET);
	uint8_t *d = malloc(*size);
	if (d && fread(d, 1, *size, f) != (size_t)*size) {
		free(d);
		d = NULL;
	}
	fclose(f);
	return d;
}

static double now_us(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec * 1e6 + t.tv_nsec / 1e3;
}

typedef struct {
	uint32_t boot, src, vram, data, out, border, udata;
	uint32_t row_units, offset, top, rows, bright, scale, cc, over, skip;
} task;

/* The line registers and sprites at data_ptr: random, or lines that repeat
 * (LCDC on, scroll and window steady, palettes that change now and then) and
 * sprites on the screen. */
static void fill_lines(uint32_t data, int realistic)
{
	fill(data, 0x6A00);
	if (!realistic)
		return;
	const uint32_t lcdc = 0x80 | (rnd() & 0x7F), scy = rnd() & 0xFF, scx = rnd() & 0xFF, wy = below(160);
	uint32_t wx = below(176), bgp = rnd() & 0xFF, obp = rnd() & 0xFF;
	for (uint32_t y = 0; y < 144; y++) {
		const uint32_t r = data + y * 8;
		put8(g_init, r + 0, (uint8_t)(below(16) ? lcdc : rnd()));
		put8(g_init, r + 1, (uint8_t)(scy + y));
		put8(g_init, r + 2, (uint8_t)(y >= wy ? y - wy : 0));
		if (!below(32))
			wx = below(176);
		put8(g_init, r + 3, (uint8_t)wx);
		if (!below(24))
			bgp = rnd() & 0xFF;
		if (!below(24))
			obp = rnd() & 0xFF;
		put8(g_init, r + 4, (uint8_t)bgp);
		put8(g_init, r + 5, (uint8_t)obp);
		put8(g_init, r + 6, (uint8_t)(below(8) ? scx : rnd()));
		put8(g_init, r + 7, (uint8_t)bgp);
	}
	for (uint32_t s = 0; s < 40; s++) {
		const uint32_t o = data + 0x480 + s * 4;
		put8(g_init, o + 0, (uint8_t)below(176));
		put8(g_init, o + 1, (uint8_t)below(176));
	}
}

/* The task in DMEM as osSpTaskLoad leaves it, the program in IMEM. */
static void load_task(const task *t, uint32_t size)
{
	uint32_t *dmem = g_dmem_init;
	for (uint32_t i = 0; i < 0x1000 / 4; i++)
		dmem[i] = rnd();
	put32(dmem, 0xFC0, 2);
	put32(dmem, 0xFC4, rnd());
	put32(dmem, 0xFC8, t->boot);
	put32(dmem, 0xFCC, size);
	put32(dmem, 0xFD8, t->udata);
	put32(dmem, 0xFDC, (rnd() & 0xFF) << 24 | t->cc << 16 | t->over << 8 | t->skip);
	put32(dmem, 0xFE0, t->src);
	put32(dmem, 0xFE4, rnd());
	put32(dmem, 0xFE8, t->out);
	put32(dmem, 0xFEC, t->vram);
	put32(dmem, 0xFF0, t->data);
	put32(dmem, 0xFF4, t->top << 24 | ((t->rows * t->row_units * 64) & 0xFFFFFF));
	put32(dmem, 0xFF8, t->border);
	put32(dmem, 0xFFC, t->bright << 24 | t->scale << 16 | t->row_units << 11 | t->offset);
}

static void run_cxd4(uint32_t size, int *result, unsigned long *n)
{
	memcpy(g_spmem, g_dmem_init, 0x1000);
	const uint32_t boot = g_dmem_init[0xFC8 / 4] & 0x7FFFF8;
	memcpy(g_spmem + 0x1000 / 4, g_ram + boot / 4, size);
	g_sp_pc = 0;
	g_sp[4] = 0x40;
	g_mi_intr = 0;
	*n = rsp_lle_run(0x400, 50000000, 0, result);
}

/* Anywhere in RDRAM, a quarter of the time in its last 64 KB (DMAs past 8 MB). */
static uint32_t anyplace(void) { return below(4) ? below(RDRAM_SIZE) : RDRAM_SIZE - 1 - below(0x10000); }

#define PICK(...) pick((const uint32_t[]){__VA_ARGS__}, sizeof((const uint32_t[]){__VA_ARGS__}) / 4)
static uint32_t pick(const uint32_t *v, uint32_t n) { return v[below(n)]; }

/* A random task for the program of this size; i walks brightness, scale and
 * 0xFDD. A game's: rows of 320 pixels, the Game Boy's in the middle. Else,
 * often, values at the program's edges: a row's offset where its pixels
 * meet the colors it reads (DMEM 0x200, 0x700-0x9FF) or are not even, the
 * smallest and largest rows, data_size's top byte at 40, the end of the
 * picture before, at and after the Game Boy's lines and the border's. */
static void make_task(task *t, const uint8_t *rom, uint32_t off, uint32_t size, unsigned i, int game)
{
	const int anywhere = !game && below(8) == 0, edge = !game && below(3) == 0;
	t->row_units = game || below(4) ? 10 : edge ? PICK(1, 2, 3, 30, 31) : 1 + below(31);
	t->offset = game || below(4) ? 160
	          : edge ? PICK(0, 1, 2, 3, 4, 94, 95, 96, 97, 98, 161, 162, 0x7FC, 0x7FD, 0x7FE, 0x7FF)
	                 : below(0x800);
	t->top = game || below(3) ? 40 : edge ? PICK(0, 1, 39, 41, 42, 60) : below(61);
	const uint32_t most = t->top + 144 + 40 + 20;
	t->rows = game || below(4) ? t->top + 184
	        : edge ? t->top + PICK(0, 1, 143, 144, 145, 183, 185, 204)
	               : t->top + below(most - t->top);
	t->bright = i & 0xFF;
	t->scale = i % 7 == 0 ? 0x40 : (i * 101 + (i >> 8)) & 0xFF;
	t->cc = (i * 53 + (i >> 8) * 7) & 0xFF;
	t->over = below(46);
	t->skip = below(46);
	if (anywhere) {
		t->boot = 0x100000 + 8 * below(0x1000);
		t->src = anyplace();
		t->vram = anyplace();
		t->data = anyplace();
		t->out = anyplace();
		t->border = anyplace();
		t->udata = anyplace();
	} else {
		t->boot = 0x100000 + 8 * below(0x1000);
		t->src = 0x120000 + 8 * below(0x1000);
		t->vram = 0x140000 + 8 * below(0x1000);
		t->data = 0x170000 + 8 * below(0x1000);
		t->out = 0x200000 + 8 * below(0x8000);
		t->border = 0x400000 + 8 * below(0x1000);
		t->udata = 0x480000 + 8 * below(0x1000);
	}
	/* Random input everywhere it reads, then the program and its tables. */
	fill(t->src, 0x6000);
	fill(t->vram, 0x6000 + 8 + 160 * 152);
	if (t->vram >= 1280)
		fill(t->vram + 0x6008 - 1280, 1280); /* sprites above the screen */
	fill_lines(t->data, game || below(2));
	fill(t->border, 0x200 + 0x100 * (t->top + 144 + 40));
	fill(t->udata, 46 * 0x7C0);
	for (uint32_t a = t->udata; a < t->udata + 46 * 0x7C0 && a + 1 < RDRAM_SIZE; a += 2)
		if (rnd() & 1) { /* ucode_data's pixels of 0 leave the border's */
			put8(g_init, a, 0);
			put8(g_init, a + 1, 0);
		}
	fill(t->out, t->rows * t->row_units * 64 + 64);
	load_task(t, size);
	for (uint32_t a = 0; a < size + 0xC0; a += 4)
		put32(g_init, t->boot + a, be32(rom + off + a));
}

static void test_program(const uint8_t *rom, uint32_t off, uint32_t size, unsigned tasks)
{
	printf("  boot code at ROM 0x%07X, size 0x%X\n", off, size);
	memset(g_init, 0, RDRAM_SIZE);
	unsigned differ = 0, ran = 0, refused = 0;
	unsigned long n_total = 0;
	double us_cxd4 = 0, us_hle = 0;
	for (unsigned i = 0; i < tasks; i++) {
		task t;
		make_task(&t, rom, off, size, i, 0);
		memcpy(g_ram, g_init, RDRAM_SIZE);
		memcpy(g_hle, g_init, RDRAM_SIZE);
		int result;
		unsigned long n;
		double t0 = now_us();
		run_cxd4(size, &result, &n);
		double t1 = now_us();
		memcpy(g_dmem_hle, g_dmem_init, 0x1000);
		const int ok = gb_tower_hle((unsigned char *)g_hle, (unsigned char *)g_dmem_hle);
		double t2 = now_us();
		CHECK(result == RSP_LLE_BROKE);
		/* Its first copy over its tables (buffers anywhere): left to cxd4, nothing written. */
		const uint32_t tab = (t.boot + size) & ~7u, copy = t.vram & ~7u;
		if (copy < tab + 0xC0 && tab < copy + 0x6000) {
			CHECK(ok == 0);
			CHECK(memcmp(g_hle, g_init, RDRAM_SIZE) == 0 && memcmp(g_dmem_hle, g_dmem_init, 0x1000) == 0);
			refused++;
			continue;
		}
		CHECK(ok == 1);
		us_cxd4 += t1 - t0;
		us_hle += t2 - t1;
		n_total += n;
		ran++;
		const int same = memcmp(g_ram, g_hle, RDRAM_SIZE) == 0 && memcmp(g_spmem, g_dmem_hle, 0x1000) == 0;
		if (!same && differ++ < 3) {
			uint32_t a = 0;
			while (a < RDRAM_SIZE && get8(g_ram, a) == get8(g_hle, a))
				a++;
			uint32_t d = 0;
			while (d < 0x1000 && get8(g_spmem, d) == get8(g_dmem_hle, d))
				d++;
			printf("    task %u differs: RDRAM first at 0x%06X (cxd4 %02X, HLE %02X), DMEM first at 0x%03X"
			       " (out 0x%06X vram 0x%06X src 0x%06X data 0x%06X border 0x%06X udata 0x%06X)\n",
			       i, a, a < RDRAM_SIZE ? get8(g_ram, a) : 0, a < RDRAM_SIZE ? get8(g_hle, a) : 0, d, t.out, t.vram,
			       t.src, t.data, t.border, t.udata);
		}
	}
	CHECK(differ == 0);
	printf("    %u random tasks, %u differ (%u more left to cxd4); %.0f instructions a task; cxd4 %.0f us a task, the HLE %.1f us (%.0fx)\n",
	       ran, differ, refused, (double)n_total / ran, us_cxd4 / ran, us_hle / ran, us_cxd4 / us_hle);

	/* Tasks it does not take: nothing written. */
	task t;
	make_task(&t, rom, off, size, 1, 0);
	memcpy(g_hle, g_init, RDRAM_SIZE);
	memcpy(g_dmem_hle, g_dmem_init, 0x1000);
	put32(g_dmem_hle, 0xFFC, (g_dmem_hle[0xFFC / 4] & 0xFFFF07FFu)); /* rows of 0 bytes: never ends */
	CHECK(gb_tower_hle((unsigned char *)g_hle, (unsigned char *)g_dmem_hle) == 0);
	memcpy(g_dmem_hle, g_dmem_init, 0x1000);
	put32(g_dmem_hle, 0xFF4, g_dmem_init[0xFF4 / 4] + 2); /* not a whole number of rows */
	CHECK(gb_tower_hle((unsigned char *)g_hle, (unsigned char *)g_dmem_hle) == 0);
	memcpy(g_dmem_hle, g_dmem_init, 0x1000);
	put32(g_dmem_hle, 0xFF4, 41u << 24 | 40 * t.row_units * 64); /* fewer rows than its first */
	CHECK(gb_tower_hle((unsigned char *)g_hle, (unsigned char *)g_dmem_hle) == 0);
	memcpy(g_dmem_hle, g_dmem_init, 0x1000);
	put32(g_dmem_hle, 0xFCC, size + 8);
	CHECK(gb_tower_hle((unsigned char *)g_hle, (unsigned char *)g_dmem_hle) == 0);
	put32(g_dmem_hle, 0xFCC, size);
	put8(g_hle, t.boot + size + 0x75, 0x55); /* tables other than the ROM's */
	CHECK(gb_tower_hle((unsigned char *)g_hle, (unsigned char *)g_dmem_hle) == 0);
	put8(g_hle, t.boot + size + 0x75, get8(g_init, t.boot + size + 0x75));
	CHECK(memcmp(g_hle, g_init, RDRAM_SIZE) == 0 && memcmp(g_dmem_hle, g_dmem_init, 0x1000) == 0);
}

/* A game's task, again and again (the HLE on the memory it already wrote). */
static void time_program(const uint8_t *rom, uint32_t off, uint32_t size)
{
	task t;
	make_task(&t, rom, off, size, 0x40, 1);
	memcpy(g_ram, g_init, RDRAM_SIZE);
	memcpy(g_hle, g_init, RDRAM_SIZE);
	int result;
	unsigned long n = 0;
	const int runs = 20, hle_runs = 400;
	double t0 = now_us();
	for (int k = 0; k < runs; k++)
		run_cxd4(size, &result, &n);
	double t1 = now_us();
	for (int k = 0; k < hle_runs; k++) {
		memcpy(g_dmem_hle, g_dmem_init, 0x1000);
		gb_tower_hle((unsigned char *)g_hle, (unsigned char *)g_dmem_hle);
	}
	double t2 = now_us();
	printf("    a game's task (%lu instructions): cxd4 %.0f us, the HLE %.1f us on this PC\n", n, (t1 - t0) / runs,
	       (t2 - t1) / hle_runs);
}

int main(int argc, char **argv)
{
	const char *path = getenv("OUR64_PS2_ROM");
	if (!path) {
		printf("  skipped: set OUR64_PS2_ROM to a Pokemon Stadium 2 (USA) z64\n");
		return 0;
	}
	const unsigned tasks = argc > 1 ? (unsigned)atoi(argv[1]) : 1000; /* tasks a variant, and a seed */
	if (argc > 2)
		g_seed = (uint32_t)strtoul(argv[2], NULL, 0) | 1;
	long size;
	uint8_t *rom = read_file(path, &size);
	if (!rom || size < 0x1000 || be32(rom) != 0x80371240) {
		printf("  skipped: no Pokemon Stadium 2 z64 at %s (set OUR64_PS2_ROM)\n", path);
		free(rom);
		return 0;
	}
	g_ram = malloc(RDRAM_SIZE);
	g_hle = malloc(RDRAM_SIZE);
	g_init = malloc(RDRAM_SIZE);
	g_regs = (rsp_lle_regs){(unsigned char *)g_ram, (unsigned char *)g_spmem, (unsigned char *)g_spmem + 0x1000, &g_mi_intr,
	                  {&g_sp[0], &g_sp[1], &g_sp[2], &g_sp[3], &g_sp[4], &g_sp[5], &g_sp[6], &g_sp[7]}, &g_sp_pc,
	                  {&g_dpc[0], &g_dpc[1], &g_dpc[2], &g_dpc[3], &g_dpc[4], &g_dpc[5], &g_dpc[6], &g_dpc[7]},
	                  check_interrupts};
	rsp_lle_init(&g_regs);

	int found = 0;
	for (long off = 0; off + 0x1000 <= size; off += 0x10) {
		if (be32(rom + off) != 0x401A6000 || be32(rom + off + 4) != 0xC800207C)
			continue;
		const uint32_t boot = be32(rom + off + 0x78) & 0xFFFF;
		if (boot == 0xA10 || boot == 0xB30 || boot == 0xA60) {
			test_program(rom, (uint32_t)off, boot, tasks);
			time_program(rom, (uint32_t)off, boot);
			found++;
		}
	}
	CHECK(found == 3);
	free(g_ram);
	free(g_hle);
	free(g_init);
	free(rom);
	return g_fail;
}
