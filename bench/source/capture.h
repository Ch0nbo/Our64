/* Our64 bench: frame capture file format (.o64f), shared with the replay tools. */
#ifndef OUR64_CAPTURE_H
#define OUR64_CAPTURE_H
#include <stdint.h>

#define O64F_VERSION 1

typedef struct {
	char     magic[4];      /* "O64F" */
	uint32_t version;
	char     rom_name[20];  /* from the ROM header, space padded */
	uint32_t rom_crc[2];
	uint32_t vi_count;      /* VIs emulated before this task */
	uint32_t task_index;    /* graphics tasks before this one */
	uint32_t rdram_size;
	uint32_t vi[14];        /* VI registers 0x04400000-0x04400034 */
	uint32_t dpc[8];        /* start, end, current, status, clock, bufbusy, pipebusy, tmem */
	uint32_t sp_status;
	uint32_t mi_intr;
	uint32_t rdram_zsize;   /* bytes of zlib data after DMEM and IMEM */
	uint32_t reserved[5];
} o64f_header;

void capture_begin(const char *sd_root, const char *rom_file);
void capture_group(uint32_t vi); /* frames that follow are <rom>-v<vi>-<task>.o64f */
int  capture_frame(uint32_t vi, uint32_t task); /* 0 on success */
#endif
