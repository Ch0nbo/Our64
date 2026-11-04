/* Our64 player: the game list's games: what a ROM's header says, the order
 * they are listed in, their letters for the A-Z index, and search. Plain C,
 * so tests/ runs it on the PC; reading the SD card and drawing are
 * library_wiiu.c's and ui_wiiu.cpp's business. */
#ifndef OUR64_LIBRARY_H
#define OUR64_LIBRARY_H
#include <stdint.h>
#include <time.h>
#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
	uint32_t crc1, crc2;
	char country;
	char id[64];  /* saves_game_id: header name + CRC1, what saves and states follow */
} library_rom;

/* The first 64 bytes of a ROM file in any byte order (.z64, .v64, .n64) and
 * the file's size. 0 when it is an N64 ROM. */
int library_header(const unsigned char *first64, unsigned long size, library_rom *r);

typedef struct {
	char file[128];   /* the name in roms/ */
	char title[64];   /* for people: the file name without tags and extension */
	library_rom rom;
	int readable;     /* the header was read and is an N64 ROM's */
	/* filled by the console side */
	int known;        /* the header and saves have been read (readable and below mean something) */
	time_t autosave;  /* when its newest autosave was written (last played), 0 none */
	int autosave_ok;  /* that autosave can be used */
	int states;       /* save states in its four slots */
	int art;          /* sd:/wiiu/our64/art/<id>.png exists */
	int recent;       /* among the games played last (the Recent tab) */
	int favorite;     /* the player starred it (the Favorites tab) */
	int deleted;      /* moved to the trash: listed nowhere until the next scan drops it */
} library_game;

void library_game_init(library_game *g, const char *file);
/* The letter bar (Zach, 2026-10-17): Favorites, a star, first (2026-10-25),
 * then Recent (2026-10-18), All, #, and A to Z. A title's category is its
 * first letter, or # for a digit or a symbol. Recent lists the games played
 * last, Favorites the starred ones, A to Z like the rest. */
#define LIBRARY_CATEGORIES 30
enum { LIBRARY_FAVORITES, LIBRARY_RECENT, LIBRARY_ALL, LIBRARY_HASH };
int  library_category_of(const char *title);     /* LIBRARY_HASH for #, the 26 after it for A-Z */
const char *library_category_label(int category, char *out, unsigned size); /* "★", "Recent", "All", "#", "A" */
/* Marks the games whose files are among recent (k names), clears the rest. */
void library_mark_recent(library_game *g, int n, const char (*recent)[128], int k);
/* Favorites: config/favorites.txt, one ROM file name per line, as
 * library_game.file. Names whose files are not on the card now stay in the
 * file. library_favorites_read loads it (the number read, -1 when there is
 * none); library_mark_favorites marks the games among them;
 * library_favorite_toggle stars or unstars a game (its new state);
 * library_favorites_text writes the file's text into out, size bytes (its
 * length; with out NULL, only the length). */
int  library_favorites_read(const char *path);
void library_mark_favorites(library_game *g, int n);
int  library_favorite_toggle(library_game *g);
int  library_favorites_text(char *out, unsigned size);
void library_bar_rect(int category, int r[4]); /* the letter bar's tab, under the header */
int  library_bar_hit(int x, int y);            /* the category tapped, -1 */
/* In the bar's order: # first, then A to Z, ignoring case. */
void library_sort(library_game *g, int n);
/* The games of a category, in list order, as indices into g; the count. A
 * deleted game is in none, and gives no category games. */
int  library_show(const library_game *g, int n, int category, int *shown);
void library_categories(const library_game *g, int n, int has[LIBRARY_CATEGORIES]); /* has[LIBRARY_ALL]: any game */
/* The next category with games in dir (+1 right, -1 left); this one at the ends. */
int  library_next_category(const int has[LIBRARY_CATEGORIES], int category, int dir);

int  library_shares(const library_game *g, int n, int i); /* another file with the same game id, -1 */
int  library_find(const library_game *g, int n, const char *file); /* by file name, ignoring case; -1 */

/* A ROM file's name: .z64, .n64 or .v64, ignoring case, and not a Mac's
 * "._" file beside it (AppleDouble, written on FAT drives). Our ROMs and the
 * ROM folder browser's count both go by it. */
int  library_is_rom(const char *name);
/* s cut to at most max characters (UTF-8, max >= 3) with "…" in the middle,
 * so a long folder's drive and its last folder both stay in view; s as it is
 * when it fits. out holds size bytes and is cut between characters too. */
void library_cut_middle(const char *s, int max, char *out, unsigned size);

#ifdef __cplusplus
}
#endif
#endif
