/* Our64 player: USB drives as ROM folders (2026-11-04). FatFs reads FAT32 and
 * exFAT partitions read-only; each mounted partition is a device "usb<D>p<P>:/"
 * (drive 1-2, partition 1-4). settings.cfg stores "usb:<serial>/path", the
 * partition's volume serial, so another USB port finds the same drive.
 *
 * This part is portable: FatFs calls disk_read and friends for physical drive
 * 0 or 1 (drive 1 or 2), and only those differ between the console (raw
 * sector reads) and the PC tests (image files).
 *
 * FatFs is built without its own locks (FF_FS_REENTRANT 0), and the cover
 * fetch and the menu both open ROM files from their own threads: every FatFs
 * call, these functions included, must sit under one lock, which the
 * console's devoptab holds. */
#ifndef OUR64_USBFS_H
#define OUR64_USBFS_H

#include <stdint.h>

#define USBFS_DRIVES 2
#define USBFS_PARTS 4

typedef struct {
	int drive, part;     /* 1-based */
	int mounted;         /* FatFs mounted it */
	char label[34];      /* volume label in UTF-8, "" none (FatFs needs 34 bytes) */
	uint32_t serial;
	int exfat;
} usbfs_volume;

/* Unmounts what was mounted, then tries every drive x partition and fills out
 * with the volumes that mounted, drive by drive, partition by partition.
 * Returns how many it filled (at most max). A drive with no partition table
 * and a FAT or exFAT volume at its first sector (a "superfloppy" stick) is
 * partition 1. */
int  usbfs_mount_all(usbfs_volume *out, int max);
void usbfs_unmount_all(void);

void usbfs_device(const usbfs_volume *v, char *out, unsigned size);   /* "usb1p2" */
/* "USB GAMES", "USB drive OUR64 EXFAT", "USB drive 1, partition 2" (no label) */
void usbfs_name(const usbfs_volume *v, char *out, unsigned size);
/* A drive's name from its label alone, as usbfs_name: "" is "USB drive". */
void usbfs_label_name(const char *label, char *out, unsigned size);
/* The volume in v that "usb1p2" or "usb1p2:/x" is on: its index, or -1. */
int  usbfs_find(const usbfs_volume *v, int n, const char *device_path);

/* FatFs's logical drive for a mounted device ("usb1p2" or "usb1p2:..." -> 1,
 * opened as "1:/..."); -1 when that partition is not mounted. */
int  usbfs_volume_index(const char *device);

/* "usb1p2:/Games/N64" -> "usb:1A2B3C4D/Games/N64" (serial of usb1p2, from the
 * n volumes in v); any other path ("sd:/x") is copied unchanged. 0, or -1 when
 * the device is not among v or out is too small. */
int  usbfs_store_path(const usbfs_volume *v, int n, const char *device_path, char *out, unsigned size);
/* The reverse, for the volumes in v; -1 when no volume has that serial (the
 * drive is not plugged in), the serial is malformed or out is too small. When
 * two volumes share a serial, the first in v wins. For both, out must not be
 * the path given. */
int  usbfs_device_path(const usbfs_volume *v, int n, const char *stored, char *out, unsigned size);

/* "sd:/Games/N64" as the SD card's own path, under sd (where the console
 * mounts it, "fs:/vol/external01"): "fs:/vol/external01/Games/N64"; "sd:/" is
 * sd itself. 0, or -1 when path does not start "sd:/" or out is too small. */
int  usbfs_sd_path(const char *sd, const char *path, char *out, unsigned size);
/* settings.cfg's ROM folder as the folder Our ROMs opens: "" is "<root>/roms",
 * "sd:/x" goes through usbfs_sd_path and "usb:<serial>/x" through
 * usbfs_device_path with the n volumes in v. 0, or -1 when the folder can't be
 * reached: its drive is not among v, the value is malformed, or out is too small. */
int  usbfs_rom_folder(const char *folder, const char *root, const char *sd, const usbfs_volume *v, int n,
                      char *out, unsigned size);

/* settings.cfg's ROM folder as a full path, for people: "" is "SD card:
 * /wiiu/our64/roms", "sd:/Games" "SD card: /Games", "usb:<serial>/ROMs/N64"
 * "USB drive OUR64 EXFAT: /ROMs/N64", named by the partition among v with that
 * serial (plugged in), else by drive_label (settings.cfg's "ROM drive", the
 * label when the folder was picked; NULL or "": "USB drive: /ROMs/N64"). Cut
 * to size between UTF-8 characters. */
void usbfs_folder_label(const char *folder, const char *drive_label, const usbfs_volume *v, int n,
                        char *out, unsigned size);

#endif
