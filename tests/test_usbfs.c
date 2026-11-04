/* PC tests for player/source/usbfs.c: USB partitions found, named and read
 * through FatFs, and settings.cfg's "usb:<serial>/path" folders, on
 * the disk images make_disk writes into build/. */
#include "usbfs.h"
#include "ff.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int usbfs_test_image(int pdrv, const char *path, unsigned sector_size);

static int g_failed, g_run;

#define CHECK(cond) do { if (!(cond)) { printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); g_failed++; return; } } while (0)

#define IMG(name) "build/" name

/* Drive 1 and drive 2 hold these images (NULL: nothing plugged in). */
static int plug(const char *one, const char *two, unsigned ss)
{
	return usbfs_test_image(0, one, ss) == 0 && usbfs_test_image(1, two, ss) == 0;
}

/* The volume serial in the boot sector at sector lba of an image. */
static uint32_t serial_at(const char *img, unsigned long long lba, int exfat)
{
	unsigned char b[4] = {0};
	FILE *f = fopen(img, "rb");
	if (!f)
		return 0;
	fseeko(f, (off_t)(lba * 512 + (exfat ? 100 : 67)), SEEK_SET);
	if (fread(b, 1, 4, f) != 4)
		b[0] = b[1] = b[2] = b[3] = 0;
	fclose(f);
	return b[0] | b[1] << 8 | b[2] << 16 | (uint32_t)b[3] << 24;
}

/* Partition p's first sector, from an image's MBR. */
static unsigned long long part_lba(const char *img, int p)
{
	unsigned char b[4] = {0};
	FILE *f = fopen(img, "rb");
	if (!f)
		return 0;
	fseek(f, 446 + (p - 1) * 16 + 8, SEEK_SET);
	if (fread(b, 1, 4, f) != 4)
		b[0] = b[1] = b[2] = b[3] = 0;
	fclose(f);
	return b[0] | b[1] << 8 | b[2] << 16 | (uint32_t)b[3] << 24;
}

static void a_partitioned_drive_mounts_both_partitions(void)
{
	usbfs_volume v[8];
	CHECK(plug(IMG("disk.img"), NULL, 512));
	int n = usbfs_mount_all(v, 8);
	CHECK(n == 2);
	CHECK(v[0].drive == 1 && v[0].part == 1 && v[0].mounted && !v[0].exfat);
	CHECK(!strcmp(v[0].label, "GAMES"));
	CHECK(v[1].drive == 1 && v[1].part == 2 && v[1].mounted && v[1].exfat);
	CHECK(!strcmp(v[1].label, "MORE"));
	CHECK(v[0].serial == serial_at(IMG("disk.img"), part_lba(IMG("disk.img"), 1), 0));
	CHECK(v[1].serial == serial_at(IMG("disk.img"), part_lba(IMG("disk.img"), 2), 1));
	CHECK(v[0].serial != v[1].serial);
	/* max caps what is filled in */
	CHECK(usbfs_mount_all(v, 1) == 1 && v[0].part == 1);
}

static void files_read_through_fatfs(void)
{
	usbfs_volume v[8];
	CHECK(plug(IMG("disk.img"), NULL, 512));
	CHECK(usbfs_mount_all(v, 8) == 2);
	DIR d;
	FILINFO fi;
	int banjo = 0, pokemon = 0, others = 0;
	CHECK(f_opendir(&d, "0:/Games/N64") == FR_OK);
	while (f_readdir(&d, &fi) == FR_OK && fi.fname[0]) {
		if (!strcmp(fi.fname, "Banjo.z64") && fi.fsize == 64)
			banjo = 1;
		else if (!strcmp(fi.fname, "Pok\xC3\xA9mon Stadium.z64"))
			pokemon = 1;
		else
			others++;
	}
	f_closedir(&d);
	CHECK(banjo && pokemon && !others);
	CHECK(f_opendir(&d, "0:/Saves") == FR_OK);
	CHECK(f_readdir(&d, &fi) == FR_OK && !fi.fname[0]);
	f_closedir(&d);

	FIL f;
	unsigned char b[100];
	UINT got;
	CHECK(f_open(&f, "0:/Games/N64/Banjo.z64", FA_READ) == FR_OK);
	CHECK(f_read(&f, b, sizeof(b), &got) == FR_OK && got == 64);
	f_close(&f);
	for (int i = 0; i < 64; i++)
		CHECK(b[i] == (unsigned char)(1 + i * 3));
	CHECK(f_open(&f, "1:/Mario.n64", FA_READ) == FR_OK);
	CHECK(f_read(&f, b, sizeof(b), &got) == FR_OK && got == 32 && b[31] == (unsigned char)(3 + 31 * 3));
	f_close(&f);
}

static void stored_paths_name_the_serial(void)
{
	usbfs_volume v[8];
	char out[256], back[256], want[256];
	CHECK(plug(IMG("disk.img"), NULL, 512));
	int n = usbfs_mount_all(v, 8);
	CHECK(n == 2);
	snprintf(want, sizeof(want), "usb:%08X/Games/N64", (unsigned)v[0].serial);
	CHECK(usbfs_store_path(v, n, "usb1p1:/Games/N64", out, sizeof(out)) == 0);
	CHECK(!strcmp(out, want));
	CHECK(usbfs_device_path(v, n, out, back, sizeof(back)) == 0);
	CHECK(!strcmp(back, "usb1p1:/Games/N64"));
	/* the root, both ways */
	snprintf(want, sizeof(want), "usb:%08X/", (unsigned)v[1].serial);
	CHECK(usbfs_store_path(v, n, "usb1p2:/", out, sizeof(out)) == 0 && !strcmp(out, want));
	CHECK(usbfs_device_path(v, n, want, out, sizeof(out)) == 0 && !strcmp(out, "usb1p2:/"));
	/* lowercase hex reads too */
	snprintf(want, sizeof(want), "usb:%08x/Mario", (unsigned)v[1].serial);
	CHECK(usbfs_device_path(v, n, want, out, sizeof(out)) == 0 && !strcmp(out, "usb1p2:/Mario"));
	/* the SD card is left alone */
	CHECK(usbfs_store_path(v, n, "sd:/x", out, sizeof(out)) == 0 && !strcmp(out, "sd:/x"));
	CHECK(usbfs_device_path(v, n, "sd:/x", out, sizeof(out)) == 0 && !strcmp(out, "sd:/x"));
	/* a drive that is not plugged in, a partition that did not mount, a broken serial */
	uint32_t other = v[0].serial ^ v[1].serial ^ 0x5A5A5A5Au;
	snprintf(want, sizeof(want), "usb:%08X/Games", (unsigned)other);
	CHECK(usbfs_device_path(v, n, want, out, sizeof(out)) == -1);
	CHECK(usbfs_store_path(v, n, "usb2p1:/x", out, sizeof(out)) == -1);
	CHECK(usbfs_store_path(v, n, "usb1p3:/x", out, sizeof(out)) == -1);
	CHECK(usbfs_device_path(v, n, "usb:1234/x", out, sizeof(out)) == -1);
	CHECK(usbfs_device_path(v, n, "usb:1234567G/x", out, sizeof(out)) == -1);
	CHECK(usbfs_device_path(v, n, "usb:123456789/x", out, sizeof(out)) == -1);
	/* too small for the answer */
	CHECK(usbfs_store_path(v, n, "usb1p1:/Games/N64", out, 10) == -1);
	CHECK(usbfs_store_path(v, n, "sd:/Games/N64", out, 5) == -1);
	CHECK(usbfs_device_path(v, n, "sd:/Games/N64", out, 5) == -1);
}

static void the_first_of_two_volumes_with_one_serial_wins(void)
{
	usbfs_volume v[2] = {
		{.drive = 2, .part = 3, .mounted = 1, .serial = 0x1A2B3C4D},
		{.drive = 1, .part = 1, .mounted = 1, .serial = 0x1A2B3C4D},
	};
	char out[64];
	CHECK(usbfs_device_path(v, 2, "usb:1A2B3C4D/Games/N64", out, sizeof(out)) == 0);
	CHECK(!strcmp(out, "usb2p3:/Games/N64"));
	CHECK(usbfs_store_path(v, 2, "usb1p1:/Games/N64", out, sizeof(out)) == 0);
	CHECK(!strcmp(out, "usb:1A2B3C4D/Games/N64"));
}

static void names_and_devices(void)
{
	usbfs_volume v[8];
	char s[64];
	CHECK(plug(IMG("disk.img"), NULL, 512));
	CHECK(usbfs_mount_all(v, 8) == 2);
	usbfs_name(&v[0], s, sizeof(s));
	CHECK(!strcmp(s, "USB GAMES"));
	usbfs_device(&v[1], s, sizeof(s));
	CHECK(!strcmp(s, "usb1p2"));
	usbfs_volume plain = {.drive = 1, .part = 2, .mounted = 1};
	usbfs_name(&plain, s, sizeof(s));
	CHECK(!strcmp(s, "USB drive 1, partition 2"));
	usbfs_name(&plain, s, 8);
	CHECK(!strcmp(s, "USB dri"));
	CHECK(usbfs_volume_index("usb1p1") == 0);
	CHECK(usbfs_volume_index("usb1p2:/Games") == 1);
	CHECK(usbfs_volume_index("usb1p3") == -1);
	CHECK(usbfs_volume_index("usb2p1") == -1);
	CHECK(usbfs_volume_index("usb3p1") == -1);
	CHECK(usbfs_volume_index("usb1p5") == -1);
	CHECK(usbfs_volume_index("usb1p12") == -1);
	CHECK(usbfs_volume_index("sd") == -1);
	usbfs_unmount_all();
	CHECK(usbfs_volume_index("usb1p1") == -1);
	CHECK(f_opendir(&(DIR){0}, "0:/") != FR_OK);
}

static void a_wii_u_formatted_drive_mounts_nothing(void)
{
	usbfs_volume v[8];
	CHECK(plug(IMG("wfs.img"), NULL, 512));
	CHECK(usbfs_mount_all(v, 8) == 0);
	CHECK(plug(NULL, NULL, 512));
	CHECK(usbfs_mount_all(v, 8) == 0);
}

static void a_stick_without_a_partition_table_is_partition_1(void)
{
	usbfs_volume v[8];
	char s[64];
	CHECK(plug(IMG("stick.img"), NULL, 512));
	CHECK(usbfs_mount_all(v, 8) == 1);
	CHECK(v[0].drive == 1 && v[0].part == 1 && !v[0].exfat && !v[0].label[0]);
	CHECK(v[0].serial == serial_at(IMG("stick.img"), 0, 0));
	usbfs_name(&v[0], s, sizeof(s));
	CHECK(!strcmp(s, "USB drive 1, partition 1"));
	FILINFO fi;
	CHECK(f_stat("0:/Paper Mario.z64", &fi) == FR_OK && fi.fsize == 64);
}

static void two_drives_mount_side_by_side(void)
{
	usbfs_volume v[8];
	CHECK(plug(IMG("disk.img"), IMG("stick.img"), 512));
	CHECK(usbfs_mount_all(v, 8) == 3);
	CHECK(v[2].drive == 2 && v[2].part == 1);
	CHECK(usbfs_volume_index("usb2p1") == 4);
	FILINFO fi;
	CHECK(f_stat("4:/Paper Mario.z64", &fi) == FR_OK);
	/* the stick's folder is found again with the stick alone, now in drive 1 */
	char stored[128], out[128];
	CHECK(usbfs_store_path(v, 3, "usb2p1:/", stored, sizeof(stored)) == 0);
	CHECK(plug(IMG("stick.img"), NULL, 512));
	int n = usbfs_mount_all(v, 8);
	CHECK(n == 1);
	CHECK(usbfs_device_path(v, n, stored, out, sizeof(out)) == 0 && !strcmp(out, "usb1p1:/"));
}

static void sectors_of_4096_bytes(void)
{
	usbfs_volume v[8];
	CHECK(plug(IMG("disk4k.img"), NULL, 4096));
	CHECK(usbfs_mount_all(v, 8) == 1);
	CHECK(v[0].part == 1 && v[0].exfat && !strcmp(v[0].label, "BIG"));
	FILINFO fi;
	CHECK(f_stat("0:/Mario.n64", &fi) == FR_OK && fi.fsize == 32);
}

static void a_corrupt_exfat_label_stops_at_11_characters(void)
{
	usbfs_volume v[8];
	CHECK(plug(IMG("badlabel.img"), NULL, 512));
	CHECK(usbfs_mount_all(v, 8) == 1);
	CHECK(v[0].exfat && !strcmp(v[0].label, "WWWWWWWWWWW"));
}

static void a_fat_size_that_wraps_does_not_mount(void)
{
	usbfs_volume v[8];
	CHECK(plug(IMG("badfat.img"), NULL, 512));
	CHECK(usbfs_mount_all(v, 8) == 0);
}

#define RUN(fn) do { g_run++; int before = g_failed; fn(); if (g_failed == before) printf("  ok   %s\n", #fn); } while (0)

int main(void)
{
	setvbuf(stdout, NULL, _IOLBF, 0);
	RUN(a_partitioned_drive_mounts_both_partitions);
	RUN(files_read_through_fatfs);
	RUN(stored_paths_name_the_serial);
	RUN(the_first_of_two_volumes_with_one_serial_wins);
	RUN(names_and_devices);
	RUN(a_wii_u_formatted_drive_mounts_nothing);
	RUN(a_stick_without_a_partition_table_is_partition_1);
	RUN(two_drives_mount_side_by_side);
	RUN(sectors_of_4096_bytes);
	RUN(a_corrupt_exfat_label_stops_at_11_characters);
	RUN(a_fat_size_that_wraps_does_not_mount);
	usbfs_unmount_all();
	plug(NULL, NULL, 512);
	printf("%d of %d failed\n", g_failed, g_run);
	return g_failed != 0;
}
