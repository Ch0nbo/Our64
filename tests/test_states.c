/* PC tests for player/source/states.c: state files and resume points. */
#include "states.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <zlib.h>

static int g_failed, g_run;
static char g_dir[128];

#define CHECK(cond) do { if (!(cond)) { printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); g_failed++; return; } } while (0)

#define RAW 70000
static unsigned char g_raw[RAW], g_out[RAW], g_work[RAW * 2];

static void path(char *out, const char *name) { snprintf(out, 256, "%s/%s", g_dir, name); }

static void fill(unsigned seed)
{
	for (unsigned i = 0; i < RAW; i++)
		g_raw[i] = (unsigned char)((i * 7 + seed) ^ (i >> 5));
}

static int write_state(const char *name, uint32_t crc1, uint32_t counter, unsigned seed)
{
	char p[256];
	path(p, name);
	fill(seed);
	state_info info = { .crc1 = crc1, .crc2 = 0x22222222, .counter = counter };
	return statefile_write(p, &info, g_raw, RAW, NULL, g_work, sizeof(g_work));
}

static long file_size(const char *name)
{
	char p[256];
	struct stat st;
	path(p, name);
	return stat(p, &st) ? -1 : st.st_size;
}

static void poke(const char *name, long at, unsigned char value)
{
	char p[256];
	path(p, name);
	FILE *f = fopen(p, "r+b");
	if (!f) return;
	fseek(f, at, SEEK_SET);
	fputc(value, f);
	fclose(f);
}

static void fresh_dir(void)
{
	char cmd[300];
	snprintf(cmd, sizeof(cmd), "rm -rf %s && mkdir -p %s", g_dir, g_dir);
	if (system(cmd)) exit(2);
}

/* ---- state files ---- */

static void write_then_read_gives_the_state(void)
{
	char p[256];
	state_info info;
	unsigned size = 0;
	CHECK(write_state("s.o64s", 0x11111111, 7, 1) == STATEFILE_OK);
	CHECK(file_size("s.o64s") < RAW); /* compressed */
	path(p, "s.o64s");
	CHECK(statefile_read(p, 0x11111111, 0x22222222, &info, g_out, RAW, &size) == STATEFILE_OK);
	CHECK(size == RAW);
	CHECK(!memcmp(g_out, g_raw, RAW));
	CHECK(info.counter == 7 && info.crc1 == 0x11111111 && info.crc2 == 0x22222222);
}

static void a_changed_byte_is_damage(void)
{
	char p[256];
	state_info info;
	unsigned size;
	write_state("s.o64s", 1, 1, 1);
	poke("s.o64s", file_size("s.o64s") - 10, 0x5A);
	path(p, "s.o64s");
	CHECK(statefile_read(p, 1, 0x22222222, &info, g_out, RAW, &size) == STATEFILE_DAMAGED);
}

static void a_short_file_is_damage(void)
{
	char p[256];
	state_info info;
	unsigned size;
	write_state("s.o64s", 1, 1, 1);
	path(p, "s.o64s");
	CHECK(file_size("s.o64s") > 0);
	CHECK(!truncate(p, file_size("s.o64s") / 2));
	CHECK(statefile_read(p, 1, 0x22222222, &info, g_out, RAW, &size) == STATEFILE_DAMAGED);
	CHECK(!truncate(p, 10));
	CHECK(statefile_read(p, 1, 0x22222222, &info, g_out, RAW, &size) == STATEFILE_DAMAGED);
}

static void another_games_state_is_refused(void)
{
	char p[256];
	state_info info;
	unsigned size;
	write_state("s.o64s", 1, 1, 1);
	path(p, "s.o64s");
	CHECK(statefile_read(p, 2, 0x22222222, &info, g_out, RAW, &size) == STATEFILE_OTHER_GAME);
	CHECK(statefile_read(p, 1, 0x33333333, &info, g_out, RAW, &size) == STATEFILE_OTHER_GAME);
}

static void another_version_is_refused(void)
{
	char p[256];
	state_info info;
	unsigned size;
	write_state("s.o64s", 1, 1, 1);
	poke("s.o64s", 7, STATEFILE_VERSION + 1); /* low byte of the big-endian version */
	path(p, "s.o64s");
	CHECK(statefile_read(p, 1, 0x22222222, &info, g_out, RAW, &size) == STATEFILE_OTHER_VERSION);
}

static void not_a_state_is_damage(void)
{
	char p[256];
	state_info info;
	unsigned size;
	path(p, "junk.o64s");
	FILE *f = fopen(p, "wb");
	for (int i = 0; i < 100; i++) fputc(i, f);
	fclose(f);
	CHECK(statefile_read(p, 1, 0x22222222, &info, g_out, RAW, &size) == STATEFILE_DAMAGED);
}

static void missing_file(void)
{
	char p[256];
	state_info info;
	unsigned size;
	path(p, "none.o64s");
	CHECK(statefile_read(p, 1, 0x22222222, &info, g_out, RAW, &size) == STATEFILE_MISSING);
}

static void a_state_bigger_than_the_buffer_is_refused(void)
{
	char p[256];
	state_info info;
	unsigned size;
	write_state("s.o64s", 1, 1, 1);
	path(p, "s.o64s");
	CHECK(statefile_read(p, 1, 0x22222222, &info, g_out, RAW - 1, &size) == STATEFILE_TOO_BIG);
}

static void write_into_a_missing_folder_fails(void)
{
	CHECK(write_state("no-folder/s.o64s", 1, 1, 1) == STATEFILE_WRITE_FAILED);
}

static void too_little_work_space_fails_without_writing(void)
{
	char p[256];
	path(p, "s.o64s");
	state_info info = { .crc1 = 1, .crc2 = 2, .counter = 3 };
	fill(1);
	CHECK(statefile_write(p, &info, g_raw, RAW, NULL, g_work, 100) == STATEFILE_WRITE_FAILED);
	CHECK(file_size("s.o64s") < 0);
}


static unsigned char g_thumb[THUMB_BYTES], g_thumb_out[THUMB_BYTES];

static void fill_thumb(unsigned seed)
{
	for (unsigned i = 0; i < THUMB_BYTES; i++)
		g_thumb[i] = (unsigned char)(i * 13 + seed);
}

static void the_header_keeps_time_slot_backup_and_thumbnail(void)
{
	char p[256];
	path(p, "s.o64s");
	fill(3);
	fill_thumb(3);
	state_info info = { .crc1 = 1, .crc2 = 0x22222222, .counter = 0 };
	info.saved_at = 1760000000;
	info.slot = 2;
	info.before = UNDO_LOAD;
	CHECK(statefile_write(p, &info, g_raw, RAW, g_thumb, g_work, sizeof(g_work)) == STATEFILE_OK);
	state_info got;
	memset(g_thumb_out, 0, sizeof(g_thumb_out));
	CHECK(statefile_peek(p, 1, 0x22222222, &got, g_thumb_out) == STATEFILE_OK);
	CHECK(got.saved_at == 1760000000 && got.slot == 2 && got.before == UNDO_LOAD && got.has_thumb);
	CHECK(got.version == STATEFILE_VERSION);
	CHECK(!memcmp(g_thumb_out, g_thumb, THUMB_BYTES));
	CHECK(statefile_peek(p, 1, 0x22222222, &got, NULL) == STATEFILE_OK && got.slot == 2); /* no picture wanted */
	unsigned size = 0;
	CHECK(statefile_read(p, 1, 0x22222222, &got, g_out, RAW, &size) == STATEFILE_OK);
	CHECK(size == RAW && !memcmp(g_out, g_raw, RAW));
}

static void a_state_without_a_thumbnail_says_so(void)
{
	char p[256];
	path(p, "s.o64s");
	CHECK(write_state("s.o64s", 1, 1, 1) == STATEFILE_OK);
	state_info got;
	CHECK(statefile_peek(p, 1, 0x22222222, &got, g_thumb_out) == STATEFILE_OK);
	CHECK(!got.has_thumb);
}

static void peek_finds_a_short_file_damaged(void)
{
	char p[256];
	path(p, "s.o64s");
	write_state("s.o64s", 1, 1, 1);
	CHECK(!truncate(p, file_size("s.o64s") - 1));
	state_info got;
	CHECK(statefile_peek(p, 1, 0x22222222, &got, g_thumb_out) == STATEFILE_DAMAGED);
	CHECK(statefile_peek(p, 2, 0x22222222, &got, g_thumb_out) != STATEFILE_OK);
}

/* A version 3 file: 32-byte header, then the packed state. */
static void write_v3(const char *name, uint32_t counter)
{
	char p[256];
	path(p, name);
	uLongf packed = sizeof(g_work);
	compress2(g_work, &packed, g_raw, RAW, 1);
	unsigned char h[32];
	uint32_t v[8] = { 0, 3, 1, 0x22222222, counter, RAW, (uint32_t)packed, (uint32_t)crc32(0, g_work, (uInt)packed) };
	for (int i = 0; i < 8; i++) {
		h[i * 4] = v[i] >> 24; h[i * 4 + 1] = v[i] >> 16; h[i * 4 + 2] = v[i] >> 8; h[i * 4 + 3] = v[i];
	}
	memcpy(h, "O64S", 4);
	FILE *f = fopen(p, "wb");
	fwrite(h, 1, 32, f);
	fwrite(g_work, 1, packed, f);
	fclose(f);
}

static void a_version_3_resume_point_still_loads(void)
{
	char p[256];
	fill(6);
	write_v3("s.o64s", 4);
	path(p, "s.o64s");
	state_info got;
	unsigned size = 0;
	CHECK(statefile_read(p, 1, 0x22222222, &got, g_out, RAW, &size) == STATEFILE_OK);
	CHECK(got.version == 3 && got.counter == 4 && !got.has_thumb && got.saved_at == 0);
	CHECK(size == RAW && !memcmp(g_out, g_raw, RAW));
	CHECK(statefile_peek(p, 1, 0x22222222, &got, g_thumb_out) == STATEFILE_OK);
}

static void version_2_is_refused(void)
{
	char p[256];
	fill(6);
	write_v3("s.o64s", 4);
	poke("s.o64s", 7, 2);
	path(p, "s.o64s");
	state_info got;
	unsigned size;
	CHECK(statefile_read(p, 1, 0x22222222, &got, g_out, RAW, &size) == STATEFILE_OTHER_VERSION);
	CHECK(statefile_peek(p, 1, 0x22222222, &got, g_thumb_out) == STATEFILE_OTHER_VERSION);
}

/* ---- save state cards ---- */

static int save_card(int card, unsigned seed, int keep_old, int undo)
{
	fill(seed);
	fill_thumb(seed);
	state_info info = { .crc1 = 1, .crc2 = 0x22222222 };
	info.saved_at = 1000 + seed;
	info.slot = card < CARD_SLOTS ? card + 1 : 0;
	info.before = (uint8_t)undo;
	return card_save(g_dir, card, &info, keep_old, g_raw, RAW, g_thumb, g_work, sizeof(g_work));
}

static int peek_card(int card, state_info *info)
{
	return card_peek(g_dir, card, 1, 0x22222222, info, g_thumb_out);
}

static void card_files_have_plain_names(void)
{
	CHECK(!strcmp(card_file(CARD_SLOT1), "slot1.o64s"));
	CHECK(!strcmp(card_file(CARD_SLOT3), "slot3.o64s"));
	CHECK(!strcmp(card_file(CARD_UNDO), "undo.o64s"));
	CHECK(CARDS == 4 && CARD_SLOTS == 3);
}

static void saving_to_an_empty_slot_keeps_no_undo(void)
{
	state_info info;
	CHECK(save_card(CARD_SLOT2, 1, 1, 0) == STATEFILE_OK);
	CHECK(peek_card(CARD_SLOT2, &info) == STATEFILE_OK && info.slot == 2 && info.saved_at == 1001);
	CHECK(peek_card(CARD_UNDO, &info) == STATEFILE_MISSING);
	CHECK(file_size("slot2.o64s.tmp") < 0);
}

static void overwriting_makes_the_old_copy_undo_save(void)
{
	state_info info;
	unsigned size;
	save_card(CARD_SLOT2, 1, 1, 0);
	CHECK(save_card(CARD_SLOT2, 2, 1, 0) == STATEFILE_OK);
	CHECK(peek_card(CARD_UNDO, &info) == STATEFILE_OK && info.slot == 2 && info.saved_at == 1001);
	CHECK(card_undo_kind(&info) == UNDO_SAVE);
	CHECK(card_read(g_dir, CARD_UNDO, 1, 0x22222222, &info, g_out, RAW, &size) == STATEFILE_OK);
	fill(1);
	CHECK(!memcmp(g_out, g_raw, RAW));
	CHECK(card_read(g_dir, CARD_SLOT2, 1, 0x22222222, &info, g_out, RAW, &size) == STATEFILE_OK);
	fill(2);
	CHECK(!memcmp(g_out, g_raw, RAW));
}

static void one_undo_replaces_the_last_whatever_its_kind(void)
{
	state_info info;
	save_card(CARD_SLOT1, 1, 1, 0);
	save_card(CARD_SLOT1, 2, 1, 0); /* Undo save: slot 1's first copy */
	CHECK(save_card(CARD_UNDO, 3, 1, UNDO_LOAD) == STATEFILE_OK);
	CHECK(peek_card(CARD_UNDO, &info) == STATEFILE_OK && info.saved_at == 1003 && card_undo_kind(&info) == UNDO_LOAD);
	CHECK(save_card(CARD_UNDO, 4, 1, UNDO_RESET) == STATEFILE_OK);
	CHECK(peek_card(CARD_UNDO, &info) == STATEFILE_OK && card_undo_kind(&info) == UNDO_RESET);
	save_card(CARD_SLOT3, 5, 1, 0);
	CHECK(save_card(CARD_SLOT3, 6, 1, 0) == STATEFILE_OK);
	CHECK(peek_card(CARD_UNDO, &info) == STATEFILE_OK && info.slot == 3 && card_undo_kind(&info) == UNDO_SAVE);
}

static void overwriting_an_unloadable_copy_keeps_no_undo(void)
{
	state_info info;
	save_card(CARD_SLOT1, 1, 1, 0);
	save_card(CARD_SLOT1, 2, 1, 0); /* undo: slot 1's first copy */
	CHECK(save_card(CARD_SLOT1, 3, 0, 0) == STATEFILE_OK);
	CHECK(peek_card(CARD_UNDO, &info) == STATEFILE_OK && info.saved_at == 1001);
	CHECK(peek_card(CARD_SLOT1, &info) == STATEFILE_OK && info.saved_at == 1003);
}

static void a_failed_save_changes_nothing(void)
{
	state_info info;
	save_card(CARD_SLOT1, 1, 1, 0);
	save_card(CARD_SLOT1, 2, 1, 0);
	fill(3);
	state_info in = { .crc1 = 1, .crc2 = 0x22222222 };
	CHECK(card_save(g_dir, CARD_SLOT1, &in, 1, g_raw, RAW, NULL, g_work, 100) == STATEFILE_WRITE_FAILED);
	CHECK(peek_card(CARD_SLOT1, &info) == STATEFILE_OK && info.saved_at == 1002);
	CHECK(peek_card(CARD_UNDO, &info) == STATEFILE_OK && info.saved_at == 1001);
	CHECK(file_size("slot1.o64s.tmp") < 0);
}

/* When the old Undo card cannot be set aside (a folder in the way), the
 * overwrite fails and the Undo card and the slot stay as they were. */
static void a_failed_backup_keeps_the_old_undo(void)
{
	state_info info;
	char p[256];
	save_card(CARD_UNDO, 1, 1, UNDO_RESET);
	save_card(CARD_SLOT1, 2, 1, 0);
	path(p, "undo.o64s.old");
	CHECK(!mkdir(p, 0777));
	path(p, "undo.o64s.old/x");
	FILE *f = fopen(p, "w");
	if (f) fclose(f);
	CHECK(save_card(CARD_SLOT1, 3, 1, 0) == STATEFILE_BACKUP_FAILED);
	CHECK(peek_card(CARD_UNDO, &info) == STATEFILE_OK && info.saved_at == 1001 && card_undo_kind(&info) == UNDO_RESET);
	CHECK(peek_card(CARD_SLOT1, &info) == STATEFILE_OK && info.saved_at == 1002);
	CHECK(file_size("slot1.o64s.tmp") < 0);
}

/* Loading the Undo card: read it first, then keep the game now in its place. */
static void loading_the_undo_card_swaps_the_two(void)
{
	state_info info;
	unsigned size;
	save_card(CARD_UNDO, 1, 1, UNDO_RESET);
	CHECK(card_read(g_dir, CARD_UNDO, 1, 0x22222222, &info, g_out, RAW, &size) == STATEFILE_OK);
	CHECK(save_card(CARD_UNDO, 2, 1, UNDO_LOAD) == STATEFILE_OK);
	fill(1);
	CHECK(!memcmp(g_out, g_raw, RAW));
	CHECK(peek_card(CARD_UNDO, &info) == STATEFILE_OK && info.saved_at == 1002 && card_undo_kind(&info) == UNDO_LOAD);
}

static void delete_empties_a_slot(void)
{
	state_info info;
	save_card(CARD_SLOT3, 1, 1, 0);
	CHECK(card_delete(g_dir, CARD_SLOT3) == 0);
	CHECK(peek_card(CARD_SLOT3, &info) == STATEFILE_MISSING);
	CHECK(card_delete(g_dir, CARD_SLOT3) == 0); /* already empty */
}

/* A power cut between the renames leaves a complete .tmp and no slot file. */
static void a_complete_tmp_is_taken_back(void)
{
	char a[256], b[256];
	state_info info;
	save_card(CARD_SLOT2, 7, 1, 0);
	path(a, "slot2.o64s");
	path(b, "slot2.o64s.tmp");
	CHECK(!rename(a, b));
	CHECK(peek_card(CARD_SLOT2, &info) == STATEFILE_OK && info.saved_at == 1007);
	CHECK(file_size("slot2.o64s") > 0 && file_size("slot2.o64s.tmp") < 0);
}

static void a_partial_tmp_is_thrown_away(void)
{
	char a[256], b[256];
	state_info info;
	save_card(CARD_SLOT2, 7, 1, 0);
	path(a, "slot2.o64s");
	path(b, "slot2.o64s.tmp");
	CHECK(!rename(a, b));
	CHECK(!truncate(b, 100));
	CHECK(peek_card(CARD_SLOT2, &info) == STATEFILE_MISSING);
	CHECK(file_size("slot2.o64s.tmp") < 0);
}

/* ---- resume points ---- */

static int resume(resume_plan *plan, unsigned *size)
{
	char a[256], b[256];
	path(a, "resume-a.o64s");
	path(b, "resume-b.o64s");
	return resume_load(a, b, 1, 0x22222222, plan, g_out, RAW, size);
}

static void no_resume_point_writes_a_first(void)
{
	resume_plan plan;
	unsigned size;
	CHECK(resume(&plan, &size) == STATEFILE_MISSING);
	CHECK(plan.loaded_file == -1 && plan.next_file == 0 && plan.next_counter == 1);
}

static void the_newer_file_loads_and_the_other_is_next(void)
{
	resume_plan plan;
	unsigned size;
	write_state("resume-a.o64s", 1, 4, 4);
	write_state("resume-b.o64s", 1, 5, 5);
	CHECK(resume(&plan, &size) == STATEFILE_OK);
	fill(5);
	CHECK(!memcmp(g_out, g_raw, RAW));
	CHECK(plan.loaded_file == 1 && plan.next_file == 0 && plan.next_counter == 6);
}

static void a_damaged_newer_file_falls_back_to_the_older(void)
{
	resume_plan plan;
	unsigned size;
	write_state("resume-a.o64s", 1, 9, 9);
	write_state("resume-b.o64s", 1, 8, 8);
	poke("resume-a.o64s", file_size("resume-a.o64s") - 3, 0xEE);
	CHECK(resume(&plan, &size) == STATEFILE_OK);
	fill(8);
	CHECK(!memcmp(g_out, g_raw, RAW));
	CHECK(plan.loaded_file == 1 && plan.next_file == 0 && plan.next_counter == 10);
}

static void another_games_resume_point_is_ignored(void)
{
	resume_plan plan;
	unsigned size;
	write_state("resume-a.o64s", 1, 2, 2);
	write_state("resume-b.o64s", 99, 50, 3);
	CHECK(resume(&plan, &size) == STATEFILE_OK);
	CHECK(plan.loaded_file == 0 && plan.next_file == 1 && plan.next_counter == 3);
}

static void both_damaged_reports_damage(void)
{
	resume_plan plan;
	unsigned size;
	write_state("resume-a.o64s", 1, 2, 2);
	write_state("resume-b.o64s", 1, 3, 3);
	poke("resume-a.o64s", file_size("resume-a.o64s") - 3, 0xEE);
	poke("resume-b.o64s", file_size("resume-b.o64s") - 3, 0xEE);
	CHECK(resume(&plan, &size) == STATEFILE_DAMAGED);
	CHECK(plan.loaded_file == -1 && plan.next_counter == 4);
}

static void forget_deletes_both(void)
{
	char a[256], b[256];
	write_state("resume-a.o64s", 1, 2, 2);
	write_state("resume-b.o64s", 1, 3, 3);
	path(a, "resume-a.o64s");
	path(b, "resume-b.o64s");
	resume_forget(a, b);
	CHECK(file_size("resume-a.o64s") < 0 && file_size("resume-b.o64s") < 0);
}


/* ---- when to capture ---- */

static void nothing_is_due_before_a_minute(void)
{
	resume_timer t;
	resume_timer_init(&t, 0);
	CHECK(resume_decide(&t, 59e6, -1, 0) == RESUME_WAIT);
	CHECK(resume_decide(&t, 60e6, -1, 0) == RESUME_DUE);
}

static void a_waiting_save_is_due_at_once(void)
{
	resume_timer t;
	resume_timer_init(&t, 0);
	CHECK(resume_decide(&t, 5e6, 4.9e6, 0) == RESUME_DUE);
}

static void a_save_with_no_chance_gives_up_after_five_seconds(void)
{
	resume_timer t;
	resume_timer_init(&t, 0);
	CHECK(resume_decide(&t, 14.9e6, 10e6, 0) == RESUME_DUE);
	CHECK(resume_decide(&t, 15e6, 10e6, 0) == RESUME_GIVE_UP);
}

static void a_busy_writer_waits(void)
{
	resume_timer t;
	resume_timer_init(&t, 0);
	CHECK(resume_decide(&t, 61e6, -1, 1) == RESUME_WAIT);
	CHECK(resume_decide(&t, 12e6, 10e6, 1) == RESUME_WAIT);
	CHECK(resume_decide(&t, 15e6, 10e6, 1) == RESUME_GIVE_UP);
}

static void a_capture_restarts_the_minute(void)
{
	resume_timer t;
	resume_timer_init(&t, 0);
	resume_captured(&t, 30e6, 3000);
	CHECK(resume_decide(&t, 89e6, -1, 0) == RESUME_WAIT);
	CHECK(resume_decide(&t, 90e6, -1, 0) == RESUME_DUE);
}

static void a_capture_fits_in_idle_time_with_a_fifth_to_spare(void)
{
	resume_timer t;
	resume_timer_init(&t, 0);
	resume_captured(&t, 0, 5000);
	CHECK(!resume_fits(&t, 5999));
	CHECK(resume_fits(&t, 6000));
}

static void the_first_capture_assumes_ten_ms(void)
{
	resume_timer t;
	resume_timer_init(&t, 0);
	CHECK(!resume_fits(&t, 11999));
	CHECK(resume_fits(&t, 12000));
}

#define RUN(fn) do { g_run++; int before = g_failed; fresh_dir(); fn(); if (g_failed == before) printf("  ok   %s\n", #fn); } while (0)

int main(void)
{
	setvbuf(stdout, NULL, _IOLBF, 0);
	snprintf(g_dir, sizeof(g_dir), "%s/our64-test-states-%d", getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp", (int)getpid());
	RUN(write_then_read_gives_the_state);
	RUN(a_changed_byte_is_damage);
	RUN(a_short_file_is_damage);
	RUN(another_games_state_is_refused);
	RUN(another_version_is_refused);
	RUN(not_a_state_is_damage);
	RUN(missing_file);
	RUN(a_state_bigger_than_the_buffer_is_refused);
	RUN(write_into_a_missing_folder_fails);
	RUN(too_little_work_space_fails_without_writing);
	RUN(the_header_keeps_time_slot_backup_and_thumbnail);
	RUN(a_state_without_a_thumbnail_says_so);
	RUN(peek_finds_a_short_file_damaged);
	RUN(a_version_3_resume_point_still_loads);
	RUN(version_2_is_refused);
	RUN(card_files_have_plain_names);
	RUN(saving_to_an_empty_slot_keeps_no_undo);
	RUN(overwriting_makes_the_old_copy_undo_save);
	RUN(one_undo_replaces_the_last_whatever_its_kind);
	RUN(overwriting_an_unloadable_copy_keeps_no_undo);
	RUN(a_failed_save_changes_nothing);
	RUN(loading_the_undo_card_swaps_the_two);
	RUN(a_failed_backup_keeps_the_old_undo);
	RUN(delete_empties_a_slot);
	RUN(a_complete_tmp_is_taken_back);
	RUN(a_partial_tmp_is_thrown_away);
	RUN(no_resume_point_writes_a_first);
	RUN(the_newer_file_loads_and_the_other_is_next);
	RUN(a_damaged_newer_file_falls_back_to_the_older);
	RUN(another_games_resume_point_is_ignored);
	RUN(both_damaged_reports_damage);
	RUN(forget_deletes_both);
	RUN(nothing_is_due_before_a_minute);
	RUN(a_waiting_save_is_due_at_once);
	RUN(a_save_with_no_chance_gives_up_after_five_seconds);
	RUN(a_busy_writer_waits);
	RUN(a_capture_restarts_the_minute);
	RUN(a_capture_fits_in_idle_time_with_a_fifth_to_spare);
	RUN(the_first_capture_assumes_ten_ms);
	char cmd[300];
	snprintf(cmd, sizeof(cmd), "rm -rf %s", g_dir);
	if (system(cmd)) {}
	printf("%d of %d failed\n", g_failed, g_run);
	return g_failed != 0;
}
