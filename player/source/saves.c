/* Our64 player: the portable parts of in-game saves (see saves.h). */
#include "saves.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define QUIET_US   1000000.0 /* write 1 s after the last change */
#define LONGEST_US 5000000.0 /* or 5 s after the first, if the game keeps writing */

/* ---- save files ---- */

int savefile_write(const char *path, const void *data, unsigned size)
{
	char tmp[512], bak[512];
	snprintf(tmp, sizeof(tmp), "%s.tmp", path);
	snprintf(bak, sizeof(bak), "%s.bak", path);
	FILE *f = fopen(tmp, "wb");
	if (!f)
		return -1;
	size_t n = fwrite(data, 1, size, f);
	if (fclose(f) != 0 || n != size) {
		remove(tmp);
		return -1;
	}
	/* Renaming onto an existing file fails on the Wii U, so the old file
	 * moves out of the way first; until the last rename, it is the .bak. */
	FILE *old = fopen(path, "rb");
	if (old) {
		fclose(old);
		remove(bak);
		if (rename(path, bak)) {
			remove(tmp);
			return -1;
		}
	}
	if (rename(tmp, path)) {
		if (old)
			rename(bak, path);
		return -1;
	}
	return 0;
}

/* The whole file into a buffer of its own, only if it is exactly size bytes. */
static void *read_exact(const char *path, unsigned size)
{
	FILE *f = fopen(path, "rb");
	if (!f)
		return NULL;
	void *buf = NULL;
	if (!fseek(f, 0, SEEK_END) && ftell(f) == (long)size && !fseek(f, 0, SEEK_SET)) {
		buf = malloc(size ? size : 1);
		if (buf && fread(buf, 1, size, f) != size) {
			free(buf);
			buf = NULL;
		}
	}
	fclose(f);
	return buf;
}

int savefile_read(const char *path, void *data, unsigned size)
{
	static const char *const suffix[] = { "", ".tmp", ".bak" };
	static const int source[] = { SAVEFILE_MAIN, SAVEFILE_TMP, SAVEFILE_BAK };
	for (int i = 0; i < 3; i++) {
		char p[512];
		snprintf(p, sizeof(p), "%s%s", path, suffix[i]);
		void *buf = read_exact(p, size);
		if (buf) {
			memcpy(data, buf, size);
			free(buf);
			return source[i];
		}
	}
	return SAVEFILE_NONE;
}

/* ---- game ID ---- */

void saves_game_id(char *out, unsigned size, const unsigned char *name, uint32_t crc1)
{
	char clean[21];
	int n = 0;
	for (int i = 0; i < 20 && name[i]; i++) {
		unsigned char c = name[i];
		clean[n++] = c < 0x20 || c >= 0x7F || strchr("\\/:*?\"<>|", c) ? '_' : (char)c;
	}
	/* FAT drops trailing spaces and dots; leading spaces only look odd. */
	while (n > 0 && (clean[n - 1] == ' ' || clean[n - 1] == '.'))
		n--;
	clean[n] = 0;
	const char *start = clean;
	while (*start == ' ')
		start++;
	snprintf(out, size, "%s-%08X", *start ? start : "UNKNOWN", (unsigned)crc1);
}

/* ---- when to write ---- */

int saves_due(saves_timer *t, int changed, double now_us)
{
	if (changed) {
		if (!t->dirty)
			t->first_us = now_us;
		t->dirty = 1;
		t->last_us = now_us;
	}
	if (!t->dirty || (now_us - t->last_us < QUIET_US && now_us - t->first_us < LONGEST_US))
		return 0;
	t->dirty = 0;
	return 1;
}

int saves_take(saves_timer *t)
{
	int was = t->dirty;
	t->dirty = 0;
	return was;
}
