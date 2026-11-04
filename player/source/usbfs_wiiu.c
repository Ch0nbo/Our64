/* Our64 player: USB drives on the console (usbfs_wiiu.h).
 *
 * Sectors: FSA's raw device commands (wut's coreinit/filesystem_fsa.h:
 * FSA_COMMAND_RAW_OPEN, _RAW_READ, _RAW_CLOSE and their request and response
 * structs), sent with IOS_Ioctl and IOS_Ioctlv on a client Mocha unlocked,
 * since wut does not export the FSA shim's own sender. Request, response and
 * vectors sit in one 64-byte aligned FSAShimBuffer; IOS writes the sectors
 * straight into FatFs's buffer when that is 64-byte aligned, else into an
 * aligned bounce buffer first.
 *
 * Files: one newlib device per mounted partition. FatFs has no locks of its
 * own (FF_FS_REENTRANT 0), so g_lock covers every FatFs call, from any
 * thread. A device stays registered from its first mount on, through rescans
 * and usbfs_stop: newlib's RemoveDevice empties the device's slot, and newlib
 * reads an open file's slot without checking it, so a thread still reading
 * (Our ROMs' loader is never joined) would crash. Instead every call checks,
 * under g_lock, that its partition is mounted now (else ENODEV) and that a
 * file or folder was opened in this scan (else EIO): each holds the scan's
 * number from when it opened, and usbfs_stop starts a new one. */
#include "usbfs_wiiu.h"
#include "mocha_wiiu.h"
#include "report.h"
#include "ff.h"
#include "diskio.h"

#include <coreinit/cache.h>
#include <coreinit/filesystem_fsa.h>
#include <coreinit/ios.h>
#include <coreinit/mutex.h>
#include <errno.h>
#include <fcntl.h>
#include <malloc.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/iosupport.h>
#include <sys/stat.h>
#include <sys/syslimits.h>

#define USBFS_VOLUMES (USBFS_DRIVES * USBFS_PARTS)
#define CHUNK_SECTORS 128         /* the most sectors one raw read asks for */
#define BOUNCE_BYTES (64 * 1024)  /* 128 sectors of 512 bytes, 16 of 4096 */

typedef struct {
	int open;
	int32_t handle;   /* FSA's raw device handle */
	uint32_t ss;      /* sector size: 512 or 4096 */
	uint64_t sectors; /* 0: not known */
} raw_drive;

static const char *const g_drive_path[USBFS_DRIVES] = { "/dev/usb01", "/dev/usb02" };

static OSMutex g_lock;
static int g_lock_ready;
static int g_started;
static int g_why = USBFS_READY;
static FSAClientHandle g_client = -1;
static FSAShimBuffer *g_shim;  /* 64-byte aligned */
static uint8_t *g_bounce;      /* 64-byte aligned, BOUNCE_BYTES */
static raw_drive g_drive[USBFS_DRIVES];
static usbfs_volume g_vols[USBFS_VOLUMES];
static int g_nvols;
static unsigned g_scan;        /* counts scans: files from an earlier one fail */

/* ---- raw sectors ---- */

static void shim_clear(void)
{
	memset(&g_shim->request, 0, sizeof(g_shim->request));
	memset(&g_shim->response, 0, sizeof(g_shim->response));
}

/* An ioctl with the shim's request and response, as FSA's own shim sends them. */
static int shim_ioctl(uint32_t command)
{
	DCFlushRange(&g_shim->request, sizeof(g_shim->request));
	DCFlushRange(&g_shim->response, sizeof(g_shim->response));
	int r = IOS_Ioctl(g_client, command, &g_shim->request, sizeof(g_shim->request), &g_shim->response,
	                  sizeof(g_shim->response));
	DCInvalidateRange(&g_shim->response, sizeof(g_shim->response));
	return r;
}

static void raw_close(int d)
{
	raw_drive *r = &g_drive[d];
	if (!r->open)
		return;
	shim_clear();
	g_shim->request.rawClose.handle = r->handle;
	int e = shim_ioctl(FSA_COMMAND_RAW_CLOSE);
	if (e < 0)
		report_line("USB: %s did not close (%d)", g_drive_path[d], e);
	memset(r, 0, sizeof(*r));
}

/* 0 when the drive opened with a sector size FatFs reads. */
static int raw_open(int d)
{
	raw_drive *r = &g_drive[d];
	memset(r, 0, sizeof(*r));
	shim_clear();
	snprintf(g_shim->request.rawOpen.path, sizeof(g_shim->request.rawOpen.path), "%s", g_drive_path[d]);
	int e = shim_ioctl(FSA_COMMAND_RAW_OPEN);
	if (e < 0)
		return -1; /* nothing plugged in there, most often */
	r->open = 1;
	r->handle = g_shim->response.rawOpen.handle;
	FSADeviceInfo info __attribute__((aligned(0x40)));
	memset(&info, 0, sizeof(info));
	FSError fe = FSAGetDeviceInfo(g_client, g_drive_path[d], &info);
	if (fe == FS_ERROR_OK) {
		r->ss = info.deviceSectorSize;
		r->sectors = info.deviceSizeInSectors;
	} else {
		/* no size known: 512-byte sectors, and a drive of 4096 fails its first read */
		report_line("USB: %s has no device info (%d): reading it as 512-byte sectors", g_drive_path[d], (int)fe);
		r->ss = 512;
		r->sectors = 0;
	}
	if (r->ss != 512 && r->ss != 4096) {
		report_line("USB: %s has %u-byte sectors: not read", g_drive_path[d], (unsigned)r->ss);
		raw_close(d);
		return -1;
	}
	report_line("USB: %s opened, %llu sectors of %u bytes", g_drive_path[d], (unsigned long long)r->sectors,
	            (unsigned)r->ss);
	return 0;
}

/* count sectors (at most CHUNK_SECTORS) into data, 64-byte aligned and
 * count * ss long, which is whole cache lines. */
static int raw_read(int d, void *data, uint64_t sector, uint32_t count)
{
	raw_drive *r = &g_drive[d];
	const uint32_t len = count * r->ss;
	/* the vectors sit at 0x880 in the 64-byte aligned shim, so 4-byte alignment holds */
	IOSVec *vec = (IOSVec *)((uint8_t *)g_shim + offsetof(FSAShimBuffer, ioctlvVec));
	shim_clear();
	g_shim->request.rawRead.blocks_offset = sector;
	g_shim->request.rawRead.count = count;
	g_shim->request.rawRead.size = r->ss;
	g_shim->request.rawRead.device_handle = (uint32_t)r->handle;
	vec[0].vaddr = &g_shim->request;
	vec[0].len = sizeof(g_shim->request);
	vec[1].vaddr = data;
	vec[1].len = len;
	vec[2].vaddr = &g_shim->response;
	vec[2].len = sizeof(g_shim->response);
	vec[0].paddr = vec[1].paddr = vec[2].paddr = NULL; /* IOS_Ioctlv fills them */
	DCFlushRange(&g_shim->request, sizeof(g_shim->request));
	DCFlushRange(&g_shim->response, sizeof(g_shim->response));
	DCFlushRange(data, len); /* no dirty line may land on the sectors after IOS wrote them */
	int e = IOS_Ioctlv(g_client, FSA_COMMAND_RAW_READ, 1, 2, vec);
	DCInvalidateRange(data, len);
	DCInvalidateRange(&g_shim->response, sizeof(g_shim->response));
	if (e < 0) {
		report_line("USB: %s read of %u sectors at %llu failed (%d)", g_drive_path[d], (unsigned)count,
		            (unsigned long long)sector, e);
		return -1;
	}
	return 0;
}

/* ---- FatFs's disk functions: physical drive 0 is /dev/usb01, 1 /dev/usb02 ---- */

DSTATUS disk_status(BYTE pdrv)
{
	return pdrv < USBFS_DRIVES && g_drive[pdrv].open ? 0 : STA_NOINIT | STA_NODISK;
}

/* usbfs_rescan opens the drives before FatFs mounts them */
DSTATUS disk_initialize(BYTE pdrv)
{
	return disk_status(pdrv);
}

DRESULT disk_read(BYTE pdrv, BYTE *buff, LBA_t sector, UINT count)
{
	if (disk_status(pdrv))
		return RES_NOTRDY;
	const raw_drive *r = &g_drive[pdrv];
	if (r->sectors && (sector >= r->sectors || count > r->sectors - sector))
		return RES_PARERR;
	while (count) {
		const int direct = ((uintptr_t)buff & 0x3F) == 0;
		uint32_t n = direct ? CHUNK_SECTORS : BOUNCE_BYTES / r->ss;
		if (n > count)
			n = count;
		if (raw_read(pdrv, direct ? buff : g_bounce, sector, n))
			return RES_ERROR;
		if (!direct)
			memcpy(buff, g_bounce, (size_t)n * r->ss);
		buff += (size_t)n * r->ss;
		sector += n;
		count -= n;
	}
	return RES_OK;
}

DRESULT disk_ioctl(BYTE pdrv, BYTE cmd, void *buff)
{
	if (disk_status(pdrv))
		return RES_NOTRDY;
	switch (cmd) {
	case CTRL_SYNC: /* nothing is written */
		return RES_OK;
	case GET_SECTOR_COUNT:
		*(LBA_t *)buff = (LBA_t)g_drive[pdrv].sectors;
		return RES_OK;
	case GET_SECTOR_SIZE:
		*(WORD *)buff = (WORD)g_drive[pdrv].ss;
		return RES_OK;
	case GET_BLOCK_SIZE:
		*(DWORD *)buff = 1;
		return RES_OK;
	}
	return RES_PARERR;
}

/* ---- the devices ---- */

typedef struct {
	FIL fil;
	int64_t at;   /* the position lseek gave, which may be past the end */
	unsigned scan;
	int vol;
} usb_file;

typedef struct {
	DIR dir;
	unsigned scan;
	int vol;
} usb_dir;

static devoptab_t g_tab[USBFS_VOLUMES];
static char g_name[USBFS_VOLUMES][8];  /* "usb1p2" */
static int g_vol_of[USBFS_VOLUMES];     /* each device's deviceData: its FatFs volume */
static int g_registered[USBFS_VOLUMES];

static int fat_errno(FRESULT fr)
{
	switch (fr) {
	case FR_OK: return 0;
	case FR_NO_FILE:
	case FR_NO_PATH: return ENOENT;
	case FR_INVALID_NAME: return EINVAL;
	case FR_DENIED: return EACCES;
	case FR_EXIST: return EEXIST;
	case FR_INVALID_OBJECT: return EBADF;
	case FR_WRITE_PROTECTED: return EROFS;
	case FR_INVALID_DRIVE:
	case FR_NOT_ENABLED:
	case FR_NO_FILESYSTEM: return ENODEV;
	case FR_TIMEOUT:
	case FR_LOCKED: return EBUSY;
	case FR_NOT_ENOUGH_CORE: return ENOMEM;
	case FR_TOO_MANY_OPEN_FILES: return EMFILE;
	case FR_INVALID_PARAMETER: return EINVAL;
	default: return EIO; /* FR_DISK_ERR, FR_INT_ERR, FR_NOT_READY, FR_MKFS_ABORTED */
	}
}

static int fail(struct _reent *r, int e)
{
	r->_errno = e;
	OSUnlockMutex(&g_lock);
	return -1;
}

/* The device's volume, still mounted (lock held); -1 with errno ENODEV. */
static int device_volume(struct _reent *r)
{
	const int vol = r->deviceData ? *(const int *)r->deviceData : -1;
	if (!g_started || vol < 0 || vol >= USBFS_VOLUMES || usbfs_volume_index(g_name[vol]) != vol) {
		r->_errno = ENODEV;
		return -1;
	}
	return vol;
}

/* "usb1p2:/Games/x" -> "1:/Games/x"; 0, or -1 with errno set (lock held).
 * *root: the path is the partition's root, which FatFs cannot stat. */
static int fat_path(struct _reent *r, const char *path, char *out, unsigned size, int *vol, int *root)
{
	if ((*vol = device_volume(r)) < 0)
		return -1;
	const char *colon = strchr(path, ':');
	const char *rest = colon ? colon + 1 : path;
	int n = snprintf(out, size, "%d:%s%s", *vol, rest[0] == '/' ? "" : "/", rest);
	if (n < 0 || (unsigned)n >= size) {
		r->_errno = ENAMETOOLONG;
		return -1;
	}
	if (root)
		*root = !rest[strspn(rest, "/")];
	return 0;
}

/* An object opened in this scan, on a volume still mounted (lock held). */
static int still_valid(int vol, unsigned scan)
{
	return g_started && scan == g_scan && usbfs_volume_index(g_name[vol]) == vol;
}

/* FAT's date and time (local, no zone) as seconds since 1970. */
static time_t fat_time(WORD date, WORD hms)
{
	int y = 1980 + (date >> 9), m = (date >> 5) & 15, d = date & 31;
	if (m < 1 || m > 12 || d < 1)
		return 0;
	/* days from 1970-01-01 to y-m-d (Howard Hinnant's days_from_civil) */
	y -= m <= 2;
	const int era = y / 400, yoe = y - era * 400;
	const int doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
	const int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
	const long days = (long)era * 146097 + doe - 719468;
	return (time_t)days * 86400 + (hms >> 11) * 3600 + ((hms >> 5) & 63) * 60 + (hms & 31) * 2;
}

static void fill_stat(const FILINFO *fno, uint32_t ss, struct stat *st)
{
	memset(st, 0, sizeof(*st));
	st->st_mode = fno->fattrib & AM_DIR ? S_IFDIR | 0555 : S_IFREG | 0444;
	st->st_nlink = 1;
	st->st_size = fno->fattrib & AM_DIR ? 0 : (off_t)fno->fsize;
	st->st_blksize = ss;
	st->st_blocks = (st->st_size + 511) / 512;
	st->st_mtime = st->st_atime = st->st_ctime = fat_time(fno->fdate, fno->ftime);
}

static uint32_t volume_ss(int vol) { return g_drive[vol / USBFS_PARTS].ss; }

static int dev_open(struct _reent *r, void *fileStruct, const char *path, int flags, int mode)
{
	(void)mode;
	usb_file *f = fileStruct;
	if ((flags & O_ACCMODE) != O_RDONLY || (flags & O_TRUNC)) {
		r->_errno = EROFS;
		return -1;
	}
	char p[600];
	int vol, root;
	OSLockMutex(&g_lock);
	if (fat_path(r, path, p, sizeof(p), &vol, &root))
		return fail(r, r->_errno);
	if (root)
		return fail(r, EISDIR);
	FRESULT fr = f_open(&f->fil, p, FA_READ);
	if (fr == FR_NO_FILE) { /* FatFs says the same for a folder */
		FILINFO fno;
		if (f_stat(p, &fno) == FR_OK && (fno.fattrib & AM_DIR))
			return fail(r, EISDIR);
	}
	if (fr != FR_OK)
		return fail(r, fat_errno(fr));
	f->at = 0;
	f->scan = g_scan;
	f->vol = vol;
	OSUnlockMutex(&g_lock);
	return 0;
}

static int dev_close(struct _reent *r, void *fd)
{
	(void)r;
	usb_file *f = fd;
	OSLockMutex(&g_lock);
	if (still_valid(f->vol, f->scan))
		f_close(&f->fil);
	f->scan = ~0u;
	OSUnlockMutex(&g_lock);
	return 0; /* a file from an earlier scan has nothing left to close */
}

static ssize_t dev_read(struct _reent *r, void *fd, char *ptr, size_t len)
{
	usb_file *f = fd;
	UINT got = 0;
	OSLockMutex(&g_lock);
	if (!still_valid(f->vol, f->scan))
		return fail(r, EIO);
	if (f->at >= (int64_t)f_size(&f->fil)) { /* at or past the end */
		OSUnlockMutex(&g_lock);
		return 0;
	}
	FRESULT fr = f_read(&f->fil, ptr, (UINT)len, &got);
	f->at = (int64_t)f_tell(&f->fil);
	if (fr != FR_OK)
		return fail(r, fat_errno(fr));
	OSUnlockMutex(&g_lock);
	return (ssize_t)got;
}

static off_t dev_seek(struct _reent *r, void *fd, off_t pos, int dir)
{
	usb_file *f = fd;
	OSLockMutex(&g_lock);
	if (!still_valid(f->vol, f->scan))
		return fail(r, EIO);
	int64_t base;
	switch (dir) {
	case SEEK_SET: base = 0; break;
	case SEEK_CUR: base = f->at; break;
	case SEEK_END: base = (int64_t)f_size(&f->fil); break;
	default: return fail(r, EINVAL);
	}
	const int64_t at = base + (int64_t)pos;
	if (at < 0)
		return fail(r, EINVAL);
	/* read-only FatFs stops a seek at the end; lseek allows past it, where reads give 0 */
	const int64_t size = (int64_t)f_size(&f->fil);
	FRESULT fr = f_lseek(&f->fil, (FSIZE_t)(at < size ? at : size));
	if (fr != FR_OK)
		return fail(r, fat_errno(fr));
	f->at = at;
	OSUnlockMutex(&g_lock);
	return (off_t)at;
}

static int dev_fstat(struct _reent *r, void *fd, struct stat *st)
{
	usb_file *f = fd;
	OSLockMutex(&g_lock);
	if (!still_valid(f->vol, f->scan))
		return fail(r, EIO);
	FILINFO fno;
	memset(&fno, 0, sizeof(fno));
	fno.fsize = f_size(&f->fil);
	fill_stat(&fno, volume_ss(f->vol), st);
	OSUnlockMutex(&g_lock);
	return 0;
}

static int dev_stat(struct _reent *r, const char *file, struct stat *st)
{
	char p[600];
	int vol, root;
	OSLockMutex(&g_lock);
	if (fat_path(r, file, p, sizeof(p), &vol, &root))
		return fail(r, r->_errno);
	FILINFO fno;
	memset(&fno, 0, sizeof(fno));
	if (root) {
		fno.fattrib = AM_DIR;
	} else {
		FRESULT fr = f_stat(p, &fno);
		if (fr != FR_OK)
			return fail(r, fat_errno(fr));
	}
	fill_stat(&fno, volume_ss(vol), st);
	OSUnlockMutex(&g_lock);
	return 0;
}

static DIR_ITER *dev_diropen(struct _reent *r, DIR_ITER *dirState, const char *path)
{
	usb_dir *d = dirState->dirStruct;
	char p[600];
	int vol;
	OSLockMutex(&g_lock);
	if (fat_path(r, path, p, sizeof(p), &vol, NULL)) {
		fail(r, r->_errno);
		return NULL;
	}
	FRESULT fr = f_opendir(&d->dir, p);
	if (fr == FR_NO_PATH) { /* a file there, or nothing */
		FILINFO fno;
		if (f_stat(p, &fno) == FR_OK && !(fno.fattrib & AM_DIR)) {
			fail(r, ENOTDIR);
			return NULL;
		}
	}
	if (fr != FR_OK) {
		fail(r, fat_errno(fr));
		return NULL;
	}
	d->scan = g_scan;
	d->vol = vol;
	OSUnlockMutex(&g_lock);
	return dirState;
}

/* filename: newlib's NAME_MAX + 1 bytes, as many as FatFs's longest name in UTF-8 */
static int dev_dirnext(struct _reent *r, DIR_ITER *dirState, char *filename, struct stat *st)
{
	usb_dir *d = dirState->dirStruct;
	OSLockMutex(&g_lock);
	if (!still_valid(d->vol, d->scan))
		return fail(r, EIO);
	FILINFO fno;
	FRESULT fr = f_readdir(&d->dir, &fno);
	if (fr != FR_OK)
		return fail(r, fat_errno(fr));
	if (!fno.fname[0])
		return fail(r, ENOENT); /* the end */
	snprintf(filename, NAME_MAX + 1, "%s", fno.fname);
	if (st)
		fill_stat(&fno, volume_ss(d->vol), st);
	OSUnlockMutex(&g_lock);
	return 0;
}

static int dev_dirclose(struct _reent *r, DIR_ITER *dirState)
{
	(void)r;
	usb_dir *d = dirState->dirStruct;
	OSLockMutex(&g_lock);
	if (still_valid(d->vol, d->scan))
		f_closedir(&d->dir);
	d->scan = ~0u;
	OSUnlockMutex(&g_lock);
	return 0;
}

/* The device for g_vols[i], once (lock held). */
static void register_device(const usbfs_volume *v)
{
	const int vol = (v->drive - 1) * USBFS_PARTS + (v->part - 1);
	if (g_registered[vol])
		return;
	usbfs_device(v, g_name[vol], sizeof(g_name[vol]));
	devoptab_t *t = &g_tab[vol];
	memset(t, 0, sizeof(*t));
	g_vol_of[vol] = vol;
	t->name = g_name[vol];
	t->structSize = sizeof(usb_file);
	t->open_r = dev_open;
	t->close_r = dev_close;
	t->read_r = dev_read;
	t->seek_r = dev_seek;
	t->fstat_r = dev_fstat;
	t->stat_r = dev_stat;
	t->dirStateSize = sizeof(usb_dir);
	t->diropen_r = dev_diropen;
	t->dirnext_r = dev_dirnext;
	t->dirclose_r = dev_dirclose;
	t->deviceData = &g_vol_of[vol];
	if (AddDevice(t) < 0)
		report_line("USB: no room for device %s", g_name[vol]);
	else
		g_registered[vol] = 1;
}

/* ---- start, rescan, stop ---- */

/* Lock held. */
static void close_all(void)
{
	g_scan++;
	g_nvols = 0;
	usbfs_unmount_all();
	for (int d = 0; d < USBFS_DRIVES; d++)
		raw_close(d);
}

/* Lock held, g_started. */
static void scan(void)
{
	close_all();
	if (g_client < 0)
		return;
	int opened = 0;
	for (int d = 0; d < USBFS_DRIVES; d++)
		opened += !raw_open(d);
	g_nvols = opened ? usbfs_mount_all(g_vols, USBFS_VOLUMES) : 0;
	for (int i = 0; i < g_nvols; i++) {
		char name[64], dev[8];
		register_device(&g_vols[i]);
		usbfs_name(&g_vols[i], name, sizeof(name));
		usbfs_device(&g_vols[i], dev, sizeof(dev));
		report_line("USB: %s:/ is %s (%s, serial %08X)", dev, name, g_vols[i].exfat ? "exFAT" : "FAT",
		            (unsigned)g_vols[i].serial);
	}
	report_line("USB: %d drive%s open, %d partition%s mounted", opened, opened == 1 ? "" : "s", g_nvols,
	            g_nvols == 1 ? "" : "s");
}

/* Lock held: the client and buffers, or g_why says what is missing. */
static void begin(void)
{
	g_started = 1;
	g_why = USBFS_READY;
	if (!mocha_wiiu_present()) {
		g_why = USBFS_NO_MOCHA;
		report_line("USB: Mocha did not answer: USB drives are off");
		return;
	}
	g_client = mocha_wiiu_fsa_client();
	if (g_client < 0) {
		g_why = USBFS_NO_CLIENT;
		report_line("USB: no unlocked FSA client: USB drives are off");
		return;
	}
	if (!g_shim)
		g_shim = memalign(0x40, sizeof(FSAShimBuffer));
	if (!g_bounce)
		g_bounce = memalign(0x40, BOUNCE_BYTES);
	if (!g_shim || !g_bounce) {
		g_why = USBFS_NO_MEMORY;
		report_line("USB: no memory for the sector buffers: USB drives are off");
		FSADelClient(g_client);
		g_client = -1;
	}
}

static void lock(void)
{
	if (!g_lock_ready) { /* the first start, before any device exists; stop before it does nothing */
		OSInitMutexEx(&g_lock, "Our64 USB");
		g_lock_ready = 1;
	}
	OSLockMutex(&g_lock);
}

int usbfs_start(void)
{
	lock();
	if (!g_started) {
		begin();
		scan();
	}
	const int n = g_nvols;
	OSUnlockMutex(&g_lock);
	return n;
}

int usbfs_rescan(void)
{
	lock();
	if (!g_started)
		begin();
	scan();
	const int n = g_nvols;
	OSUnlockMutex(&g_lock);
	return n;
}

const usbfs_volume *usbfs_volumes(int *n)
{
	*n = g_nvols;
	return g_vols;
}

int usbfs_drive_open(int drive)
{
	return drive >= 1 && drive <= USBFS_DRIVES && g_drive[drive - 1].open;
}

int usbfs_why(void) { return g_why; }

void usbfs_stop(void)
{
	if (!g_lock_ready)
		return;
	OSLockMutex(&g_lock);
	if (g_started) {
		g_started = 0; /* the devices stay registered and answer ENODEV and EIO from now on */
		close_all();
		if (g_client >= 0)
			FSADelClient(g_client);
		g_client = -1;
		free(g_shim);
		free(g_bounce);
		g_shim = NULL;
		g_bounce = NULL;
		g_why = USBFS_READY;
	}
	OSUnlockMutex(&g_lock);
}
