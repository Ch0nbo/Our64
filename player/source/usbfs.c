/* Our64 player: USB drives as ROM folders, the portable part (usbfs.h).
 * FatFs logical drive (d - 1) * 4 + (p - 1) is drive d's partition p, so
 * "usb1p2:/x" is "1:/x" and "usb2p1:/x" is "4:/x". */
#include "usbfs.h"
#include "ff.h"

#include <stdio.h>
#include <string.h>

/* FatFs's table: logical drive -> physical drive (0 or 1) and partition.
 * Partition 1 is switched to 0 (FatFs's own search) while it is tried as a
 * superfloppy; see usbfs_mount_all. */
PARTITION VolToPart[FF_VOLUMES] = {
	{0, 1}, {0, 2}, {0, 3}, {0, 4},
	{1, 1}, {1, 2}, {1, 3}, {1, 4},
};

#define USBFS_VOLUMES (USBFS_DRIVES * USBFS_PARTS)
_Static_assert(USBFS_VOLUMES == FF_VOLUMES, "ffconf.h's FF_VOLUMES is the drives times the partitions");

static FATFS g_fs[USBFS_VOLUMES];
static int g_mounted[USBFS_VOLUMES];

static void drive_path(int vol, char path[3])
{
	path[0] = (char)('0' + vol);
	path[1] = ':';
	path[2] = 0;
}

static void unmount(int vol)
{
	char path[3];
	drive_path(vol, path);
	f_unmount(path);
	g_mounted[vol] = 0;
	VolToPart[vol].pt = (BYTE)(vol % USBFS_PARTS + 1);
}

void usbfs_unmount_all(void)
{
	for (int vol = 0; vol < USBFS_VOLUMES; vol++)
		unmount(vol);
}

/* A drive with no partition table and a FAT VBR at sector 0: with a forced
 * partition number FatFs reads the VBR's boot code as an MBR and finds
 * nothing, so partition 1 is first tried with FatFs's own search (pt 0),
 * which takes a VBR at sector 0 before any partition. Found anywhere but
 * sector 0, that search may have skipped a partition 1 that is not FAT, so
 * partition 1 is then mounted by number. *superfloppy says which it was. */
static FRESULT mount_part1(int vol, int *superfloppy)
{
	char path[3];
	drive_path(vol, path);
	*superfloppy = 0;
	VolToPart[vol].pt = 0;
	FRESULT r = f_mount(&g_fs[vol], path, 1);
	if (r == FR_OK && g_fs[vol].volbase == 0) {
		*superfloppy = 1;
		return FR_OK;
	}
	f_unmount(path);
	VolToPart[vol].pt = 1;
	return f_mount(&g_fs[vol], path, 1);
}

int usbfs_mount_all(usbfs_volume *out, int max)
{
	int n = 0;
	usbfs_unmount_all();
	for (int d = 0; d < USBFS_DRIVES; d++) {
		int superfloppy = 0;
		for (int p = 0; p < USBFS_PARTS && !superfloppy; p++) {
			int vol = d * USBFS_PARTS + p;
			char path[3];
			usbfs_volume v;
			DWORD serial = 0;
			drive_path(vol, path);
			memset(&v, 0, sizeof(v));
			FRESULT r = p == 0 ? mount_part1(vol, &superfloppy) : f_mount(&g_fs[vol], path, 1);
			if (r == FR_OK)
				r = f_getlabel(path, v.label, &serial);
			if (r != FR_OK) {
				unmount(vol);
				continue;
			}
			g_mounted[vol] = 1;
			v.drive = d + 1;
			v.part = p + 1;
			v.mounted = 1;
			v.serial = serial;
			v.exfat = g_fs[vol].fs_type == FS_EXFAT;
			if (out && n < max)
				out[n++] = v;
		}
	}
	return n;
}

void usbfs_device(const usbfs_volume *v, char *out, unsigned size)
{
	snprintf(out, size, "usb%dp%d", v->drive, v->part);
}

void usbfs_name(const usbfs_volume *v, char *out, unsigned size)
{
	if (v->label[0])
		snprintf(out, size, "USB %s", v->label);
	else
		snprintf(out, size, "USB drive %d, partition %d", v->drive, v->part);
}

/* "usb<D>p<P>" at the start of s, D 1-2 and P 1-4: its length (6), else 0. */
static int parse_device(const char *s, int *drive, int *part)
{
	if (strncmp(s, "usb", 3) || s[3] < '1' || s[3] > '0' + USBFS_DRIVES || s[4] != 'p' ||
	    s[5] < '1' || s[5] > '0' + USBFS_PARTS)
		return 0;
	*drive = s[3] - '0';
	*part = s[5] - '0';
	return 6;
}

int usbfs_volume_index(const char *device)
{
	int d, p;
	int len = parse_device(device, &d, &p);
	if (!len || (device[len] && device[len] != ':'))
		return -1;
	int vol = (d - 1) * USBFS_PARTS + (p - 1);
	return g_mounted[vol] ? vol : -1;
}

static int copy(const char *s, char *out, unsigned size)
{
	size_t n = strlen(s);
	if (n >= size)
		return -1;
	memcpy(out, s, n + 1);
	return 0;
}

static int fits(int written, unsigned size)
{
	return written >= 0 && (unsigned)written < size ? 0 : -1;
}

int usbfs_store_path(const usbfs_volume *v, int n, const char *device_path, char *out, unsigned size)
{
	int d, p;
	int len = parse_device(device_path, &d, &p);
	if (!len || device_path[len] != ':')
		return copy(device_path, out, size);
	const char *rest = device_path + len + 1;
	if (*rest == '/')
		rest++;
	for (int i = 0; i < n; i++)
		if (v[i].mounted && v[i].drive == d && v[i].part == p)
			return fits(snprintf(out, size, "usb:%08X/%s", (unsigned)v[i].serial, rest), size);
	return -1;
}

static int hex(char c)
{
	if (c >= '0' && c <= '9') return c - '0';
	if (c >= 'A' && c <= 'F') return c - 'A' + 10;
	if (c >= 'a' && c <= 'f') return c - 'a' + 10;
	return -1;
}

int usbfs_device_path(const usbfs_volume *v, int n, const char *stored, char *out, unsigned size)
{
	if (strncmp(stored, "usb:", 4))
		return copy(stored, out, size);
	uint32_t serial = 0;
	for (int i = 0; i < 8; i++) {
		int h = hex(stored[4 + i]);
		if (h < 0)
			return -1;
		serial = serial << 4 | (uint32_t)h;
	}
	const char *rest = stored + 12;
	if (*rest && *rest != '/')
		return -1;
	if (*rest == '/')
		rest++;
	/* two volumes with one serial (a cloned drive): the first in v wins */
	for (int i = 0; i < n; i++)
		if (v[i].mounted && v[i].serial == serial)
			return fits(snprintf(out, size, "usb%dp%d:/%s", v[i].drive, v[i].part, rest), size);
	return -1;
}
