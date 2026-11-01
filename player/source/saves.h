/* Our64 player: in-game saves (EEPROM, SRAM, Flash, Controller Pak) on the SD.
 * The parts here are plain C with stdio, so tests/ runs them on the PC. */
#ifndef OUR64_SAVES_H
#define OUR64_SAVES_H
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* A save file written so a power cut never leaves it half written: the data
 * goes to <path>.tmp, the old file becomes <path>.bak, then .tmp becomes the
 * file. 0 on success; on failure the old file is untouched. */
int savefile_write(const char *path, const void *data, unsigned size);

/* Reads the newest complete copy (exactly size bytes) into data: the file,
 * else a complete .tmp left by a power cut between the two renames, else the
 * .bak. Says which one; SAVEFILE_NONE leaves data untouched. */
enum { SAVEFILE_NONE, SAVEFILE_MAIN, SAVEFILE_TMP, SAVEFILE_BAK };
int savefile_read(const char *path, void *data, unsigned size);

/* "<header name>-<CRC1>", e.g. "SUPER MARIO 64-635A2BFF": saves follow the
 * ROM's contents, not its file name. name is the header's 20 bytes. */
void saves_game_id(char *out, unsigned size, const unsigned char *name, uint32_t crc1);

/* When a changed save is written: 1 s after the game last changed it, or
 * 5 s after the first unsaved change if the game keeps changing it. */
typedef struct {
	int dirty;
	double first_us, last_us;
} saves_timer;
int saves_due(saves_timer *t, int changed, double now_us); /* 1: write it now */
int saves_take(saves_timer *t);                            /* 1: there was an unsaved change; now clear */

#ifdef __cplusplus
}
#endif
#endif
