/* PC tests for player/source/saves.c: save files that survive a power cut,
 * game IDs, and when a changed save is due to be written. */
#include "saves.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int g_failed, g_run;
static char g_dir[128];

#define CHECK(cond) do { if (!(cond)) { printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); g_failed++; return; } } while (0)

static void path(char *out, const char *name) { snprintf(out, 256, "%s/%s", g_dir, name); }

static void put(const char *name, const void *data, size_t n)
{
	char p[256];
	path(p, name);
	FILE *f = fopen(p, "wb");
	fwrite(data, 1, n, f);
	fclose(f);
}

static int exists(const char *name)
{
	char p[256];
	struct stat st;
	path(p, name);
	return !stat(p, &st);
}

static void fresh_dir(void)
{
	char cmd[600];
	snprintf(cmd, sizeof(cmd), "rm -rf %s && mkdir -p %s", g_dir, g_dir);
	if (system(cmd)) exit(2);
}

/* ---- savefile ---- */

static void write_then_read_gives_the_data(void)
{
	char p[256], in[16] = "fifteen bytes!!", out[16] = {0};
	path(p, "game.eep");
	CHECK(savefile_write(p, in, 16) == 0);
	CHECK(savefile_read(p, out, 16) == SAVEFILE_MAIN);
	CHECK(!memcmp(in, out, 16));
	CHECK(!exists("game.eep.tmp"));
}

static void second_write_keeps_the_first_as_bak(void)
{
	char p[256], b[256], out[4];
	path(p, "game.sra");
	path(b, "game.sra.bak");
	CHECK(savefile_write(p, "old!", 4) == 0);
	CHECK(savefile_write(p, "new!", 4) == 0);
	CHECK(savefile_read(p, out, 4) == SAVEFILE_MAIN);
	CHECK(!memcmp(out, "new!", 4));
	FILE *f = fopen(b, "rb");
	CHECK(f);
	CHECK(fread(out, 1, 4, f) == 4);
	fclose(f);
	CHECK(!memcmp(out, "old!", 4));
}

/* Power cut after the old file became .bak, before .tmp became the file. */
static void complete_tmp_is_used_when_the_file_is_missing(void)
{
	char p[256], out[4];
	put("game.fla.bak", "old!", 4);
	put("game.fla.tmp", "new!", 4);
	path(p, "game.fla");
	CHECK(savefile_read(p, out, 4) == SAVEFILE_TMP);
	CHECK(!memcmp(out, "new!", 4));
}

/* Power cut while writing .tmp: the file is still the last good save. */
static void partial_tmp_is_ignored(void)
{
	char p[256], out[4];
	put("game.mpk", "good", 4);
	put("game.mpk.tmp", "ba", 2);
	path(p, "game.mpk");
	CHECK(savefile_read(p, out, 4) == SAVEFILE_MAIN);
	CHECK(!memcmp(out, "good", 4));
}

static void wrong_size_file_falls_back_to_bak(void)
{
	char p[256], out[4];
	put("bad.eep", "xy", 2);
	put("bad.eep.bak", "safe", 4);
	path(p, "bad.eep");
	CHECK(savefile_read(p, out, 4) == SAVEFILE_BAK);
	CHECK(!memcmp(out, "safe", 4));
}

static void no_file_leaves_the_buffer_alone(void)
{
	char p[256], out[5] = "keep";
	path(p, "none.eep");
	CHECK(savefile_read(p, out, 4) == SAVEFILE_NONE);
	CHECK(!memcmp(out, "keep", 4));
}

static void failed_write_keeps_the_old_file(void)
{
	char p[256], out[4];
	path(p, "missing-dir/game.eep");
	CHECK(savefile_write(p, "data", 4) != 0);
	path(p, "keep.eep");
	CHECK(savefile_write(p, "good", 4) == 0);
	/* A directory where the temp file should go makes the write fail. */
	char t[256];
	path(t, "keep.eep.tmp");
	CHECK(!mkdir(t, 0777));
	CHECK(savefile_write(p, "lost", 4) != 0);
	CHECK(savefile_read(p, out, 4) == SAVEFILE_MAIN);
	CHECK(!memcmp(out, "good", 4));
}

/* ---- game ID ---- */

static void game_id_is_header_name_and_crc(void)
{
	char id[64];
	saves_game_id(id, sizeof(id), (const unsigned char *)"SUPER MARIO 64      ", 0x635A2BFF);
	CHECK(!strcmp(id, "SUPER MARIO 64-635A2BFF"));
}

static void game_id_replaces_characters_fat_rejects(void)
{
	char id[64];
	saves_game_id(id, sizeof(id), (const unsigned char *)"A:B/C?\"D*<>|\\\x82\xA0     ", 0x1);
	CHECK(!strcmp(id, "A_B_C__D_______-00000001"));
}

static void game_id_without_a_name(void)
{
	char id[64];
	saves_game_id(id, sizeof(id), (const unsigned char *)"                    ", 0xABCDEF01);
	CHECK(!strcmp(id, "UNKNOWN-ABCDEF01"));
}

/* ---- when a changed save is written ---- */

static void save_is_due_after_one_quiet_second(void)
{
	saves_timer t = {0};
	CHECK(!saves_due(&t, 1, 0));
	CHECK(!saves_due(&t, 0, 900000));
	CHECK(saves_due(&t, 0, 1000000));
	CHECK(!saves_due(&t, 0, 2000000));
}

static void each_write_restarts_the_quiet_second(void)
{
	saves_timer t = {0};
	CHECK(!saves_due(&t, 1, 0));
	CHECK(!saves_due(&t, 1, 800000));
	CHECK(!saves_due(&t, 0, 1500000));
	CHECK(saves_due(&t, 0, 1800000));
}

static void constant_writes_still_save_every_five_seconds(void)
{
	saves_timer t = {0};
	double now = 0;
	int saved_at = -1;
	for (int i = 0; i <= 60 && saved_at < 0; i++, now += 100000)
		if (saves_due(&t, 1, now))
			saved_at = i;
	CHECK(saved_at == 50);
}

static void nothing_written_is_never_due(void)
{
	saves_timer t = {0};
	CHECK(!saves_due(&t, 0, 0));
	CHECK(!saves_due(&t, 0, 10000000));
	CHECK(!saves_take(&t));
}

static void take_reports_and_clears_a_pending_save(void)
{
	saves_timer t = {0};
	saves_due(&t, 1, 0);
	CHECK(saves_take(&t));
	CHECK(!saves_take(&t));
	CHECK(!saves_due(&t, 0, 5000000));
}

#define RUN(fn) do { g_run++; int before = g_failed; fresh_dir(); fn(); if (g_failed == before) printf("  ok   %s\n", #fn); } while (0)

int main(void)
{
	snprintf(g_dir, sizeof(g_dir), "%s/our64-test-saves-%d", getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp", (int)getpid());
	RUN(write_then_read_gives_the_data);
	RUN(second_write_keeps_the_first_as_bak);
	RUN(complete_tmp_is_used_when_the_file_is_missing);
	RUN(partial_tmp_is_ignored);
	RUN(wrong_size_file_falls_back_to_bak);
	RUN(no_file_leaves_the_buffer_alone);
	RUN(failed_write_keeps_the_old_file);
	RUN(game_id_is_header_name_and_crc);
	RUN(game_id_replaces_characters_fat_rejects);
	RUN(game_id_without_a_name);
	RUN(save_is_due_after_one_quiet_second);
	RUN(each_write_restarts_the_quiet_second);
	RUN(constant_writes_still_save_every_five_seconds);
	RUN(nothing_written_is_never_due);
	RUN(take_reports_and_clears_a_pending_save);
	char cmd[600];
	snprintf(cmd, sizeof(cmd), "rm -rf %s", g_dir);
	if (system(cmd)) {}
	printf("%d of %d failed\n", g_failed, g_run);
	return g_failed != 0;
}
