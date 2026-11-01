/* Our64 player: the game list's games (see library.h). */
#include "library.h"
#include "theme.h"
#include "prompt.h"
#include "saves.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static uint32_t be32(const unsigned char *p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }

int library_header(const unsigned char *first64, unsigned long size, library_rom *r)
{
	unsigned char h[64];
	uint32_t magic = be32(first64);
	memset(r, 0, sizeof(*r));
	if (size < 0x101000) /* the header and boot code take 4 KB; no game fits in less than 1 MB */
		return -1;
	for (int i = 0; i < 64; i++) {
		switch (magic) {
		case 0x80371240: h[i] = first64[i]; break;          /* .z64 */
		case 0x37804012: h[i] = first64[i ^ 1]; break;      /* .v64: bytes swapped in pairs */
		case 0x40123780: h[i] = first64[i ^ 3]; break;      /* .n64: little-endian words */
		default: return -1;
		}
	}
	r->crc1 = be32(h + 16);
	r->crc2 = be32(h + 20);
	r->country = (char)h[62];
	saves_game_id(r->id, sizeof(r->id), h + 32, r->crc1);
	return 0;
}

void library_game_init(library_game *g, const char *file)
{
	memset(g, 0, sizeof(*g));
	snprintf(g->file, sizeof(g->file), "%s", file);
	ui_game_title(g->title, sizeof(g->title), file);
}

int library_category_of(const char *title)
{
	unsigned char c = (unsigned char)ui_sort_title(title)[0];
	return isalpha(c) ? LIBRARY_HASH + 1 + toupper(c) - 'A' : LIBRARY_HASH;
}

const char *library_category_label(int category, char *out, unsigned size)
{
	if (category <= LIBRARY_FAVORITES)
		snprintf(out, size, "\u2605"); /* the system font's black star */
	else if (category == LIBRARY_RECENT)
		snprintf(out, size, "Recent");
	else if (category == LIBRARY_ALL)
		snprintf(out, size, "All");
	else if (category == LIBRARY_HASH)
		snprintf(out, size, "#");
	else
		snprintf(out, size, "%c", 'A' + category - LIBRARY_HASH - 1);
	return out;
}

void library_mark_recent(library_game *g, int n, const char (*recent)[128], int k)
{
	for (int i = 0; i < n; i++) {
		g[i].recent = 0;
		for (int r = 0; r < k && recent[r][0]; r++)
			if (!strcasecmp(g[i].file, recent[r]))
				g[i].recent = 1;
	}
}

/* Favorites, read once; the games' flags follow it. */
#define FAVORITES_MAX 2000
static char (*g_favorites)[128];
static int g_favorite_count;

static int favorite_at(const char *file)
{
	for (int i = 0; i < g_favorite_count; i++)
		if (!strcasecmp(g_favorites[i], file))
			return i;
	return -1;
}

int library_favorites_read(const char *path)
{
	g_favorite_count = 0;
	if (!g_favorites && !(g_favorites = malloc(FAVORITES_MAX * sizeof(*g_favorites))))
		return -1;
	FILE *f = fopen(path, "r");
	if (!f)
		return -1;
	char line[300];
	while (g_favorite_count < FAVORITES_MAX && fgets(line, sizeof(line), f)) {
		line[strcspn(line, "\r\n")] = 0;
		if (line[0] && line[0] != '#' && favorite_at(line) < 0)
			snprintf(g_favorites[g_favorite_count++], sizeof(g_favorites[0]), "%.127s", line);
	}
	fclose(f);
	return g_favorite_count;
}

void library_mark_favorites(library_game *g, int n)
{
	for (int i = 0; i < n; i++)
		g[i].favorite = favorite_at(g[i].file) >= 0;
}

int library_favorite_toggle(library_game *g)
{
	if (!g_favorites && !(g_favorites = malloc(FAVORITES_MAX * sizeof(*g_favorites))))
		return g->favorite;
	const int at = favorite_at(g->file);
	if (at >= 0) {
		memmove(g_favorites[at], g_favorites[at + 1], (size_t)(g_favorite_count - at - 1) * sizeof(g_favorites[0]));
		g_favorite_count--;
	} else if (g_favorite_count < FAVORITES_MAX) {
		snprintf(g_favorites[g_favorite_count++], sizeof(g_favorites[0]), "%s", g->file);
	}
	return g->favorite = at < 0 && favorite_at(g->file) >= 0;
}

int library_favorites_text(char *out, unsigned size)
{
	int n = snprintf(out, out ? size : 0, "# Our64 favorites: one ROM file per line.\n");
	for (int i = 0; i < g_favorite_count; i++)
		n += snprintf(out ? out + n : NULL, out && (unsigned)n < size ? size - n : 0, "%s\n", g_favorites[i]);
	return n;
}

/* The star first, then Recent and All (Zach, 2026-10-25), as ui/'s bar is
 * sized (2026-10-24): the letters 24 apart, 25 until the star came. */
#define BAR_Y 12       /* the row of covers: at the top, no header (2026-10-30) */
#define BAR_Y_LIST 126 /* the list: under the header */
#define BAR_H 32
#define BAR_STAR_W 34
#define BAR_RECENT_W 84
#define BAR_ALL_W 54
#define BAR_STEP 24

void library_bar_rect(int category, int r[4])
{
	const int x0 = (854 - (BAR_STAR_W + BAR_RECENT_W + BAR_ALL_W + (LIBRARY_CATEGORIES - LIBRARY_HASH) * BAR_STEP)) / 2;
	r[0] = category == LIBRARY_FAVORITES ? x0
	     : category == LIBRARY_RECENT ? x0 + BAR_STAR_W
	     : category == LIBRARY_ALL ? x0 + BAR_STAR_W + BAR_RECENT_W
	     : x0 + BAR_STAR_W + BAR_RECENT_W + BAR_ALL_W + (category - LIBRARY_HASH) * BAR_STEP;
	r[1] = theme_roms_list() ? BAR_Y_LIST : BAR_Y;
	r[2] = category == LIBRARY_FAVORITES ? BAR_STAR_W : category == LIBRARY_RECENT ? BAR_RECENT_W
	     : category == LIBRARY_ALL ? BAR_ALL_W : BAR_STEP;
	r[3] = BAR_H;
}

int library_bar_hit(int x, int y)
{
	for (int c = 0; c < LIBRARY_CATEGORIES; c++) {
		int r[4];
		library_bar_rect(c, r);
		if (x >= r[0] && x < r[0] + r[2] && y >= r[1] - 6 && y < r[1] + r[3] + 6)
			return c;
	}
	return -1;
}

static int compare(const void *a, const void *b)
{
	const library_game *x = a, *y = b;
	int cx = library_category_of(x->title), cy = library_category_of(y->title);
	if (cx != cy)
		return cx - cy;
	int c = strcasecmp(ui_sort_title(x->title), ui_sort_title(y->title));
	return c ? c : strcmp(x->file, y->file);
}

void library_sort(library_game *g, int n) { qsort(g, (size_t)n, sizeof(*g), compare); }

int library_show(const library_game *g, int n, int category, int *shown)
{
	int k = 0;
	for (int i = 0; i < n; i++)
		if (!g[i].deleted && (category == LIBRARY_ALL || (category == LIBRARY_RECENT ? g[i].recent
		                                : category == LIBRARY_FAVORITES ? g[i].favorite
		                                                                : library_category_of(g[i].title) == category)))
			shown[k++] = i;
	return k;
}

void library_categories(const library_game *g, int n, int has[LIBRARY_CATEGORIES])
{
	memset(has, 0, sizeof(int) * LIBRARY_CATEGORIES);
	for (int i = 0; i < n; i++) {
		if (g[i].deleted)
			continue;
		has[LIBRARY_ALL] = 1;
		has[library_category_of(g[i].title)] = 1;
		has[LIBRARY_RECENT] |= g[i].recent;
		has[LIBRARY_FAVORITES] |= g[i].favorite;
	}
}

/* The bar wraps: left from its first category goes to its last letter with
 * games, right from the last to the first (Zach, 2026-10-20). */
int library_next_category(const int has[LIBRARY_CATEGORIES], int category, int dir)
{
	for (int k = 1; k < LIBRARY_CATEGORIES; k++) {
		int c = ((category + dir * k) % LIBRARY_CATEGORIES + LIBRARY_CATEGORIES) % LIBRARY_CATEGORIES;
		if (has[c])
			return c;
	}
	return category;
}

int library_shares(const library_game *g, int n, int i)
{
	for (int k = 0; k < n; k++)
		if (k != i && g[k].readable && g[i].readable && !strcmp(g[k].rom.id, g[i].rom.id))
			return k;
	return -1;
}

int library_find(const library_game *g, int n, const char *file)
{
	for (int i = 0; i < n; i++)
		if (!strcasecmp(g[i].file, file))
			return i;
	return -1;
}
