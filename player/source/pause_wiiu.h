/* Our64 player: the pause menu on the console. What states_wiiu.c (the
 * game's state and the SD card), pause_wiiu.c (the menu loop) and main.c
 * share. Everything here runs on the CPU core unless it says otherwise. */
#ifndef OUR64_PAUSE_WIIU_H
#define OUR64_PAUSE_WIIU_H
#include "pause.h"
#include "library.h"
#include "prompt.h"
#ifdef __cplusplus
extern "C" {
#endif

/* ---- states_wiiu.c ---- */
int  states_on(void);              /* save states and the autosave work this session */
/* HOME during play: the game is captured at the next instruction boundary,
 * its autosave and in-game saves go to the SD, and the pause menu runs
 * there with the game frozen (pause_run). */
void states_pause(void);
uint16_t *states_now_thumb(void);  /* the paused game's picture, THUMB_W x THUMB_H RGB565 */
/* A card's header and thumbnail (thumb: THUMB_BYTES). Returns 1 when it has a picture. */
int  states_card_scan(int card, pause_card *c, void *thumb);
/* SD writer jobs with the paused game: a slot (keep_old: its old copy
 * becomes the Undo card), or the Undo card itself (undo: UNDO_LOAD or
 * UNDO_RESET). Poll states_job_running. */
void states_card_save(int card, int keep_old, int undo);
int  states_job_running(void);
int  states_job_result(void);      /* STATEFILE_ of the last job */
int  states_card_read(int card);   /* into the load buffer, checked: STATEFILE_ */
int  states_card_delete(int card); /* 0 when the card is empty after */
void states_autosave(pause_autosave *a); /* for the menu's autosave line */
int  states_writing(void);         /* an autosave or in-game save is going to the SD */
/* What the menu chose that needs go() stopped (main.c, after go() returns). */
enum { ACTION_NONE, ACTION_LOAD, ACTION_RESET };
int  states_take_action(void);
void states_set_aside(void);       /* the autosave cannot be used: renamed, kept, not replaced */
void states_load_now(void);        /* the card read last becomes the game */
void states_after_reset(void);     /* an autosave of the fresh game at the first chance */
/* An autosave or in-game save reached the SD card or did not (writer thread). */
enum { WRITE_AUTOSAVE, WRITE_INGAME };
void states_write_done(int what, int ok);

/* ---- pause_wiiu.c ---- */
enum { PAUSE_RESUME, PAUSE_LOAD, PAUSE_RESET, PAUSE_EXIT, PAUSE_ROMS };
/* The pause menu, until the player resumes, loads, resets or leaves (for
 * the Wii U Menu or Our ROMs). captured: the game is in the pause buffer
 * (save states work). */
int  pause_run(int captured);
/* ---- setup_wiiu.c ---- */
void setup_start(const char *root, const our_settings *s); /* settings.cfg as read at launch */
const our_settings *setup_settings(void);
int  setup_save_settings(void);    /* 0 when written */
/* Our Settings, from the pause menu or Our ROMs. HOME there: settings_take_home is 1 after. */
void settings_run(int from_pause);
int  settings_take_home(void);
int  settings_take_rescan(void);   /* 1 once after Settings, from Our ROMs, changed the ROM folder */

/* ---- library_wiiu.c: Our ROMs ---- */
enum { LIB_PLAY, LIB_WIIU, LIB_SYSTEM };
int  library_scan(const char *root);   /* the count */
int  library_count(void);
const library_game *library_get(int i);
int  library_index_of(const char *file); /* -1 */
void library_path(int i, char *out, unsigned size);
void library_refresh(int i);            /* after playing: its autosave and save states again */
int  library_run(int *game, const char *notice);
int  library_is_rom(const char *name);  /* a .z64, .n64 or .v64 file name */
const char *library_folder_label(void); /* the ROM folder in use, for people: "SD card: wiiu/our64/roms" */
int  library_missing_covers(void);      /* readable games without a cover */

/* ---- main.c ---- */
int  player_closing(void);         /* the system asked Our64 to close (power button) */
/* A menu's wait between frames: the rest of a 60 Hz frame. ui_show already
 * waits for the screen, so a menu that draws each frame does not wait here. */
void player_menu_wait(void);
void player_paused(int on);        /* a menu holds the game: the watchdog expects no VIs */
/* The menu's Exit (or a power press) finished its writes. warned: the
 * player saw a failed write on the menu and left anyway. */
enum { LEAVE_WIIU, LEAVE_SYSTEM, LEAVE_ROMS };
void player_left(int where, int warned);
int  player_us(void);              /* US console: 12-hour clock, month first */
const char *player_title(void);    /* the game, for people */
void player_reset_machine(void);   /* the N64 as at power on; in-game save memory stays */

#ifdef __cplusplus
}
#endif
#endif
