/* PC tests for player/source/library.c: Our ROMs' games, their order and
 * the letter bar. */
#include "library.h"
#include "prompt.h"
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

static void rom_names_skip_a_macs_dot_underscore_files(void)
{
	CHECK(library_is_rom("Banjo.z64") && library_is_rom("Mario.N64") && library_is_rom("Zelda.v64"));
	CHECK(!library_is_rom("._Banjo.z64"));   /* AppleDouble, beside the ROM on a FAT drive */
	CHECK(library_is_rom(".Banjo.z64"));     /* only "._" is a Mac's */
	CHECK(!library_is_rom("Banjo.zip") && !library_is_rom("z64") && !library_is_rom("Banjo.z64.txt"));
}

static void a_long_folder_is_cut_in_the_middle(void)
{
	char out[64];
	library_cut_middle("SD card: /wiiu/our64/roms", 40, out, sizeof(out));
	CHECK(!strcmp(out, "SD card: /wiiu/our64/roms")); /* fits: as it is */
	library_cut_middle("USB GAMES: Games/Nintendo 64/USA", 21, out, sizeof(out));
	CHECK(!strcmp(out, "USB GAMES:\xE2\x80\xA6" "ndo 64/USA")); /* 10 before, 10 after: "…" is the 21st */
	library_cut_middle("abcdefghij", 5, out, sizeof(out));
	CHECK(!strcmp(out, "ab\xE2\x80\xA6ij"));
	/* characters, not bytes: "é" is two bytes and one character, and is never split */
	library_cut_middle("\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9", 5, out, sizeof(out));
	CHECK(!strcmp(out, "\xC3\xA9\xC3\xA9\xE2\x80\xA6\xC3\xA9\xC3\xA9"));
	library_cut_middle("ab\xC3\xA9", 10, out, 4); /* out too small: cut before the "é", not inside it */
	CHECK(!strcmp(out, "ab"));
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

/* Covers scaled to fit 640 x 360: libretro's 512 x 374 at 493 x 360, the
 * aspect kept, a plain color kept exactly, and the area averaged. */
static void covers_scale_by_area_to_fit(void)
{
	const int w = 512, h = 374;
	unsigned char *src = malloc((size_t)w * h * 4), *out = malloc(640 * 360 * 4);
	CHECK(src && out);
	for (int i = 0; i < w * h; i++) {
		src[i * 4] = 200; src[i * 4 + 1] = 30; src[i * 4 + 2] = 7; src[i * 4 + 3] = 255;
	}
	int ow = 0, oh = 0;
	CHECK(library_fit_rgba(src, w, h, 640, 360, out, &ow, &oh) == 0);
	CHECK(oh == 360 && ow == 493); /* 512 * 360 / 374 = 492.8 */
	for (int i = 0; i < ow * oh; i++)
		CHECK(out[i * 4] == 200 && out[i * 4 + 1] == 30 && out[i * 4 + 2] == 7 && out[i * 4 + 3] == 255);
	/* 8 x 8 checkers of two colors: each output pixel is the mean of the
	 * source area it covers, worked out here in doubles */
	for (int y = 0; y < h; y++)
		for (int x = 0; x < w; x++) {
			const int dark = ((x / 8) + (y / 8)) & 1;
			unsigned char *p = src + (y * w + x) * 4;
			p[0] = dark ? 10 : 240; p[1] = dark ? 200 : 20; p[2] = 0; p[3] = 255;
		}
	CHECK(library_fit_rgba(src, w, h, 640, 360, out, &ow, &oh) == 0);
	CHECK(ow == 493 && oh == 360);
	int worst = 0;
	for (int oy = 0; oy < oh; oy++)
		for (int ox = 0; ox < ow; ox++)
			for (int c = 0; c < 2; c++) {
				const double x0 = (double)ox * w / ow, x1 = (double)(ox + 1) * w / ow;
				const double y0 = (double)oy * h / oh, y1 = (double)(oy + 1) * h / oh;
				double sum = 0;
				for (int y = (int)y0; y < h && y < y1; y++)
					for (int x = (int)x0; x < w && x < x1; x++) {
						const double cx = (x + 1 < x1 ? x + 1 : x1) - (x > x0 ? x : x0);
						const double cy = (y + 1 < y1 ? y + 1 : y1) - (y > y0 ? y : y0);
						sum += src[(y * w + x) * 4 + c] * cx * cy;
					}
				const int want = (int)(sum / ((x1 - x0) * (y1 - y0)) + 0.5), got = out[(oy * ow + ox) * 4 + c];
				const int d = want > got ? want - got : got - want;
				worst = d > worst ? d : worst;
			}
	CHECK(worst <= 1);
	/* at half size, stripes one source pixel wide average to gray; the left
	 * half dark and the right half light stay apart */
	unsigned char *big = malloc(1280 * 720 * 4);
	CHECK(big);
	for (int y = 0; y < 720; y++)
		for (int x = 0; x < 1280; x++)
			for (int c = 0; c < 4; c++)
				big[(y * 1280 + x) * 4 + c] = c == 3 ? 255 : x < 640 ? (x & 1 ? 100 : 0) : 250;
	CHECK(library_fit_rgba(big, 1280, 720, 640, 360, out, &ow, &oh) == 0);
	CHECK(ow == 640 && oh == 360);
	const unsigned char *mid = out + (180 * ow + 100) * 4, *right = out + (180 * ow + 400) * 4;
	CHECK(mid[0] == 50 && mid[1] == 50 && mid[3] == 255);
	CHECK(right[0] == 250 && right[3] == 255);
	free(big);
	/* wider than it is tall: the width decides */
	unsigned char *wide = calloc(1280 * 400 * 4, 1);
	CHECK(wide);
	CHECK(library_fit_rgba(wide, 1280, 400, 640, 360, out, &ow, &oh) == 0);
	CHECK(ow == 640 && oh == 200);
	free(wide);
	/* one that fits is copied as it is */
	CHECK(library_fit_rgba(src, 300, 200, 640, 360, out, &ow, &oh) == 0);
	CHECK(ow == 300 && oh == 200 && !memcmp(out, src, 300 * 200 * 4));
	/* past 16 M pixels: refused */
	CHECK(library_fit_rgba(src, 5000, 5000, 640, 360, out, &ow, &oh) == -1);
	free(src);
	free(out);
}

#define RUN(fn) do { g_run++; int before = g_failed; fn(); if (g_failed == before) printf("  ok   %s\n", #fn); } while (0)

/* Settings > Games' Start on (2026-11-06). */
static void start_on_picks_the_first_tab(void)
{
	int has[LIBRARY_CATEGORIES] = { 0 };
	has[LIBRARY_ALL] = 1;
	CHECK(library_start_category(START_ALL, has, 1) == LIBRARY_ALL);
	CHECK(library_start_category(START_RECENT, has, 0) == LIBRARY_ALL);       /* nothing played */
	CHECK(library_start_category(START_FAVORITES, has, 0) == LIBRARY_ALL);    /* no favorites */
	has[LIBRARY_RECENT] = 1;
	CHECK(library_start_category(START_RECENT, has, 1) == LIBRARY_RECENT);
	CHECK(library_start_category(START_RECENT, has, 0) == LIBRARY_ALL);       /* the last game is not among them */
	CHECK(library_start_category(START_ALL, has, 1) == LIBRARY_ALL);
	CHECK(library_start_category(START_FAVORITES, has, 1) == LIBRARY_ALL);
	has[LIBRARY_FAVORITES] = 1;
	CHECK(library_start_category(START_FAVORITES, has, 0) == LIBRARY_FAVORITES); /* whatever the last game */
	CHECK(library_start_category(START_RECENT, has, 1) == LIBRARY_RECENT);
}

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
	RUN(rom_names_skip_a_macs_dot_underscore_files);
	RUN(a_long_folder_is_cut_in_the_middle);
	RUN(a_deleted_game_is_in_no_list);
	RUN(paks_follow_the_game_not_the_rumble_switch);
	RUN(counter_factors_come_from_the_catalogue);
	RUN(covers_scale_by_area_to_fit);
	RUN(start_on_picks_the_first_tab);
	printf("%d of %d failed\n", g_failed, g_run);
	return g_failed != 0;
}
