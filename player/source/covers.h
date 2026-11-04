/* Our64 player: box art from libretro-thumbnails over plain HTTP, the
 * portable parts: the cover's file name and URL path, the request and the
 * reply (covers.c). Plain C. Every output is NUL-terminated inside size; one
 * that does not fit comes out "" (a cut name or path would ask for another
 * file). */
#ifndef OUR64_COVERS_H
#define OUR64_COVERS_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

#define COVERS_HOST "thumbnails.libretro.com"

/* libretro-thumbnails' file name for a title: each of & * / : ` < > ? \ | " becomes _
 * (RetroArch's gfx_thumbnail.c). */
void covers_thumb_name(const char *title, char *out, unsigned size);
/* "/Nintendo%20-%20Nintendo%2064/Named_Boxarts/<percent-encoded name>.png" */
void covers_path(const char *thumb_name, char *out, unsigned size);
/* The request: GET <path> HTTP/1.0, Host: thumbnails.libretro.com,
 * User-Agent: Our64/<version>, Connection: close. Its length, or -1 when it
 * does not fit. */
int covers_request(const char *path, char *out, unsigned size);
/* A whole reply of n bytes: 200 with a body -> 200, the body's offset and
 * length (Content-Length when given); else the status (404...), or -1 when
 * it is not HTTP, its headers never end or the body is shorter than its
 * Content-Length (a cut-off download). */
int covers_reply(const unsigned char *reply, unsigned n, unsigned *body, unsigned *body_n);
/* The second try: the ROM's file name without its folder and extension
 * ("roms/Banjo-Kazooie (USA).z64" -> "Banjo-Kazooie (USA)"). */
void covers_file_title(const char *file, char *out, unsigned size);

/* ---- Fetch covers (Settings > Games): the rules the console's fetch
 * follows, here so tests/ checks them ---- */

/* A reply bigger than this is not a cover (libretro's N64 covers are about
 * 350 KB). */
#define COVERS_MAX_REPLY (2u * 1024 * 1024)
/* Network failures in a row that end a fetch: the server is down or gone. */
#define COVERS_NET_TRIES 3

/* What one game's try came to. */
enum { COVERS_GOT_PNG, COVERS_NOT_FOUND, COVERS_NOT_PNG, COVERS_NET_FAILED };
/* covers_reply's status as an outcome: 200 COVERS_GOT_PNG (the body still has
 * to be a PNG), 404 and the other 4xx COVERS_NOT_FOUND, anything else (-1, a
 * cut-off or garbled reply, 5xx) COVERS_NET_FAILED. */
int covers_status_outcome(int status);
/* The first 8 bytes are PNG's signature. */
int covers_is_png(const unsigned char *body, unsigned n);
/* After each game: counts network failures in a row in *streak (any other
 * outcome ends the run); 1 when this one was the COVERS_NET_TRIES'th and the
 * fetch stops. */
int covers_server_gone(int *streak, int outcome);
/* The second title, after a 404 on table_title (covertitles_find's): the ROM
 * file's own (covers_file_title), when there was a table title and the file's
 * differs. 1 with out filled, else 0 and out "". */
int covers_second_title(const char *table_title, const char *file, char *out, unsigned size);

/* How a fetch ended. */
enum { COVERS_OK, COVERS_NO_NETWORK, COVERS_NO_SERVER, COVERS_CARD_FULL };
/* The line under Settings > Games' header after a fetch: "Fetched 9 covers.
 * 3 not found.", "Fetched 1 cover.", "No covers found.", "Stopped: fetched 2
 * covers." after B, or the failure's own line with its OUR- code. */
void covers_summary(int result, int stopped, int saved, int not_found, char *out, unsigned size);

/* One game to fetch a cover for, copied when the fetch starts. */
typedef struct {
	char id[64];      /* library_rom.id: the cover is <root>/art/<id>.png */
	uint32_t crc1, crc2;
	char country;
	char file[128];   /* the ROM's file name: the second title */
} covers_game;

/* ---- On the console (covers_wiiu.c): one fetch at a time, on a thread of
 * its own; every call below is the menu thread's ---- */

/* Starts fetching the n games' covers into <root>/art. 0 started; -1 when a
 * fetch is still running, or no memory or thread. */
int  covers_start(const char *root, const covers_game *games, int n);
/* 1 while the thread works; 0 once it has finished (or was never started). */
int  covers_running(void);
void covers_progress(int *done, int *total, int *saved); /* games tried, games, covers saved */
int  covers_not_found(void);       /* games tried with no cover found (404s and bad PNGs) */
/* The next game id whose cover was saved since the last call, NULL when none:
 * for library_cover_saved. Valid until the next covers_start. */
const char *covers_take_saved(void);
/* Asks the thread to stop (between games and inside every wait) and waits
 * for it to end. Also ends a fetch that finished: call it before
 * covers_result. */
void covers_stop(void);
int  covers_result(void);          /* COVERS_OK..., once stopped */

#ifdef __cplusplus
}
#endif
#endif
