/* Our64 bench: frame capture for the stage-2 replay benchmark.
 *
 * At each graphics task in the capture window, writes everything a graphics
 * plugin reads to sd:/wiiu/our64/frames/<rom>-<task>.o64f, so a decoder can
 * replay the frame on the PC or the console without the CPU core.
 * Format (big-endian): o64f_header, DMEM (4 KB), IMEM (4 KB), then RDRAM
 * compressed with zlib. RDRAM is in N64 byte order, as Not64 keeps it. */
#include <gctypes.h>
#include "gc_memory/memory.h"
#include "main/rom.h"
#include "bench.h"
#include "capture.h"
#include "report.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <zlib.h>

extern unsigned long rdram[0x800000 / 4];

static char g_dir[256], g_rom[64], g_group[80];
static uint8_t g_zbuf[64 * 1024]; /* RDRAM streams through it: no 8 MB buffer to find */
static z_stream g_z;
static int g_z_ok;

void capture_begin(const char *sd_root, const char *rom_file)
{
	snprintf(g_dir, sizeof(g_dir), "%s/frames", sd_root);
	mkdir(g_dir, 0777);
	snprintf(g_rom, sizeof(g_rom), "%s", rom_file);
	char *dot = strrchr(g_rom, '.');
	if (dot) *dot = 0;
	snprintf(g_group, sizeof(g_group), "%s", g_rom);
	if (!g_z_ok) {
		memset(&g_z, 0, sizeof(g_z));
		g_z_ok = deflateInit(&g_z, 1) == Z_OK;
		if (!g_z_ok)
			report_line("CAPTURE: zlib could not start (no memory?)");
	}
}

void capture_group(uint32_t vi)
{
	snprintf(g_group, sizeof(g_group), "%s-v%u", g_rom, (unsigned)vi);
}

int capture_frame(uint32_t vi, uint32_t task)
{
	o64f_header h;
	memset(&h, 0, sizeof(h));
	memcpy(h.magic, "O64F", 4);
	h.version = O64F_VERSION;
	memcpy(h.rom_name, ROM_HEADER.Name, sizeof(h.rom_name));
	h.rom_crc[0] = ROM_HEADER.CRC1;
	h.rom_crc[1] = ROM_HEADER.CRC2;
	h.vi_count = vi;
	h.task_index = task;
	h.rdram_size = sizeof(rdram);
	memcpy(h.vi, &vi_register.vi_status, sizeof(h.vi));
	h.dpc[0] = dpc_register.dpc_start;
	h.dpc[1] = dpc_register.dpc_end;
	h.dpc[2] = dpc_register.dpc_current;
	h.dpc[3] = dpc_register.dpc_status;
	h.dpc[4] = dpc_register.dpc_clock;
	h.dpc[5] = dpc_register.dpc_bufbusy;
	h.dpc[6] = dpc_register.dpc_pipebusy;
	h.dpc[7] = dpc_register.dpc_tmem;
	h.sp_status = sp_register.sp_status_reg;
	h.mi_intr = MI_register.mi_intr_reg;

	char path[384];
	snprintf(path, sizeof(path), "%s/%s-%05u.o64f", g_dir, g_group, (unsigned)task);
	static unsigned told;
	if (!g_z_ok) return -1;
	FILE *f = fopen(path, "wb");
	if (!f) {
		if (told++ < 3)
			report_line("CAPTURE: could not open %s", path);
		return -1;
	}
	/* The header goes first with the compressed size left 0, then again once it is known. */
	int ok = fwrite(&h, sizeof(h), 1, f) == 1 &&
	         fwrite(SP_DMEMb, 0x1000, 1, f) == 1 &&
	         fwrite(SP_IMEMb, 0x1000, 1, f) == 1;
	deflateReset(&g_z);
	g_z.next_in = (Bytef *)rdram;
	g_z.avail_in = sizeof(rdram);
	int z = Z_OK;
	while (ok && z == Z_OK) {
		g_z.next_out = g_zbuf;
		g_z.avail_out = sizeof(g_zbuf);
		z = deflate(&g_z, Z_FINISH);
		size_t n = sizeof(g_zbuf) - g_z.avail_out;
		ok = (z == Z_OK || z == Z_STREAM_END) && (n == 0 || fwrite(g_zbuf, n, 1, f) == 1);
	}
	h.rdram_zsize = g_z.total_out;
	ok = ok && z == Z_STREAM_END && fseek(f, 0, SEEK_SET) == 0 && fwrite(&h, sizeof(h), 1, f) == 1;
	ok = fclose(f) == 0 && ok;
	if (!ok && told++ < 3)
		report_line("CAPTURE: writing %s failed (zlib %d)", path, z);
	return ok ? 0 : -1;
}
