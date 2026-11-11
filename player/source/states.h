/* Our64 player: save state files and resume points. Plain C with stdio and
 * zlib, so tests/ runs it on the PC. What goes inside a state is
 * states_wiiu.c's business; here it is bytes. */
#ifndef OUR64_STATES_H
#define OUR64_STATES_H
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* A file is a 64-byte big-endian header ("O64S", version, the ROM's CRC1
 * and CRC2, counter, raw size, packed size, CRC-32 of the packed bytes, the
 * console time it was saved, thumbnail bytes, slot, before), the thumbnail
 * when there is one, then the state, zlib-compressed. Bump the version
 * whenever the layout of the state itself changes: older files are then
 * refused, never misread, unless the caller still knows their layout
 * (STATEFILE_OLDEST: version 3 resume points still load). */
#define STATEFILE_VERSION 4 /* 4: no in-game save memory, 64-byte header; 3: save memory after RDRAM, 32-byte header;
                               2: RDRAM first; 1 had states captured mid-instruction */
#define STATEFILE_OLDEST  3

/* The picture on a save state's card: the game's frame, 4:3, RGB565. */
#define THUMB_W 160
#define THUMB_H 120
#define THUMB_BYTES (THUMB_W * THUMB_H * 2)

typedef struct {
	uint32_t crc1, crc2; /* the ROM it belongs to */
	uint32_t counter;    /* resume points: higher is newer */
	uint32_t saved_at;   /* console clock, seconds; 0 when unknown (version 3) */
	uint8_t slot;        /* the slot (1-4) it was saved in; 0 for none */
	uint8_t before;      /* the Undo card: UNDO_LOAD or UNDO_RESET; 0 for a slot's own copy */
	uint8_t has_thumb;   /* reads: the file has a thumbnail */
	uint32_t version;    /* reads: the file's version */
} state_info;

enum {
	STATEFILE_OK,
	STATEFILE_MISSING,
	STATEFILE_DAMAGED,       /* short, bad CRC, or will not decompress to its size */
	STATEFILE_OTHER_VERSION, /* written by an Our64 with another state layout */
	STATEFILE_OTHER_GAME,
	STATEFILE_TOO_BIG,       /* bigger than the caller's buffer */
	STATEFILE_WRITE_FAILED,
	STATEFILE_BACKUP_FAILED, /* the old copy could not be kept, so nothing changed */
	STATEFILE_ABORTED,       /* statefile_abort was set: packing or writing stopped part way */
};

/* Set from another thread to stop statefile_pack and statefile_store_over
 * between slices (the watchdog's guard against a resume point job that does
 * not end, 2026-11-10). The caller clears it before it starts. */
extern volatile int statefile_abort;
extern unsigned statefile_store_slice; /* the write's slice: 64 KB */

/* Bytes of work space statefile_write needs for raw_size bytes of state. */
unsigned statefile_work_size(unsigned raw_size);
/* thumb: THUMB_BYTES, or NULL for none. */
int statefile_write(const char *path, const state_info *info, const void *raw, unsigned raw_size,
                    const void *thumb, void *work, unsigned work_size);
/* statefile_write in its two steps, to time them apart: pack compresses into
 * work and fills the header (bytes: header and state, no thumbnail); store
 * writes them, with the thumbnail when thumb is not NULL. */
int statefile_pack(const state_info *info, const void *raw, unsigned raw_size, int thumb, void *work,
                   unsigned work_size, unsigned *bytes);
int statefile_store(const char *path, const void *work, unsigned bytes, const void *thumb);
/* store for resume points, with no thumbnail: over the file in place, which
 * keeps room after the state (see states.c). A torn write fails the CRC, and
 * resume_load takes the other file. */
int statefile_store_over(const char *path, const void *work, unsigned bytes);
int statefile_read(const char *path, uint32_t crc1, uint32_t crc2, state_info *info,
                   void *raw, unsigned raw_cap, unsigned *raw_size);
/* The header and thumbnail alone (thumb: THUMB_BYTES, filled when the file
 * has one; NULL skips it), for a card. The file must be at least the size its header gives; the state's
 * CRC is checked only by a full read. */
int statefile_peek(const char *path, uint32_t crc1, uint32_t crc2, state_info *info, void *thumb);

/* Save state cards in a game's folder: three slots the player fills and one
 * Undo card Our64 keeps: the game as it was before the last overwrite, load
 * or reset, whichever came last (Zach, 2026-10-17). */
enum { CARD_SLOT1, CARD_SLOT2, CARD_SLOT3, CARD_UNDO, CARDS };
#define CARD_SLOTS 3
enum { UNDO_SAVE = 1, UNDO_LOAD, UNDO_RESET };
const char *card_file(int card); /* "slot1.o64s", "undo.o64s" */
int card_undo_kind(const state_info *info); /* what the Undo card undoes: UNDO_ */
/* Writes <file>.tmp, then puts it in place. A slot's old copy becomes the
 * Undo card when keep_old is set (it could be loaded), and is deleted
 * otherwise. On failure nothing changes. */
int card_save(const char *dir, int card, const state_info *info, int keep_old, const void *raw, unsigned raw_size,
              const void *thumb, void *work, unsigned work_size);
/* A card's header and thumbnail. A complete <file>.tmp with no file (a power
 * cut between the renames) is put in place first; a partial one is deleted. */
int card_peek(const char *dir, int card, uint32_t crc1, uint32_t crc2, state_info *info, void *thumb);
int card_read(const char *dir, int card, uint32_t crc1, uint32_t crc2, state_info *info, void *raw,
              unsigned raw_cap, unsigned *raw_size);
int card_delete(const char *dir, int card); /* 0 when the card is empty after */

/* Resume points: two files that take turns, so a power cut while one is
 * written leaves the other. resume_load reads the newest good one and
 * returns STATEFILE_OK, else the newest file's error (STATEFILE_MISSING when
 * there are none). Either way it says which file to write next (the one not
 * loaded) and the counter to give it (above every counter seen). */
typedef struct {
	int next_file;         /* 0: a, 1: b */
	uint32_t next_counter;
	int loaded_file;       /* -1 when nothing loaded */
} resume_plan;
int resume_load(const char *a, const char *b, uint32_t crc1, uint32_t crc2, resume_plan *plan,
                void *raw, unsigned raw_cap, unsigned *raw_size);
void resume_forget(const char *a, const char *b); /* deletes both */
/* Auto-Resume turned off (Settings > Games): every game's resume points go,
 * resume-a/b.o64s and the set-aside resume-*.unused.o64s, under
 * <root>/states/<game id>/. Save state slots and Undo stay. Returns the files
 * deleted. resume_point_file: name is one of those files. */
int  resume_forget_all(const char *root, void (*each)(int done, int total, void *ctx), void *ctx); /* each: per game with resume points, or NULL */
int  resume_games_with_points(const char *root); /* games that have resume points */
int  resume_point_file(const char *name);

/* When a resume point is captured. Due every 60 s, and at once when an
 * in-game save waits for one (the resume point goes to the SD first, so the
 * two never disagree). A due capture happens at the start of a VI only when
 * the time that VI would spend waiting anyway is at least 1.2x the last
 * capture's cost (resume_fits): the copy uses time the CPU core would sleep,
 * so it costs no speed. A save that waited 5 s with no chance gives up: the
 * caller deletes the resume points instead, so an older one can never undo
 * a newer save. */
typedef struct {
	double last_us;  /* last capture */
	double cost_us;  /* what the last capture took */
} resume_timer;
enum { RESUME_WAIT, RESUME_DUE, RESUME_GIVE_UP };
extern double resume_every_us; /* 60 s; a test switch can set it */
void resume_timer_init(resume_timer *t, double now_us);
/* save_since_us: when an in-game save started waiting, < 0 when none. */
int  resume_decide(const resume_timer *t, double now_us, double save_since_us, int writer_busy);
int  resume_fits(const resume_timer *t, double idle_us);
void resume_captured(resume_timer *t, double now_us, double cost_us);

#ifdef __cplusplus
}
#endif
#endif
