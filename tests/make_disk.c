/* Makes the USB drive images test_usbfs reads, with a read-write FatFs build
 * (tests/fatfs_rw/ffconf.h). make_disk <dir> writes:
 *   disk.img      64 MB, MBR: 1 FAT32 "GAMES" with /Games/N64/Banjo.z64 and an
 *                 empty /Saves, 2 exFAT "MORE" with /Mario.n64
 *   disk4k.img    64 MB in 4096-byte sectors, MBR: 1 exFAT "BIG" with /Mario.n64
 *   wfs.img       an MBR with one type 0x07 partition of random bytes: how a
 *                 Wii U-formatted drive (or one split by 5usbpart) looks
 *   stick.img     a superfloppy: FAT32 at sector 0, no MBR, no label
 *   badlabel.img  a superfloppy exFAT whose label entry claims 255 characters
 *   badfat.img    stick.img's boot sector with two FATs of 2^31 sectors each:
 *                 their total wraps to 0 in 32 bits
 *   badclus.img   a superfloppy exFAT that claims 255 clusters (256 is the least)
 *   mixed.img     80 MB, MBR: 1 type 0x07 of random bytes, 2 FAT32 with /Yoshi.z64
 *   gpt.img       100 MB, GPT: 1 FAT32 "GPT ONE" with /Kirby.z64, 2 exFAT "GPT TWO" */
#include "ff.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int usbfs_test_image(int pdrv, const char *path, unsigned sector_size);

/* "0:" and "1:" are partitions 1 and 2 of the drive, "2:" the whole drive. */
PARTITION VolToPart[FF_VOLUMES] = {{0, 1}, {0, 2}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}};

static BYTE g_work[65536];
static const char *g_dir;

#define TRY(x) do { FRESULT r_ = (x); if (r_ != FR_OK) { fprintf(stderr, "make_disk: %s: %s gave %d\n", g_path, #x, r_); exit(1); } } while (0)
static char g_path[512];

static const char *image(const char *name, long long bytes, unsigned ss)
{
	snprintf(g_path, sizeof(g_path), "%s/%s", g_dir, name);
	FILE *f = fopen(g_path, "wb");
	if (!f || ftruncate(fileno(f), bytes) != 0) {
		fprintf(stderr, "make_disk: cannot write %s\n", g_path);
		exit(1);
	}
	fclose(f);
	if (usbfs_test_image(0, g_path, ss) != 0)
		exit(1);
	return g_path;
}

static void file(const char *path, unsigned size, unsigned seed)
{
	FIL f;
	BYTE b[256];
	UINT n;
	for (unsigned i = 0; i < size; i++)
		b[i] = (BYTE)(seed + i * 3);
	TRY(f_open(&f, path, FA_WRITE | FA_CREATE_ALWAYS));
	TRY(f_write(&f, b, size, &n));
	TRY(f_close(&f));
}

static void format(const char *drive, BYTE fmt, DWORD au)
{
	MKFS_PARM opt = {fmt, 0, 0, 0, au};
	TRY(f_mkfs(drive, &opt, g_work, sizeof(g_work)));
}

static void put32(BYTE *p, DWORD v) { p[0] = (BYTE)v; p[1] = (BYTE)(v >> 8); p[2] = (BYTE)(v >> 16); p[3] = (BYTE)(v >> 24); }
static DWORD get32(const BYTE *p) { return p[0] | p[1] << 8 | p[2] << 16 | (DWORD)p[3] << 24; }

static void rw(FILE *f, long long at, BYTE *b, unsigned n, int write)
{
	if (fseeko(f, at, SEEK_SET) != 0 || (write ? fwrite(b, 1, n, f) : fread(b, 1, n, f)) != n) {
		fprintf(stderr, "make_disk: %s at %lld\n", write ? "write" : "read", at);
		exit(1);
	}
}

/* Random bytes over sectors [from, to) of the image at path (closed in FatFs). */
static void junk(const char *path, long long from, long long to)
{
	BYTE s[512];
	uint32_t x = 0x2545F491;
	FILE *f = fopen(path, "r+b");
	if (!f)
		exit(1);
	for (long long at = from; at < to; at++) {
		for (int i = 0; i < 512; i += 4) {
			x ^= x << 13; x ^= x >> 17; x ^= x << 5;
			put32(s + i, x);
		}
		rw(f, at * 512, s, 512, 1);
	}
	fclose(f);
}

/* Patches one little-endian DWORD at byte at of the image at path. */
static void poke32(const char *path, long long at, DWORD v)
{
	BYTE b[4];
	FILE *f = fopen(path, "r+b");
	if (!f)
		exit(1);
	put32(b, v);
	rw(f, at, b, 4, 1);
	fclose(f);
}

int main(int argc, char **argv)
{
	FATFS fs;
	if (argc != 2) {
		fprintf(stderr, "usage: make_disk <dir>\n");
		return 2;
	}
	g_dir = argv[1];

	/* disk.img */
	image("disk.img", 64ll << 20, 512);
	{
		LBA_t parts[] = {75, 25, 0}; /* percent */
		TRY(f_fdisk(0, parts, g_work));
	}
	format("0:", FM_FAT32, 512);
	TRY(f_mount(&fs, "0:", 1));
	TRY(f_setlabel("0:GAMES"));
	TRY(f_mkdir("0:/Games"));
	TRY(f_mkdir("0:/Games/N64"));
	file("0:/Games/N64/Banjo.z64", 64, 1);
	file("0:/Games/N64/Pok\xC3\xA9mon Stadium.z64", 16, 2);
	TRY(f_mkdir("0:/Saves"));
	TRY(f_unmount("0:"));
	format("1:", FM_EXFAT, 4096);
	TRY(f_mount(&fs, "1:", 1));
	TRY(f_setlabel("1:MORE"));
	file("1:/Mario.n64", 32, 3);
	TRY(f_unmount("1:"));

	/* disk4k.img */
	image("disk4k.img", 64ll << 20, 4096);
	{
		LBA_t parts[] = {100, 0};
		TRY(f_fdisk(0, parts, g_work));
	}
	format("0:", FM_EXFAT, 4096);
	TRY(f_mount(&fs, "0:", 1));
	TRY(f_setlabel("0:BIG"));
	file("0:/Mario.n64", 32, 4);
	TRY(f_unmount("0:"));

	/* stick.img */
	image("stick.img", 40ll << 20, 512);
	format("2:", FM_FAT32 | FM_SFD, 512);
	TRY(f_mount(&fs, "2:", 1));
	file("2:/Paper Mario.z64", 64, 5);
	TRY(f_unmount("2:"));

	/* badlabel.img: the 0x83 entry's character count says 255, its 11 slots hold W */
	image("badlabel.img", 8ll << 20, 512);
	format("2:", FM_EXFAT | FM_SFD, 4096);
	TRY(f_mount(&fs, "2:", 1));
	TRY(f_setlabel("2:LABEL"));
	TRY(f_unmount("2:"));
	usbfs_test_image(0, NULL, 512);
	{
		FILE *f = fopen(g_path, "r+b");
		BYTE vbr[512], dir[512];
		if (!f)
			return 1;
		rw(f, 0, vbr, 512, 0);
		long long root = (long long)(get32(vbr + 88) + ((get32(vbr + 96) - 2) << vbr[109])) << vbr[108];
		rw(f, root, dir, 512, 0);
		int found = 0;
		for (int e = 0; e < 512; e += 32) {
			if (dir[e] != 0x83)
				continue;
			dir[e + 1] = 255;
			for (int i = 0; i < 11; i++) {
				dir[e + 2 + i * 2] = 'W';
				dir[e + 3 + i * 2] = 0;
			}
			found = 1;
			break;
		}
		if (!found) {
			fprintf(stderr, "make_disk: no label entry in %s\n", g_path);
			return 1;
		}
		rw(f, root, dir, 512, 1);
		fclose(f);
	}

	/* badfat.img: stick.img's boot sector, FATSz32 0x80000000 and two FATs */
	{
		BYTE vbr[512];
		char stick[512];
		snprintf(stick, sizeof(stick), "%s/stick.img", g_dir);
		FILE *f = fopen(stick, "rb");
		if (!f)
			return 1;
		rw(f, 0, vbr, 512, 0);
		fclose(f);
		image("badfat.img", 1 << 20, 512);
		usbfs_test_image(0, NULL, 512);
		vbr[16] = 2;               /* BPB_NumFATs */
		put32(vbr + 36, 0x80000000); /* BPB_FATSz32 */
		f = fopen(g_path, "r+b");
		if (!f)
			return 1;
		rw(f, 0, vbr, 512, 1);
		fclose(f);
	}

	/* wfs.img: 4 MB, partition 1 from sector 64 to the end, type 0x07, random bytes */
	{
		BYTE s[512];
		image("wfs.img", 4 << 20, 512);
		usbfs_test_image(0, NULL, 512);
		FILE *f = fopen(g_path, "r+b");
		if (!f)
			return 1;
		memset(s, 0, sizeof(s));
		s[446 + 4] = 0x07;
		put32(s + 446 + 8, 64);
		put32(s + 446 + 12, (4 << 20) / 512 - 64);
		s[510] = 0x55;
		s[511] = 0xAA;
		rw(f, 0, s, 512, 1);
		fclose(f);
		junk(g_path, 64, (4 << 20) / 512);
	}

	/* badclus.img: exFAT's ClusterCount (VBR offset 92) one below FatFs's least */
	image("badclus.img", 8ll << 20, 512);
	format("2:", FM_EXFAT | FM_SFD, 4096);
	usbfs_test_image(0, NULL, 512);
	poke32(g_path, 92, 0xFF);

	/* mixed.img: FatFs's own search would take partition 2 for partition 1 */
	image("mixed.img", 80ll << 20, 512);
	{
		LBA_t parts[] = {50, 50, 0};
		TRY(f_fdisk(0, parts, g_work));
	}
	format("1:", FM_FAT32, 512);
	TRY(f_mount(&fs, "1:", 1));
	file("1:/Yoshi.z64", 64, 6);
	TRY(f_unmount("1:"));
	usbfs_test_image(0, NULL, 512);
	{
		BYTE mbr[512];
		FILE *f = fopen(g_path, "rb");
		if (!f)
			return 1;
		rw(f, 0, mbr, 512, 0);
		fclose(f);
		if (mbr[446 + 4] != 0x07) {
			fprintf(stderr, "make_disk: %s partition 1 is type %02X\n", g_path, mbr[446 + 4]);
			return 1;
		}
		DWORD start = get32(mbr + 446 + 8);
		junk(g_path, start, start + 64);
	}

	/* gpt.img: at least FF_MIN_GPT sectors, so f_fdisk writes GPT */
	image("gpt.img", 100ll << 20, 512);
	{
		LBA_t parts[] = {60, 40, 0};
		TRY(f_fdisk(0, parts, g_work));
	}
	format("0:", FM_FAT32, 512);
	TRY(f_mount(&fs, "0:", 1));
	TRY(f_setlabel("0:GPT ONE"));
	file("0:/Kirby.z64", 64, 7);
	TRY(f_unmount("0:"));
	format("1:", FM_EXFAT, 4096);
	TRY(f_mount(&fs, "1:", 1));
	TRY(f_setlabel("1:GPT TWO"));
	TRY(f_unmount("1:"));
	usbfs_test_image(0, NULL, 512);
	{
		BYTE mbr[512], hdr[512];
		FILE *f = fopen(g_path, "rb");
		if (!f)
			return 1;
		rw(f, 0, mbr, 512, 0);
		rw(f, 512, hdr, 512, 0);
		fclose(f);
		if (mbr[446 + 4] != 0xEE || memcmp(hdr, "EFI PART", 8)) {
			fprintf(stderr, "make_disk: %s is not GPT\n", g_path);
			return 1;
		}
	}
	return 0;
}
