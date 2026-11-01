/* Our64 bench: the whole ROM sits in RAM (the Wii U has room for any N64
 * ROM), so the ROM cache is a plain buffer. File access is stdio on the SD. */
#include <gctypes.h>
#include "main/ROM-Cache.h"
#include "main/rom.h"

#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern int rom_length;
int  init_byte_swap(unsigned int magicWord);

static u8 *g_rom;

/* Not64's file-browser hooks, backed by stdio on full paths in file->name. */
static int sd_readFile(fileBrowser_file *file, void *buf, unsigned int len)
{
	FILE *f = fopen(file->name, "rb");
	if (!f) return FILE_BROWSER_ERROR;
	fseek(f, file->offset, SEEK_SET);
	int n = (int)fread(buf, 1, len, f);
	fclose(f);
	if (n > 0) file->offset += n;
	return n;
}

static int sd_seekFile(fileBrowser_file *file, unsigned int where, unsigned int type)
{
	if (type == FILE_BROWSER_SEEK_SET) file->offset = where;
	else if (type == FILE_BROWSER_SEEK_CUR) file->offset += where;
	else file->offset = file->size + where;
	return 0;
}

static int sd_none(fileBrowser_file *file) { (void)file; return 0; }
static int sd_writeNone(fileBrowser_file *file, void *buf, unsigned int len) { (void)file; (void)buf; return (int)len; }
static int sd_readNone(fileBrowser_file *file, void *buf, unsigned int len) { (void)file; (void)buf; (void)len; return 0; }

int (*romFile_init)(fileBrowser_file *) = sd_none;
int (*romFile_readFile)(fileBrowser_file *, void *, unsigned int) = sd_readFile;
int (*romFile_seekFile)(fileBrowser_file *, unsigned int, unsigned int) = sd_seekFile;
int (*romFile_deinit)(fileBrowser_file *) = sd_none;
/* Saves are not written by the benchmark. */
int (*saveFile_init)(fileBrowser_file *) = sd_none;
int (*saveFile_readFile)(fileBrowser_file *, void *, unsigned int) = sd_readNone;
int (*saveFile_writeFile)(fileBrowser_file *, void *, unsigned int) = sd_writeNone;
int (*saveFile_deinit)(fileBrowser_file *) = sd_none;
fileBrowser_file *saveFile_dir;

void ROMCache_init(fileBrowser_file *file)
{
	rom_length = file->size;
}

void ROMCache_deinit(void)
{
	free(g_rom);
	g_rom = NULL;
}

void *ROMCache_pointer(u32 rom_offset) { return g_rom + rom_offset; }

void ROMCache_read(u8 *ram_dest, u32 rom_offset, u32 length)
{
	if (rom_offset >= (u32)rom_length) { memset(ram_dest, 0, length); return; }
	if (rom_offset + length > (u32)rom_length) {
		u32 have = rom_length - rom_offset;
		memcpy(ram_dest, g_rom + rom_offset, have);
		memset(ram_dest + have, 0, length - have);
		return;
	}
	memcpy(ram_dest, g_rom + rom_offset, length);
}

void ROMCache_write(u8 *ram_src, u32 rom_offset, u32 length)
{
	memcpy(g_rom + rom_offset, ram_src, length);
}

/* Called between chunks of the read with the bytes read so far (the
 * player's loading bar); NULL for none. */
void (*romcache_progress)(unsigned done, unsigned total);

int ROMCache_load(fileBrowser_file *file)
{
	free(g_rom);
	g_rom = memalign(64, (rom_length + 3) & ~3);
	if (!g_rom) return ROM_CACHE_ERROR_READ;
	file->offset = 0;
	const int chunk = 2 * 1024 * 1024;
	for (int at = 0; at < rom_length; at += chunk) {
		int n = rom_length - at < chunk ? rom_length - at : chunk;
		if (romFile_readFile(file, g_rom + at, n) != n)
			return ROM_CACHE_ERROR_READ;
		if (romcache_progress)
			romcache_progress((unsigned)(at + n), (unsigned)rom_length);
	}
	if (init_byte_swap(*(u32 *)g_rom) == BYTE_SWAP_BAD)
		return ROM_CACHE_INVALID_ROM;
	byte_swap((char *)g_rom, rom_length);
	return 0;
}
