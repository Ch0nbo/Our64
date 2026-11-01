/* PC tests for rsp_lle.c: Pokemon Stadium 2's GB Tower RSP programs, taken
 * from the ROM, run on the vendored cxd4 as the player runs them: a task
 * built in DMEM as osSpTaskLoad leaves it, MFC0 reading SP_STATUS's SIG3 as
 * clear. Skipped without the ROM (OUR64_PS2_ROM).
 *
 * The task fields, as Stadium 2's programs use them (read from their code;
 * the values the game passes are not known, so these are plausible ones):
 *   0xFE0 dram_stack: 0x6000 bytes copied to the address in 0xFEC first;
 *         0xFE0 then points 8 bytes past the copy, where each Game Boy
 *         line's 160 pixel bytes are read from
 *   0xFEC (output_buff_size): an address: tile data and maps, 0x1000 on
 *   0xFF0 data_ptr: 8 bytes of registers a line (LCDC, the background's map
 *         row, the window's, where the background ends, two palette
 *         numbers, SCX, BGP), the sprites (0x480), in 0xA60 4 palettes
 *         (0x520) and an attribute map (0x540); Game Boy Color palettes
 *         chosen by the palette numbers (0x5A0 + n * 0x40, 0x29A0 + n * 0x40)
 *   0xFE8 output_buff: the picture, 16 bits a pixel, row after row
 *   0xFF4 data_size: top byte 0x28 (plus blank rows first): rows taken from
 *         0xFF8 before the Game Boy's; low 24 bits the picture's bytes (the
 *         task ends when it has written exactly that many)
 *   0xFF8 yield_data_ptr: colors (0x200), then 0x100 bytes a row
 *   0xFFC: the border's brightness (0 is black, bit 7 ignored); 0xFFD the Game
 *         Boy colors' scale (0x40 leaves them as they are); 0xFFE bits 11-15 a
 *         row's bytes / 64 (0 never ends), bits 0-10 a horizontal offset
 *   0xFD8 ucode_data: rows drawn over border rows after the Game Boy's
 *         (their pixels that are not 0), 0xFDE of them after 0xFDF rows; 0 here
 *   0xFDD: in 0xB30, a color correction (bit 7 off, bits 0-6 its strength) */
#include "rsp_lle.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int g_fail;
#define CHECK(c) do { if (!(c)) { printf("  FAIL line %d: %s\n", __LINE__, #c); g_fail = 1; } } while (0)

#define RDRAM_SIZE 0x800000
#define BOOT_AT    0x100000 /* ucode_boot: the program and the 0xC0 bytes after it */
#define STACK_AT   0x200000 /* dram_stack */
#define VRAM_AT    0x300000 /* 0xFEC */
#define PIXELS_AT  (VRAM_AT + 0x6008)
#define DATA_AT    0x400000 /* data_ptr */
#define UDATA_AT   0x480000 /* ucode_data */
#define OUT_AT     0x500000 /* output_buff */
#define LINES_AT   0x600000 /* yield_data_ptr */
#define ROW_BYTES  0x140    /* 160 pixels */
#define ROWS       (0x28 + 144)

static uint32_t *g_rdram;
static uint32_t g_spmem[0x2000 / 4]; /* DMEM, then IMEM */
static uint32_t g_mi_intr, g_sp[8], g_sp_pc, g_dpc[8];
static unsigned g_checks;
static rsp_lle_regs g_regs;

static void check_interrupts(void) { g_checks++; }

/* Memory as words in host order: a byte's address within its word flips on a little-endian host. */
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
#define BYTE_X 3u
#else
#define BYTE_X 0u
#endif
static void put8(void *mem, uint32_t a, uint8_t v) { ((uint8_t *)mem)[a ^ BYTE_X] = v; }
static uint8_t get8(const void *mem, uint32_t a) { return ((const uint8_t *)mem)[a ^ BYTE_X]; }
static void put32(void *mem, uint32_t a, uint32_t v) { ((uint32_t *)mem)[a / 4] = v; }
static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }

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

/* A Game Boy picture's input, a fixed pattern that changes with the frame. */
static void fill_input(unsigned frame)
{
	uint32_t seed = 0x12345678u + frame;
	for (uint32_t a = 0; a < 0x6000; a++) {
		seed = seed * 1103515245u + 12345u;
		put8(g_rdram, STACK_AT + a, (uint8_t)(seed >> 16));
	}
	for (uint32_t a = 0; a < 0x8000; a++)
		put8(g_rdram, DATA_AT + a, (uint8_t)(a * 13 + frame));
	for (uint32_t a = 0; a < 0x200 + 0x100 * 0x28; a++)
		put8(g_rdram, LINES_AT + a, (uint8_t)(a * 7 + frame));
	/* Read after the copy: 160 bytes for each of the 144 lines, past the copied 0x6000 and 8. */
	for (uint32_t y = 0; y < 144; y++)
		for (uint32_t x = 0; x < 160; x++)
			put8(g_rdram, PIXELS_AT + y * 160 + x, (uint8_t)((x / 8 + y / 8 + frame) & 3));
}

/* The task as libultra's osSpTaskLoad leaves it: OSTask at DMEM 0xFC0, the
 * boot code at IMEM 0x1000, SP_PC 0x04001000, the RSP running. */
static void load_task(uint32_t size)
{
	uint32_t *dmem = g_spmem, *imem = g_spmem + 0x1000 / 4;
	memset(dmem + 0xFC0 / 4, 0, 0x40);
	put32(dmem, 0xFC0, 2);          /* type: M_AUDTASK */
	put32(dmem, 0xFC8, BOOT_AT);    /* ucode_boot */
	put32(dmem, 0xFCC, size);       /* ucode_boot_size */
	put32(dmem, 0xFD8, UDATA_AT);   /* ucode_data */
	put32(dmem, 0xFDC, 0);          /* ucode_data_size: rows to repeat */
	put32(dmem, 0xFE0, STACK_AT);   /* dram_stack */
	put32(dmem, 0xFE4, 0x400);
	put32(dmem, 0xFE8, OUT_AT);     /* output_buff */
	put32(dmem, 0xFEC, VRAM_AT);    /* output_buff_size */
	put32(dmem, 0xFF0, DATA_AT);    /* data_ptr */
	put32(dmem, 0xFF4, 0x28u << 24 | ROWS * ROW_BYTES);
	put32(dmem, 0xFF8, LINES_AT);   /* yield_data_ptr */
	put32(dmem, 0xFFC, 0x40400000u | (ROW_BYTES / 64) << 11);
	memcpy(imem, g_rdram + BOOT_AT / 4, size);
	g_sp_pc = 0;          /* the low 12 bits of 0x04001000, as Not64's do_SP_task leaves it */
	g_sp[4] = 0x40;       /* interrupt on break; not halted */
	g_mi_intr = 0;
}

static void test_program(const uint8_t *rom, uint32_t off, uint32_t size)
{
	printf("  boot code at ROM 0x%07X, size 0x%X\n", off, size);
	memset(g_rdram, 0, RDRAM_SIZE);
	for (uint32_t i = 0; i < size + 0xC0; i += 4)
		put32(g_rdram, BOOT_AT + i, be32(rom + off + i));

	/* Detection: the task's ucode_boot in RDRAM, or what IMEM already holds. */
	load_task(size);
	const unsigned char *rd = (const unsigned char *)g_rdram, *dm = (const unsigned char *)g_spmem;
	CHECK(rsp_lle_gb_tower(rd, dm, dm + 0x1000, NULL) == size);
	put32(g_spmem, 0xFCC, size + 4);
	int seen = 0;
	CHECK(rsp_lle_gb_tower(rd, dm, dm + 0x1000, &seen) == 0); /* an unknown boot size */
	CHECK(seen);
	put32(g_spmem, 0xFCC, size);
	put32(g_spmem, 0xFC8, BOOT_AT + 0x100);
	CHECK(rsp_lle_gb_tower(rd, dm, dm + 0x1000, NULL) == size); /* found in IMEM */
	g_spmem[0x1000 / 4] ^= 1;
	CHECK(rsp_lle_gb_tower(rd, dm, dm + 0x1000, &seen) == 0);
	CHECK(!seen);

	/* Without the SIG3 rule the program waits for the CPU to clear SIG3, and
	 * cxd4 gives up after 32767 reads of SP_STATUS: halted, not finished. */
	fill_input(0);
	load_task(size);
	int result;
	unsigned long n = rsp_lle_run(0, 50000000, 0, &result);
	CHECK(result == RSP_LLE_STOPPED);
	CHECK((g_sp[4] & 0x601) == 0x401); /* halted, SIG3 set, no SIG2 */
	printf("    without the SIG3 rule: halted after %lu instructions, not finished\n", n);

	/* With it: frames run to their BREAK. */
	unsigned long total = 0, min = ~0ul, max = 0, n_first = 0;
	double us = 0;
	const int frames = 4;
	uint32_t sum[frames];
	for (int frame = 0; frame < frames; frame++) {
		memset(g_rdram + VRAM_AT / 4, 0, RDRAM_SIZE - VRAM_AT);
		fill_input(frame & 1); /* frames 0 and 2 the same input, 1 and 3 another */
		load_task(size);
		unsigned checks = g_checks;
		double t = now_us();
		n = rsp_lle_run(0x400, 50000000, 0, &result);
		us += now_us() - t;
		CHECK(result == RSP_LLE_BROKE);
		CHECK((g_sp[4] & 0x203) == 0x203); /* SIG2 (task done), broke, halt */
		CHECK(g_sp[4] & 0x400);            /* SIG3 still set: the CPU clears it */
		CHECK(g_mi_intr & 1);              /* the SP interrupt */
		CHECK(g_checks == checks + 1);
		total += n;
		min = n < min ? n : min;
		max = n > max ? n : max;
		/* The 0x6000 bytes copied, and the picture written at output_buff. */
		int copied = 1;
		for (uint32_t a = 0; a < 0x6000; a++)
			copied &= get8(g_rdram, VRAM_AT + a) == get8(g_rdram, STACK_AT + a);
		CHECK(copied);
		uint32_t nonzero = 0, beyond = 0;
		sum[frame] = 0;
		for (uint32_t a = 0; a < ROWS * ROW_BYTES; a++) {
			nonzero += get8(g_rdram, OUT_AT + a) != 0;
			sum[frame] = sum[frame] * 31 + get8(g_rdram, OUT_AT + a);
		}
		for (uint32_t a = ROWS * ROW_BYTES; a < 0x10000; a++)
			beyond += get8(g_rdram, OUT_AT + a) != 0;
		/* Pixels other than black (0x0001), in the rows from 0xFF8 and in the Game Boy's. */
		uint32_t lit[2] = {0, 0};
		for (uint32_t row = 0; row < ROWS; row++)
			for (uint32_t x = 0; x < ROW_BYTES; x += 2) {
				const uint32_t a = OUT_AT + row * ROW_BYTES + x;
				lit[row >= 0x28] += get8(g_rdram, a) != 0 || get8(g_rdram, a + 1) > 1;
			}
		CHECK(nonzero > ROWS * ROW_BYTES / 4);
		CHECK(beyond == 0);
		CHECK(lit[0] > 0x28 * 80);
		CHECK(lit[1] > 144 * 40);
		if (frame == 0)
			n_first = n;
		if (frame == 0)
			printf("    %lu instructions; pixels not black: %u of the first 40 rows' %u, %u of the Game Boy's %u\n", n,
			       lit[0], 0x28 * 160, lit[1], 144 * 160);
	}
	/* Split at SIG3, as the player runs it: on the CPU's registers until the
	 * program sets SIG3, the rest on copies of them (another core's). */
	memset(g_rdram + VRAM_AT / 4, 0, RDRAM_SIZE - VRAM_AT);
	fill_input(0);
	load_task(size);
	const unsigned long first = rsp_lle_run(0x400, 50000000, 1, &result);
	CHECK(result == RSP_LLE_AT_SIG3);
	CHECK((g_sp[4] & 0x603) == 0x400); /* running, SIG3 set, no SIG2 */
	int read = 1; /* the input is read by then: the CPU may change it */
	for (uint32_t a = 0; a < 0x6000; a++)
		read &= get8(g_rdram, VRAM_AT + a) == get8(g_rdram, STACK_AT + a);
	CHECK(read);
	uint32_t mi = g_mi_intr, sp[8], pc = g_sp_pc, dpc[8];
	memcpy(sp, g_sp, sizeof(sp));
	memcpy(dpc, g_dpc, sizeof(dpc));
	const rsp_lle_regs copies = {(unsigned char *)g_rdram, (unsigned char *)g_spmem, (unsigned char *)g_spmem + 0x1000, &mi,
	                             {&sp[0], &sp[1], &sp[2], &sp[3], &sp[4], &sp[5], &sp[6], &sp[7]}, &pc,
	                             {&dpc[0], &dpc[1], &dpc[2], &dpc[3], &dpc[4], &dpc[5], &dpc[6], &dpc[7]}, check_interrupts};
	rsp_lle_bind(&copies);
	const unsigned long rest = rsp_lle_run(0x400, 50000000, 0, &result);
	rsp_lle_bind(&g_regs);
	CHECK(result == RSP_LLE_BROKE);
	CHECK((sp[4] & 0x203) == 0x203);
	CHECK((g_sp[4] & 0x203) == 0); /* the CPU's registers untouched by the rest */
	CHECK(first + rest == n_first);
	uint32_t split_sum = 0;
	for (uint32_t a = 0; a < ROWS * ROW_BYTES; a++)
		split_sum = split_sum * 31 + get8(g_rdram, OUT_AT + a);
	CHECK(split_sum == sum[0]);
	printf("    SIG3 set after %lu of its %lu instructions\n", first, first + rest);

	CHECK(sum[0] == sum[2]);
	CHECK(sum[1] == sum[3]);
	CHECK(sum[0] != sum[1]);
	/* The Wii U's estimate: 30-50 Espresso cycles an interpreted RSP instruction, at 1.24 GHz. */
	const double avg = total / (double)frames;
	printf("    %d frames: %lu-%lu instructions a task, %.0f us a task on this PC; on the Wii U about %.0f-%.0f ms\n",
	       frames, min, max, us / frames, avg * 30 / 1.24e6, avg * 50 / 1.24e6);
}

int main(void)
{
	const char *path = getenv("OUR64_PS2_ROM");
	if (!path) {
		printf("  skipped: set OUR64_PS2_ROM to a Pokemon Stadium 2 (USA) z64\n");
		return 0;
	}
	long size;
	uint8_t *rom = read_file(path, &size);
	if (!rom || size < 0x1000 || be32(rom) != 0x80371240) {
		printf("  skipped: no Pokemon Stadium 2 z64 at %s (set OUR64_PS2_ROM)\n", path);
		free(rom);
		return 0;
	}
	g_rdram = calloc(1, RDRAM_SIZE);
	g_regs = (rsp_lle_regs){(unsigned char *)g_rdram, (unsigned char *)g_spmem, (unsigned char *)g_spmem + 0x1000, &g_mi_intr,
	                  {&g_sp[0], &g_sp[1], &g_sp[2], &g_sp[3], &g_sp[4], &g_sp[5], &g_sp[6], &g_sp[7]}, &g_sp_pc,
	                  {&g_dpc[0], &g_dpc[1], &g_dpc[2], &g_dpc[3], &g_dpc[4], &g_dpc[5], &g_dpc[6], &g_dpc[7]},
	                  check_interrupts};
	rsp_lle_init(&g_regs);

	/* Each copy of the boot code: its size is in `addiu $5, $11, size` at 0x1078,
	 * where it reads the 0xC0 bytes of tables after itself. */
	int found = 0;
	for (long off = 0; off + 0x1000 <= size; off += 0x10) {
		if (be32(rom + off) != 0x401A6000 || be32(rom + off + 4) != 0xC800207C)
			continue;
		uint32_t w = be32(rom + off + 0x78);
		CHECK(w >> 16 == 0x2565);
		uint32_t boot = w & 0xFFFF;
		CHECK(boot == 0xA10 || boot == 0xB30 || boot == 0xA60);
		if (w >> 16 == 0x2565 && boot < 0xF00) {
			test_program(rom, (uint32_t)off, boot);
			found++;
		}
	}
	CHECK(found == 3);
	printf("  %u cxd4 messages (reserved instructions, odd register writes)\n", rsp_lle_messages());
	free(g_rdram);
	free(rom);
	return g_fail;
}
