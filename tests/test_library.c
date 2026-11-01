/* PC tests for player/source/library.c: Our ROMs' games, their order and
 * the letter bar. */
#include "library.h"
#include "gamedb.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int g_failed, g_run;

#define CHECK(cond) do { if (!(cond)) { printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); g_failed++; return; } } while (0)

/* A big-endian (.z64) header: magic, clock, PC, release, CRC1, CRC2, 8 unknown, name. */
static void header(unsigned char *h, uint32_t crc1, uint32_t crc2, const char *name, char country)
{
	memset(h, 0, 64);
	h[0] = 0x80; h[1] = 0x37; h[2] = 0x12; h[3] = 0x40;
	for (int i = 0; i < 4; i++) {
		h[16 + i] = (unsigned char)(crc1 >> (24 - 8 * i));
		h[20 + i] = (unsigned char)(crc2 >> (24 - 8 * i));
	}
	memset(h + 32, ' ', 20);
	memcpy(h + 32, name, strlen(name));
	h[62] = (unsigned char)country;
}

static void read_a_z64_header(void)
{
	unsigned char h[64];
	library_rom r;
	header(h, 0x635A2BFF, 0x8B022326, "SUPER MARIO 64", 'E');
	CHECK(library_header(h, 8 * 1024 * 1024, &r) == 0);
	CHECK(r.crc1 == 0x635A2BFF && r.crc2 == 0x8B022326 && r.country == 'E');
	CHECK(!strcmp(r.id, "SUPER MARIO 64-635A2BFF")); /* the saves' game id */
}

static void read_byte_swapped_and_little_endian_headers(void)
{
	unsigned char h[64], v64[64], n64[64];
	library_rom r;
	header(h, 0x635A2BFF, 0x8B022326, "SUPER MARIO 64", 'E');
	for (int i = 0; i < 64; i += 2) { v64[i] = h[i + 1]; v64[i + 1] = h[i]; }
	for (int i = 0; i < 64; i += 4) { n64[i] = h[i + 3]; n64[i + 1] = h[i + 2]; n64[i + 2] = h[i + 1]; n64[i + 3] = h[i]; }
	CHECK(library_header(v64, 8 << 20, &r) == 0 && r.crc1 == 0x635A2BFF && !strcmp(r.id, "SUPER MARIO 64-635A2BFF"));
	CHECK(library_header(n64, 8 << 20, &r) == 0 && r.crc1 == 0x635A2BFF && !strcmp(r.id, "SUPER MARIO 64-635A2BFF"));
}

static void not_a_rom_is_refused(void)
{
	unsigned char h[64];
	library_rom r;
	memset(h, 0x41, 64);
	CHECK(library_header(h, 8 << 20, &r) < 0);
	header(h, 1, 2, "TINY", 'E');
	CHECK(library_header(h, 0x1000, &r) < 0); /* no room for a boot code and a game */
}

static void titles_come_from_file_names(void)
{
	library_game g;
	library_game_init(&g, "Legend of Zelda, The - Ocarina of Time (USA) (Rev 1).z64");
	CHECK(!strcmp(g.title, "The Legend of Zelda: Ocarina of Time"));
	CHECK(library_category_of(g.title) == library_category_of("L")); /* filed under L, as before */
	CHECK(!strcmp(g.file, "Legend of Zelda, The - Ocarina of Time (USA) (Rev 1).z64"));
}

static void games_sort_a_to_z_ignoring_case_with_symbols_first(void)
{
	/* # leads, as on the letter bar: Recent, All, #, A to Z (Zach, 2026-10-17) */
	const char *files[] = { "wave race 64.z64", "1080 Snowboarding.z64", "Banjo-Kazooie.z64", "army men.n64",
	                        "Blast Corps.v64" };
	library_game g[5];
	for (int i = 0; i < 5; i++)
		library_game_init(&g[i], files[i]);
	library_sort(g, 5);
	CHECK(!strcmp(g[0].title, "1080 Snowboarding"));
	CHECK(!strcmp(g[1].title, "army men"));
	CHECK(!strcmp(g[2].title, "Banjo-Kazooie"));
	CHECK(!strcmp(g[3].title, "Blast Corps"));
	CHECK(!strcmp(g[4].title, "wave race 64"));
}

static void the_letter_bar_is_favorites_recent_all_then_hash_then_a_to_z(void)
{
	char label[8];
	/* the star first, Recent after it (Zach, 2026-10-25) */
	CHECK(LIBRARY_FAVORITES == 0 && LIBRARY_RECENT == 1 && LIBRARY_ALL == 2 && LIBRARY_HASH == 3);
	CHECK(!strcmp(library_category_label(LIBRARY_FAVORITES, label, sizeof(label)), "\u2605"));
	CHECK(!strcmp(library_category_label(LIBRARY_RECENT, label, sizeof(label)), "Recent"));
	CHECK(!strcmp(library_category_label(LIBRARY_ALL, label, sizeof(label)), "All"));
	CHECK(!strcmp(library_category_label(LIBRARY_HASH, label, sizeof(label)), "#"));
	CHECK(!strcmp(library_category_label(LIBRARY_HASH + 1, label, sizeof(label)), "A"));
	CHECK(!strcmp(library_category_label(LIBRARY_CATEGORIES - 1, label, sizeof(label)), "Z"));
	CHECK(library_category_of("banjo") == LIBRARY_HASH + 2);
	CHECK(library_category_of("1080 Snowboarding") == LIBRARY_HASH);
	CHECK(library_category_of("'98 Koshien") == LIBRARY_HASH);
	CHECK(library_category_of("") == LIBRARY_HASH);
	/* the bar's tabs sit side by side, left to right, inside the 854-wide screen */
	int a[4], b[4];
	library_bar_rect(0, a);
	CHECK(a[0] >= 10);
	for (int c = 1; c < LIBRARY_CATEGORIES; c++) {
		library_bar_rect(c, b);
		CHECK(b[0] == a[0] + a[2]);
		CHECK(library_bar_hit(b[0] + b[2] / 2, b[1] + 4) == c);
		memcpy(a, b, sizeof(a));
	}
	CHECK(a[0] + a[2] <= 844);
}

static void a_letter_shows_its_games_and_all_shows_every_game(void)
{
	const char *files[] = { "Banjo.z64", "Blast Corps.z64", "Diddy Kong Racing.z64", "F-Zero X.z64", "007.z64" };
	library_game g[5];
	for (int i = 0; i < 5; i++)
		library_game_init(&g[i], files[i]);
	library_sort(g, 5);
	int shown[5];
	CHECK(library_show(g, 5, LIBRARY_ALL, shown) == 5 && !strcmp(g[shown[0]].title, "007"));
	CHECK(library_show(g, 5, library_category_of("B"), shown) == 2);
	CHECK(!strcmp(g[shown[0]].title, "Banjo") && !strcmp(g[shown[1]].title, "Blast Corps"));
	CHECK(library_show(g, 5, library_category_of("C"), shown) == 0);
	int has[LIBRARY_CATEGORIES];
	library_categories(g, 5, has);
	CHECK(!has[LIBRARY_FAVORITES] && !has[LIBRARY_RECENT] && has[LIBRARY_ALL] && has[LIBRARY_HASH] &&
	      has[library_category_of("B")] && !has[library_category_of("A")]);
	/* left/right steps over letters with no games, and wraps at the ends (Zach, 2026-10-20) */
	CHECK(library_next_category(has, LIBRARY_ALL, +1) == LIBRARY_HASH);
	CHECK(library_next_category(has, LIBRARY_HASH, +1) == library_category_of("B"));
	CHECK(library_next_category(has, library_category_of("B"), +1) == library_category_of("D"));
	CHECK(library_next_category(has, library_category_of("F"), +1) == LIBRARY_ALL); /* no favorites or Recent games */
	CHECK(library_next_category(has, library_category_of("B"), -1) == LIBRARY_HASH);
	CHECK(library_next_category(has, LIBRARY_ALL, -1) == library_category_of("F")); /* no favorites or Recent games */
}

static void recent_shows_the_games_played_last_a_to_z(void)
{
	const char *files[] = { "Banjo.z64", "Blast Corps.z64", "Diddy Kong Racing.z64", "F-Zero X.z64", "007.z64" };
	library_game g[5];
	for (int i = 0; i < 5; i++)
		library_game_init(&g[i], files[i]);
	library_sort(g, 5);
	const char recent[3][128] = { "f-zero x.z64", "Banjo.z64", "Gone From The Card.z64" };
	library_mark_recent(g, 5, recent, 3);
	int shown[5];
	CHECK(library_show(g, 5, LIBRARY_RECENT, shown) == 2);
	CHECK(!strcmp(g[shown[0]].title, "Banjo") && !strcmp(g[shown[1]].title, "F-Zero X"));
	int has[LIBRARY_CATEGORIES];
	library_categories(g, 5, has);
	CHECK(has[LIBRARY_RECENT] && library_next_category(has, LIBRARY_ALL, -1) == LIBRARY_RECENT);
	CHECK(library_next_category(has, LIBRARY_RECENT, -1) == library_category_of("F")); /* Left on Recent: the last letter */
	CHECK(library_next_category(has, library_category_of("F"), +1) == LIBRARY_RECENT);
	library_mark_recent(g, 5, recent, 0);
	CHECK(library_show(g, 5, LIBRARY_RECENT, shown) == 0);
}

static void favorites_show_the_starred_games_and_keep_the_rest_of_the_file(void)
{
	const char *files[] = { "Banjo.z64", "Blast Corps.z64", "Diddy Kong Racing.z64", "F-Zero X.z64", "007.z64" };
	library_game g[5];
	for (int i = 0; i < 5; i++)
		library_game_init(&g[i], files[i]);
	library_sort(g, 5);
	char path[] = "/tmp/our64-favorites-XXXXXX";
	int fd = mkstemp(path);
	CHECK(fd >= 0);
	const char text[] = "# made by hand\nf-zero x.z64\r\nGone From The Card.z64\n\nBanjo.z64\nBanjo.z64\n";
	CHECK(write(fd, text, sizeof(text) - 1) == (ssize_t)sizeof(text) - 1);
	close(fd);
	CHECK(library_favorites_read(path) == 3); /* comments, blank lines and a repeat skipped */
	library_mark_favorites(g, 5);
	int shown[5];
	CHECK(library_show(g, 5, LIBRARY_FAVORITES, shown) == 2);
	CHECK(!strcmp(g[shown[0]].title, "Banjo") && !strcmp(g[shown[1]].title, "F-Zero X"));
	int has[LIBRARY_CATEGORIES];
	library_categories(g, 5, has);
	/* the star is the bar's first tab: left from it wraps to the last letter, right from that comes back */
	CHECK(has[LIBRARY_FAVORITES] && !has[LIBRARY_RECENT] && library_next_category(has, LIBRARY_ALL, -1) == LIBRARY_FAVORITES);
	CHECK(library_next_category(has, LIBRARY_FAVORITES, -1) == library_category_of("F"));
	CHECK(library_next_category(has, library_category_of("F"), +1) == LIBRARY_FAVORITES);
	/* with Recent games too, the star comes before Recent */
	const char recent[1][128] = { "Diddy Kong Racing.z64" };
	library_mark_recent(g, 5, recent, 1);
	library_categories(g, 5, has);
	CHECK(library_next_category(has, LIBRARY_FAVORITES, +1) == LIBRARY_RECENT);
	CHECK(library_next_category(has, LIBRARY_RECENT, -1) == LIBRARY_FAVORITES);
	library_mark_recent(g, 5, recent, 0);
	/* unstar Banjo, star 007: the game not on the card stays in the file */
	CHECK(library_favorite_toggle(&g[shown[0]]) == 0 && !g[shown[0]].favorite);
	CHECK(library_favorite_toggle(&g[0]) == 1 && g[0].favorite && !strcmp(g[0].title, "007"));
	char out[512];
	int n = library_favorites_text(out, sizeof(out));
	CHECK(n == library_favorites_text(NULL, 0) && n == (int)strlen(out));
	CHECK(!strncmp(out, "# Our64 favorites: one ROM file per line.\n", 42));
	CHECK(strstr(out, "f-zero x.z64\n") && strstr(out, "Gone From The Card.z64\n") && strstr(out, "007.z64\n") &&
	      !strstr(out, "Banjo"));
	FILE *f = fopen(path, "w");
	fputs(out, f);
	fclose(f);
	CHECK(library_favorites_read(path) == 3);
	library_mark_favorites(g, 5);
	CHECK(library_show(g, 5, LIBRARY_FAVORITES, shown) == 2 && !strcmp(g[shown[0]].title, "007"));
	/* a short buffer still says the whole length */
	char small[8];
	CHECK(library_favorites_text(small, sizeof(small)) == n && strlen(small) == sizeof(small) - 1);
	unlink(path);
	CHECK(library_favorites_read(path) == -1);
	library_mark_favorites(g, 5);
	library_categories(g, 5, has);
	CHECK(!has[LIBRARY_FAVORITES]);
}

static void two_files_of_one_game_are_found(void)
{
	library_game g[3];
	library_game_init(&g[0], "a.z64");
	library_game_init(&g[1], "b.z64");
	library_game_init(&g[2], "c.z64");
	snprintf(g[0].rom.id, sizeof(g[0].rom.id), "SUPER MARIO 64-635A2BFF");
	snprintf(g[1].rom.id, sizeof(g[1].rom.id), "MARIOKART64-3E5055B6");
	snprintf(g[2].rom.id, sizeof(g[2].rom.id), "SUPER MARIO 64-635A2BFF");
	g[0].readable = g[1].readable = g[2].readable = 1;
	CHECK(library_shares(g, 3, 0) == 2 && library_shares(g, 3, 2) == 0 && library_shares(g, 3, 1) == -1);
}

static void the_last_game_is_found_by_file_name(void)
{
	const char *files[] = { "Banjo.z64", "Super Mario 64 (USA).z64" };
	library_game g[2];
	for (int i = 0; i < 2; i++)
		library_game_init(&g[i], files[i]);
	CHECK(library_find(g, 2, "Super Mario 64 (USA).z64") == 1);
	CHECK(library_find(g, 2, "super mario 64 (usa).z64") == 1); /* FAT ignores case */
	CHECK(library_find(g, 2, "Gone.z64") == -1);
}

static void paks_follow_the_game_not_the_rumble_switch(void)
{
	int mk64 = gamedb_paks(0x3E5055B6, 0x2E92DA52); /* Mario Kart 64 (U): ghosts on the Controller Pak */
	CHECK(mk64 == GAMEDB_MEMPAK);
	CHECK(gamedb_paks(0x635A2BFF, 0x8B022326) == 0); /* Super Mario 64 (U): neither */
	CHECK(gamedb_paks(0x664BA3D4, 0x678A80B7) == (GAMEDB_RUMBLE | GAMEDB_TRANSFER)); /* Mario Golf (U) */
	CHECK(gamedb_pak_for(mk64) == PAK_CONTROLLER);
	CHECK(gamedb_pak_for(GAMEDB_RUMBLE) == PAK_RUMBLE);
	CHECK(gamedb_pak_for(GAMEDB_RUMBLE | GAMEDB_MEMPAK) == PAK_RUMBLE);
	CHECK(gamedb_pak_for(0) == PAK_CONTROLLER);
}

static void counter_factors_come_from_the_catalogue(void)
{
	CHECK(gamedb_count_per_op(0x61751D6A, 0x771B9888) == 1); /* Smash Remix 2.0.1 +EXTRA: its readme asks for 1 */
	CHECK(gamedb_count_per_op(0x916B8B5B, 0x780B85A4) == 1); /* Super Smash Bros. (U) */
	CHECK(gamedb_count_per_op(0x635A2BFF, 0x8B022326) == 0); /* Super Mario 64: not listed, Not64's own 2 */
}

/* X deletes a game (Zach, 2026-10-25): marked, it stays in the array for the
 * loader's indices but is in no list, and gives no letter games. */
static void a_deleted_game_is_in_no_list(void)
{
	const char *names[] = { "Banjo-Kazooie (USA).z64", "Blast Corps (USA).z64" };
	library_game g[2];
	for (int i = 0; i < 2; i++)
		library_game_init(&g[i], names[i]);
	g[0].favorite = 1;
	g[0].recent = 1;
	g[0].deleted = 1;
	int shown[2], has[LIBRARY_CATEGORIES];
	CHECK(library_show(g, 2, LIBRARY_ALL, shown) == 1 && shown[0] == 1);
	CHECK(library_show(g, 2, LIBRARY_FAVORITES, shown) == 0);
	CHECK(library_show(g, 2, LIBRARY_RECENT, shown) == 0);
	CHECK(library_show(g, 2, library_category_of("B"), shown) == 1 && shown[0] == 1);
	library_categories(g, 2, has);
	CHECK(!has[LIBRARY_FAVORITES] && !has[LIBRARY_RECENT] && has[LIBRARY_ALL] && has[library_category_of("B")]);
	g[1].deleted = 1;
	library_categories(g, 2, has);
	CHECK(!has[LIBRARY_ALL] && !has[library_category_of("B")]);
}

#define RUN(fn) do { g_run++; int before = g_failed; fn(); if (g_failed == before) printf("  ok   %s\n", #fn); } while (0)

int main(void)
{
	setvbuf(stdout, NULL, _IOLBF, 0);
	RUN(read_a_z64_header);
	RUN(read_byte_swapped_and_little_endian_headers);
	RUN(not_a_rom_is_refused);
	RUN(titles_come_from_file_names);
	RUN(games_sort_a_to_z_ignoring_case_with_symbols_first);
	RUN(the_letter_bar_is_favorites_recent_all_then_hash_then_a_to_z);
	RUN(recent_shows_the_games_played_last_a_to_z);
	RUN(favorites_show_the_starred_games_and_keep_the_rest_of_the_file);
	RUN(a_letter_shows_its_games_and_all_shows_every_game);
	RUN(two_files_of_one_game_are_found);
	RUN(the_last_game_is_found_by_file_name);
	RUN(a_deleted_game_is_in_no_list);
	RUN(paks_follow_the_game_not_the_rumble_switch);
	RUN(counter_factors_come_from_the_catalogue);
	printf("%d of %d failed\n", g_failed, g_run);
	return g_failed != 0;
}
