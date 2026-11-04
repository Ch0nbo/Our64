/* FatFs's disk functions on image files, for the PC tests: physical drive 0
 * or 1 reads the image given to usbfs_test_image (and writes it, in
 * make_disk's read-write FatFs build). Built once per FatFs configuration. */
#include "ff.h"
#include "diskio.h"

#include <stdio.h>
#include <sys/types.h>

int usbfs_test_image(int pdrv, const char *path, unsigned sector_size);

static FILE *g_img[2];
static unsigned g_ss[2];
static LBA_t g_count[2];

/* path NULL: no drive there. Returns 0, or -1 when the image does not open. */
int usbfs_test_image(int pdrv, const char *path, unsigned sector_size)
{
	if (pdrv < 0 || pdrv > 1)
		return -1;
	if (g_img[pdrv])
		fclose(g_img[pdrv]);
	g_img[pdrv] = NULL;
	if (!path)
		return 0;
	g_img[pdrv] = fopen(path, FF_FS_READONLY ? "rb" : "r+b");
	if (!g_img[pdrv])
		return -1;
	fseeko(g_img[pdrv], 0, SEEK_END);
	g_ss[pdrv] = sector_size;
	g_count[pdrv] = (LBA_t)ftello(g_img[pdrv]) / sector_size;
	return 0;
}

DSTATUS disk_status(BYTE pdrv)
{
	return pdrv < 2 && g_img[pdrv] ? 0 : STA_NOINIT | STA_NODISK;
}

DSTATUS disk_initialize(BYTE pdrv)
{
	return disk_status(pdrv);
}

DRESULT disk_read(BYTE pdrv, BYTE *buff, LBA_t sector, UINT count)
{
	if (disk_status(pdrv))
		return RES_NOTRDY;
	if (sector + count > g_count[pdrv])
		return RES_PARERR;
	FILE *f = g_img[pdrv];
	if (fseeko(f, (off_t)sector * g_ss[pdrv], SEEK_SET) != 0)
		return RES_ERROR;
	return fread(buff, g_ss[pdrv], count, f) == count ? RES_OK : RES_ERROR;
}

#if !FF_FS_READONLY
DRESULT disk_write(BYTE pdrv, const BYTE *buff, LBA_t sector, UINT count)
{
	if (disk_status(pdrv))
		return RES_NOTRDY;
	if (sector + count > g_count[pdrv])
		return RES_PARERR;
	FILE *f = g_img[pdrv];
	if (fseeko(f, (off_t)sector * g_ss[pdrv], SEEK_SET) != 0)
		return RES_ERROR;
	return fwrite(buff, g_ss[pdrv], count, f) == count ? RES_OK : RES_ERROR;
}
#endif

DRESULT disk_ioctl(BYTE pdrv, BYTE cmd, void *buff)
{
	if (disk_status(pdrv))
		return RES_NOTRDY;
	switch (cmd) {
	case CTRL_SYNC:
		return fflush(g_img[pdrv]) == 0 ? RES_OK : RES_ERROR;
	case GET_SECTOR_COUNT:
		*(LBA_t *)buff = g_count[pdrv];
		return RES_OK;
	case GET_SECTOR_SIZE:
		*(WORD *)buff = (WORD)g_ss[pdrv];
		return RES_OK;
	case GET_BLOCK_SIZE:
		*(DWORD *)buff = 1;
		return RES_OK;
	}
	return RES_PARERR;
}
