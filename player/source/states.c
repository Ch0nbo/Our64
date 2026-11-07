/* Our64 player: save state files and resume points (see states.h). */
#include "states.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

#define HEADER    64
#define HEADER_V3 32
#define LEVEL  1 /* zlib's fastest: RDRAM is mostly zeros and repeats, and the writer runs on a spare core */

static void put32(unsigned char *p, uint32_t v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }
static uint32_t get32(const unsigned char *p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }

unsigned statefile_work_size(unsigned raw_size)
{
	return HEADER + (unsigned)compressBound(raw_size);
}

int statefile_pack(const state_info *info, const void *raw, unsigned raw_size, int thumb, void *work,
                   unsigned work_size, unsigned *bytes)
{
	unsigned char *w = work;
	if (work_size < statefile_work_size(raw_size))
		return STATEFILE_WRITE_FAILED;
	uLongf packed = work_size - HEADER;
	if (compress2(w + HEADER, &packed, raw, raw_size, LEVEL) != Z_OK)
		return STATEFILE_WRITE_FAILED;
	memset(w, 0, HEADER);
	memcpy(w, "O64S", 4);
	put32(w + 4, STATEFILE_VERSION);
	put32(w + 8, info->crc1);
	put32(w + 12, info->crc2);
	put32(w + 16, info->counter);
	put32(w + 20, raw_size);
	put32(w + 24, (uint32_t)packed);
	put32(w + 28, (uint32_t)crc32(0, w + HEADER, (uInt)packed));
	put32(w + 32, info->saved_at);
	put32(w + 36, thumb ? THUMB_BYTES : 0);
	w[40] = info->slot;
	w[41] = info->before;
	*bytes = HEADER + (unsigned)packed;
	return STATEFILE_OK;
}

int statefile_store(const char *path, const void *work, unsigned bytes, const void *thumb)
{
	const unsigned char *w = work;
	FILE *f = fopen(path, "wb");
	if (!f)
		return STATEFILE_WRITE_FAILED;
	size_t n = fwrite(w, 1, HEADER, f);
	if (thumb)
		n += fwrite(thumb, 1, THUMB_BYTES, f);
	n += fwrite(w + HEADER, 1, bytes - HEADER, f);
	if (fclose(f) != 0 || n != bytes + (thumb ? THUMB_BYTES : 0)) {
		remove(path);
		return STATEFILE_WRITE_FAILED;
	}
	return STATEFILE_OK;
}

/* Over the file's own clusters, so the card's free space is never searched:
 * on a nearly full card that search made each write take seconds. A file
 * too small for bytes is written anew with a quarter to spare, zeros after
 * the state (the header says where it ends). */
int statefile_store_over(const char *path, const void *work, unsigned bytes)
{
	static const unsigned char zeros[4096];
	FILE *f = fopen(path, "r+b");
	unsigned room = 0;
	if (f && (fseek(f, 0, SEEK_END) || ftell(f) < (long)bytes || fseek(f, 0, SEEK_SET))) {
		fclose(f);
		f = NULL;
	}
	if (!f) {
		f = fopen(path, "wb");
		if (!f)
			return STATEFILE_WRITE_FAILED;
		room = (bytes + bytes / 4 + 0xFFFF) & ~0xFFFFu;
	}
	size_t n = fwrite(work, 1, bytes, f);
	for (unsigned at = bytes; n == bytes && at < room; at += sizeof(zeros)) {
		unsigned k = room - at < sizeof(zeros) ? room - at : sizeof(zeros);
		if (fwrite(zeros, 1, k, f) != k)
			n = 0;
	}
	if (fclose(f) != 0 || n != bytes) {
		remove(path);
		return STATEFILE_WRITE_FAILED;
	}
	return STATEFILE_OK;
}

int statefile_write(const char *path, const state_info *info, const void *raw, unsigned raw_size,
                    const void *thumb, void *work, unsigned work_size)
{
	unsigned bytes;
	int r = statefile_pack(info, raw, raw_size, thumb != NULL, work, work_size, &bytes);
	return r == STATEFILE_OK ? statefile_store(path, work, bytes, thumb) : r;
}

/* The header alone: fills info and the sizes, or says why the file is no
 * use. Leaves the file at the thumbnail (version 4) or the packed state. */
static int read_header(FILE *f, uint32_t crc1, uint32_t crc2, state_info *info, uint32_t *raw_size,
                       uint32_t *packed, uint32_t *crc, uint32_t *thumb_bytes)
{
	unsigned char h[HEADER];
	memset(info, 0, sizeof(*info));
	if (fread(h, 1, HEADER_V3, f) != HEADER_V3 || memcmp(h, "O64S", 4))
		return STATEFILE_DAMAGED;
	info->version = get32(h + 4);
	if (info->version < STATEFILE_OLDEST || info->version > STATEFILE_VERSION)
		return STATEFILE_OTHER_VERSION;
	*thumb_bytes = 0;
	if (info->version >= 4) {
		if (fread(h + HEADER_V3, 1, HEADER - HEADER_V3, f) != HEADER - HEADER_V3)
			return STATEFILE_DAMAGED;
		info->saved_at = get32(h + 32);
		*thumb_bytes = get32(h + 36);
		if (*thumb_bytes != 0 && *thumb_bytes != THUMB_BYTES)
			return STATEFILE_DAMAGED;
		info->has_thumb = *thumb_bytes != 0;
		info->slot = h[40];
		info->before = h[41];
	}
	info->crc1 = get32(h + 8);
	info->crc2 = get32(h + 12);
	info->counter = get32(h + 16);
	*raw_size = get32(h + 20);
	*packed = get32(h + 24);
	*crc = get32(h + 28);
	if (info->crc1 != crc1 || info->crc2 != crc2)
		return STATEFILE_OTHER_GAME;
	return STATEFILE_OK;
}

static long header_size(const state_info *info) { return info->version >= 4 ? HEADER : HEADER_V3; }

int statefile_peek(const char *path, uint32_t crc1, uint32_t crc2, state_info *info, void *thumb)
{
	FILE *f = fopen(path, "rb");
	if (!f) {
		memset(info, 0, sizeof(*info));
		return STATEFILE_MISSING;
	}
	uint32_t size, packed, crc, thumb_bytes;
	int r = read_header(f, crc1, crc2, info, &size, &packed, &crc, &thumb_bytes);
	if (r == STATEFILE_OK && thumb_bytes && thumb && fread(thumb, 1, thumb_bytes, f) != thumb_bytes)
		r = STATEFILE_DAMAGED;
	if (r == STATEFILE_OK || r == STATEFILE_OTHER_GAME) {
		long want = header_size(info) + (long)thumb_bytes + (long)packed;
		if (fseek(f, 0, SEEK_END) || ftell(f) < want) /* resume points keep room after the state */
			r = STATEFILE_DAMAGED;
	}
	fclose(f);
	return r;
}

int statefile_read(const char *path, uint32_t crc1, uint32_t crc2, state_info *info,
                   void *raw, unsigned raw_cap, unsigned *raw_size)
{
	FILE *f = fopen(path, "rb");
	if (!f) {
		memset(info, 0, sizeof(*info));
		return STATEFILE_MISSING;
	}
	uint32_t size, packed, crc, thumb_bytes;
	int r = read_header(f, crc1, crc2, info, &size, &packed, &crc, &thumb_bytes);
	if (r == STATEFILE_OK && size > raw_cap)
		r = STATEFILE_TOO_BIG;
	if (r == STATEFILE_OK && thumb_bytes && fseek(f, thumb_bytes, SEEK_CUR))
		r = STATEFILE_DAMAGED;
	unsigned char *p = NULL;
	if (r == STATEFILE_OK) {
		p = malloc(packed ? packed : 1);
		r = !p || fread(p, 1, packed, f) != packed ? STATEFILE_DAMAGED : STATEFILE_OK;
	}
	fclose(f);
	if (r == STATEFILE_OK && (uint32_t)crc32(0, p, packed) != crc)
		r = STATEFILE_DAMAGED;
	if (r == STATEFILE_OK) {
		uLongf out = raw_cap;
		if (uncompress(raw, &out, p, packed) != Z_OK || out != size)
			r = STATEFILE_DAMAGED;
		else
			*raw_size = size;
	}
	free(p);
	return r;
}

/* ---- save state cards ---- */

static const char *const g_card_file[CARDS] = { "slot1.o64s", "slot2.o64s", "slot3.o64s", "undo.o64s" };

const char *card_file(int card) { return card >= 0 && card < CARDS ? g_card_file[card] : "none.o64s"; }

/* A slot's copy moved to the Undo card keeps its slot number and no kind. */
int card_undo_kind(const state_info *info)
{
	if (info->before == UNDO_LOAD || info->before == UNDO_RESET)
		return info->before;
	return UNDO_SAVE;
}

static void card_path(char *out, unsigned size, const char *dir, int card, const char *tail)
{
	snprintf(out, size, "%s/%s%s", dir, card_file(card), tail);
}

static int exists(const char *path)
{
	FILE *f = fopen(path, "rb");
	if (f)
		fclose(f);
	return f != NULL;
}

int card_save(const char *dir, int card, const state_info *info, int keep_old, const void *raw, unsigned raw_size,
              const void *thumb, void *work, unsigned work_size)
{
	char path[384], tmp[384], backup[400];
	card_path(path, sizeof(path), dir, card, "");
	card_path(tmp, sizeof(tmp), dir, card, ".tmp");
	if (statefile_write(tmp, info, raw, raw_size, thumb, work, work_size) != STATEFILE_OK) {
		remove(tmp);
		return STATEFILE_WRITE_FAILED;
	}
	/* The copy being replaced goes aside first (<file>.old, and the Undo card's
	 * own when a slot's copy is about to take its place), so a failure puts
	 * everything back as it was. */
	char old[400], undo_old[400];
	int kept = 0, aside = 0, undo_aside = 0;
	card_path(backup, sizeof(backup), dir, CARD_UNDO, "");
	card_path(undo_old, sizeof(undo_old), dir, CARD_UNDO, ".old");
	snprintf(old, sizeof(old), "%s.old", path);
	if (exists(path)) {
		if (card < CARD_SLOTS && keep_old) {
			remove(undo_old);
			if (exists(backup)) {
				if (rename(backup, undo_old)) {
					remove(tmp);
					return STATEFILE_BACKUP_FAILED;
				}
				undo_aside = 1;
			}
			if (rename(path, backup)) {
				if (undo_aside)
					rename(undo_old, backup);
				remove(tmp);
				return STATEFILE_BACKUP_FAILED;
			}
			kept = 1;
		} else {
			remove(old);
			if (rename(path, old)) {
				remove(tmp);
				return STATEFILE_WRITE_FAILED;
			}
			aside = 1;
		}
	}
	if (rename(tmp, path)) {
		if (kept) {
			rename(backup, path);
			if (undo_aside)
				rename(undo_old, backup);
		}
		if (aside)
			rename(old, path);
		remove(tmp);
		return STATEFILE_WRITE_FAILED;
	}
	remove(old);
	remove(undo_old);
	return STATEFILE_OK;
}

int card_peek(const char *dir, int card, uint32_t crc1, uint32_t crc2, state_info *info, void *thumb)
{
	char path[384], tmp[384];
	card_path(path, sizeof(path), dir, card, "");
	int r = statefile_peek(path, crc1, crc2, info, thumb);
	if (r != STATEFILE_MISSING)
		return r;
	card_path(tmp, sizeof(tmp), dir, card, ".tmp");
	r = statefile_peek(tmp, crc1, crc2, info, thumb);
	if (r == STATEFILE_MISSING)
		return r;
	if (r == STATEFILE_DAMAGED || rename(tmp, path)) {
		remove(tmp);
		memset(info, 0, sizeof(*info));
		return STATEFILE_MISSING;
	}
	return r;
}

int card_read(const char *dir, int card, uint32_t crc1, uint32_t crc2, state_info *info, void *raw,
              unsigned raw_cap, unsigned *raw_size)
{
	char path[384];
	card_path(path, sizeof(path), dir, card, "");
	return statefile_read(path, crc1, crc2, info, raw, raw_cap, raw_size);
}

int card_delete(const char *dir, int card)
{
	char path[384];
	card_path(path, sizeof(path), dir, card, "");
	remove(path);
	return exists(path) ? -1 : 0;
}

/* ---- resume points ---- */

/* The counter in a file's header, if the file is this game's. */
static int peek_counter(const char *path, uint32_t crc1, uint32_t crc2, uint32_t *counter)
{
	FILE *f = fopen(path, "rb");
	if (!f)
		return STATEFILE_MISSING;
	state_info info;
	uint32_t size, packed, crc, thumb_bytes;
	int r = read_header(f, crc1, crc2, &info, &size, &packed, &crc, &thumb_bytes);
	fclose(f);
	*counter = info.counter;
	return r;
}

int resume_load(const char *a, const char *b, uint32_t crc1, uint32_t crc2, resume_plan *plan,
                void *raw, unsigned raw_cap, unsigned *raw_size)
{
	const char *path[2] = { a, b };
	uint32_t counter[2] = { 0, 0 };
	int ok[2];
	uint32_t highest = 0;
	for (int i = 0; i < 2; i++) {
		ok[i] = peek_counter(path[i], crc1, crc2, &counter[i]) == STATEFILE_OK;
		if (ok[i] && counter[i] > highest)
			highest = counter[i];
	}
	plan->next_counter = highest + 1;
	plan->loaded_file = -1;
	plan->next_file = 0;
	/* Newest first; a file that fails the full read gives way to the other. */
	int first = ok[1] && (!ok[0] || counter[1] > counter[0]) ? 1 : 0;
	int result = STATEFILE_MISSING;
	for (int k = 0; k < 2; k++) {
		int i = k ? 1 - first : first;
		if (!ok[i])
			continue;
		state_info info;
		int r = statefile_read(path[i], crc1, crc2, &info, raw, raw_cap, raw_size);
		if (r == STATEFILE_OK) {
			plan->loaded_file = i;
			plan->next_file = 1 - i;
			return STATEFILE_OK;
		}
		if (result == STATEFILE_MISSING)
			result = r;
	}
	return result;
}

void resume_forget(const char *a, const char *b)
{
	remove(a);
	remove(b);
}

int resume_point_file(const char *name)
{
	static const char pre[] = "resume-", post[] = ".unused.o64s";
	size_t n = strlen(name);
	if (!strcmp(name, "resume-a.o64s") || !strcmp(name, "resume-b.o64s"))
		return 1;
	return n > sizeof(pre) - 1 + sizeof(post) - 1 && !strncmp(name, pre, sizeof(pre) - 1) &&
	       !strcmp(name + n - (sizeof(post) - 1), post);
}

/* The names are gathered first and the folder closed before any is removed:
 * nothing is deleted under an open directory listing. */
int resume_forget_all(const char *root, void (*each)(int done, int total, void *ctx), void *ctx)
{
	char top[300], dir[560], file[600];
	snprintf(top, sizeof(top), "%s/states", root);
	DIR *d = opendir(top);
	if (!d)
		return 0;
	int total = 0, done = 0;
	if (each) { /* the folders first, so the screen can say "12 of 94" */
		for (struct dirent *e; (e = readdir(d));)
			total += e->d_name[0] != '.';
		rewinddir(d);
		each(0, total, ctx);
	}
	int deleted = 0;
	for (struct dirent *e; (e = readdir(d));) {
		if (e->d_name[0] == '.')
			continue;
		if (each)
			each(++done < total ? done : total, total, ctx);
		snprintf(dir, sizeof(dir), "%s/%s", top, e->d_name);
		DIR *g = opendir(dir); /* a file, not a game's folder: NULL */
		if (!g)
			continue;
		char names[8][32];
		int k = 0;
		for (struct dirent *f; (f = readdir(g)) && k < 8;) {
			const size_t n = strlen(f->d_name);
			if (resume_point_file(f->d_name) && n < sizeof(names[0]))
				memcpy(names[k++], f->d_name, n + 1);
		}
		closedir(g);
		for (int i = 0; i < k; i++) {
			snprintf(file, sizeof(file), "%s/%.31s", dir, names[i]);
			deleted += remove(file) == 0;
		}
	}
	closedir(d);
	return deleted;
}

/* ---- when to capture ---- */

#define EVERY_US     60e6
#define SAVE_WAIT_US 5e6
#define FIRST_COST_US 10000.0 /* replaced by the first measurement */

void resume_timer_init(resume_timer *t, double now_us)
{
	t->last_us = now_us;
	t->cost_us = FIRST_COST_US;
}

int resume_decide(const resume_timer *t, double now_us, double save_since_us, int writer_busy)
{
	int save_waiting = save_since_us >= 0;
	if (save_waiting && now_us - save_since_us >= SAVE_WAIT_US)
		return RESUME_GIVE_UP;
	if (writer_busy || !(save_waiting || now_us - t->last_us >= EVERY_US))
		return RESUME_WAIT;
	return RESUME_DUE;
}

int resume_fits(const resume_timer *t, double idle_us)
{
	return idle_us >= 1.2 * t->cost_us;
}

void resume_captured(resume_timer *t, double now_us, double cost_us)
{
	t->last_us = now_us;
	t->cost_us = cost_us;
}
