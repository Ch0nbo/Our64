/* Our64 player: USB drives on the console (usbfs_wiiu.c). Mocha's unlocked
 * FSA client reads /dev/usb01 and /dev/usb02 sector by sector, FatFs reads
 * their FAT32 and exFAT partitions (usbfs.h), and each mounted partition is a
 * newlib device "usb<D>p<P>:/", read-only, for fopen, opendir and stat.
 *
 * Start, rescan and stop run on one thread (the CPU core's menus), and the
 * volume list they fill is read on that thread only. The files themselves
 * may be read from any thread: one lock covers every FatFs call. */
#ifndef OUR64_USBFS_WIIU_H
#define OUR64_USBFS_WIIU_H

#include "usbfs.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Why nothing mounts, when nothing does */
enum { USBFS_READY, USBFS_NO_MOCHA, USBFS_NO_CLIENT, USBFS_NO_MEMORY };

/* Mounts every FAT32 and exFAT partition, once: later calls only return the
 * count. Without Mocha it mounts nothing (usbfs_why says so). */
int  usbfs_start(void);
/* Closes and opens the drives again, for drives plugged in or pulled since,
 * and mounts what is there now; starts first when not started. */
int  usbfs_rescan(void);
/* What is mounted now, n of them. */
const usbfs_volume *usbfs_volumes(int *n);
/* 1 when drive (1 or 2) opened in the last scan, mounted or not. */
int  usbfs_drive_open(int drive);
int  usbfs_why(void);
/* At exit: the devices go, FatFs unmounts, the drives and the client close. */
void usbfs_stop(void);

#ifdef __cplusplus
}
#endif
#endif
