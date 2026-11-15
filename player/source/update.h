/* Our64 player: the updater's portable parts (Settings > Update). Plain C,
 * so tests/ runs it on the PC; the downloads and the files are
 * update_wiiu.c's business, the page setup.c's.
 *
 * A release on GitHub holds, beside the full zip, the files an installed
 * Our64 takes by itself:
 *   our64.wuhb            the app
 *   GLideN64.custom.ini   per-game graphics settings, always taken with it
 *   shaders.zip           games/<game>.pack and .keys, the pre-built shaders;
 *                         taken only when the player asks (Include shaders)
 * GitHub's releases API names each with its size and SHA-256
 * (update_parse_release); what is downloaded is checked against both. */
#ifndef OUR64_UPDATE_H
#define OUR64_UPDATE_H
#include <stdio.h>
#ifdef __cplusplus
extern "C" {
#endif

#define UPDATE_APP_NAME "our64.wuhb"
#define UPDATE_INI_NAME "GLideN64.custom.ini"
#define UPDATE_SHADERS_NAME "shaders.zip"

typedef struct {
	char url[512];   /* "" when the release has no such file */
	char sha256[65]; /* lower-case hex, "" when the server names none */
	unsigned size;
} update_asset;
typedef struct {
	char version[24]; /* the tag without its "v": "1.3.0" */
	update_asset app, ini, shaders;
} update_release;

/* "1.2.0" against "v1.10.0": below 0, 0 or above 0, by the numbers between
 * the dots; a leading "v" and whatever follows the numbers ("-test") are
 * left out. */
int update_version_cmp(const char *a, const char *b);

/* GitHub's answer for releases/latest. 0, or -1 when it is not that (no tag,
 * or not JSON). A release without our64.wuhb parses: app.url is "". */
int update_parse_release(const char *json, update_release *out);

/* The release's notes (its "body", GitHub's Markdown) as plain lines for the
 * Release notes page: each at most width bytes, cut at spaces; headings
 * without their marks and with an empty line before them, "**" and "`"
 * gone, links as their words, list items under a bullet with the lines that
 * continue them indented. A table's row is one line, however long, with a
 * tab before each cell: the page lays the cells out in columns. The count of
 * bytes written, 0 when the release has no notes; notes that do not
 * fit end at the last whole line with "...". */
unsigned update_release_notes(const char *json, char *out, unsigned size, unsigned width);

/* ---- shaders.zip ---- */

typedef struct {
	char name[160];
	unsigned method, packed, size, crc, offset; /* offset: of its local header */
} update_zip_entry;
typedef struct {
	unsigned char *dir; /* the central directory */
	unsigned dir_size, count, at, index;
} update_zip;
int   update_zip_open(FILE *f, update_zip *z);               /* 0, or -1: not a zip Our64 reads */
int   update_zip_next(update_zip *z, update_zip_entry *e);   /* 1 while there is one */
void *update_zip_read(FILE *f, const update_zip_entry *e);   /* its bytes (free them), CRC checked; NULL: damaged or no memory */
void  update_zip_close(update_zip *z);

/* A game's shader pack from the release and the one on the card, as one:
 * every pair of the new one, then the card's pairs it does not have (pairs
 * the player's own play built; the graphics core drops those it no longer
 * uses). old may be NULL or damaged: the result is then the new pack. NULL:
 * new_pack is not a pack, or no memory. Free the result. *added: the pairs
 * the card's pack lacked; 0 means the card's file can stay as it is (each
 * write into a full folder takes the card seconds: 4 files took 12 s on the
 * console, 2026-11-15). */
unsigned char *update_pack_merge(const unsigned char *new_pack, unsigned new_size, const unsigned char *old_pack,
                                 unsigned old_size, unsigned *out_size, unsigned *added);
/* The same for a game's .keys (combiner keys, one hex number a line, "#"
 * lines are comments): every key of both, sorted, under the new file's first
 * comment line. old may be NULL. Free the result. *added: the keys the
 * card's file lacked. */
char *update_keys_merge(const char *new_keys, const char *old_keys, unsigned *out_size, unsigned *added);

#ifdef __cplusplus
}
#endif
#endif
