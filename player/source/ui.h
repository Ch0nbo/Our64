/* Our64 player: Our64's own screens (ui_wiiu.cpp), drawn with libgui on the
 * graphics core. Called from the CPU core or the main thread; each call runs
 * on the graphics core (video.cpp) and returns when it is done.
 *
 * Nothing is drawn over a running game except controller notices at the top
 * (Zach, 2026-10-16). Every other screen shows outside play: before the game
 * starts, while it is frozen, or while Our64 closes. */
#ifndef OUR64_UI_H
#define OUR64_UI_H
#include "notices.h"
#include "prompt.h"
#include "pause.h"
#include "library.h"
#include "setup.h"
#ifdef __cplusplus
extern "C" {
#endif

/* Notice keys: one per N64 port, so a port's newest news replaces its older. */
#define UI_KEY_PORT(port) (port)
#define UI_KEY_BATTERY(id) (16 + (id))
#define UI_KEY_DEV 32 /* the developer's own switches */

enum {
	UI_LOADING,  /* headline and portrait while the ROM is read */
	UI_PROMPT,   /* How are you playing? */
	UI_PAUSED,   /* the game frozen, waiting for a controller: its last frame dimmed, and the notices */
	UI_CLOSING,  /* SAVING... over the last frame, dimmed, while Our64 writes before leaving */
	UI_MESSAGE,  /* a full screen of words: an error, or a write that failed while closing */
	UI_PAUSE,    /* the pause menu, save state screen or a confirm dialog, over the frozen game */
	UI_ROMS,     /* Our ROMs */
	UI_SETUP,    /* Our Settings, Our Controllers, Change order or Edit buttons */
};

/* Our ROMs (library_wiiu.c fills it): the letter bar, and the category's
 * covers stacked like a drawer with the selected one pulled out (Zach,
 * 2026-10-17). Covers are decoded on the CPU core into RGBA; the graphics
 * core turns each into a texture when its gen changes. */
#define ROMS_ART 10
/* The covers, for touch: the selected one between ROMS_COVER_LEFT and
 * _RIGHT, the next ones out beside it, the second ones out past ROMS_OUTER_
 * LEFT and _RIGHT, all between these heights. */
#define ROMS_STAGE_TOP 60
#define ROMS_STAGE_BOTTOM 388
#define ROMS_COVER_LEFT 233
#define ROMS_COVER_RIGHT 621
#define ROMS_OUTER_LEFT 98
#define ROMS_OUTER_RIGHT 756
/* The list's drawer, for touch: the selected cover between these heights, left of ROMS_LIST_RIGHT. */
#define ROMS_LIST_COVER_TOP 218
#define ROMS_LIST_COVER_BOTTOM 350
#define ROMS_LIST_RIGHT 390
/* The ROM folder (Settings > Games): read, its USB drive not found, or the folder itself gone. */
enum { ROMS_FOUND, ROMS_NO_DRIVE, ROMS_NO_FOLDER };
typedef struct {
	const library_game *games;
	const int *shown;        /* the category's games in list order */
	int count;               /* shown */
	int total;               /* every game on the card */
	int sel;                 /* position in shown */
	float pos;               /* where the drawer is: slides towards sel */
	int category;            /* the letter bar: LIBRARY_FAVORITES, LIBRARY_RECENT, LIBRARY_ALL, LIBRARY_HASH, then A-Z */
	int bar_focus;           /* left/right move along the bar, not the covers (up from the covers) */
	int has[LIBRARY_CATEGORIES];
	struct {
		int game;            /* index into games, -1 empty */
		const uint8_t *rgba;
		int w, h;
		unsigned gen;
	} art[ROMS_ART];
	char notice[160];        /* under the header until the first press */
	time_t now;
	int us;
	int glyphs;
	int missing;             /* ROMS_: the ROM folder can't be read, and why */
	int usb;                 /* the ROM folder is on a USB drive: Delete is off */
	char folder[400];        /* the folder in use as a full path, for people ("SD card: /wiiu/our64/roms"), whole */
} roms_view;

/* Loading (UI_LOADING): the game, a bar and the step. */
typedef struct {
	char step[96];
	float progress;          /* 0..1, across every step of the load */
	char stopped[256];       /* the autosave cannot be used: why (A Start, B Back) */
	int game;                /* its cover, beside the portrait, from Our ROMs' art (ui_view.roms); -1 none */
} loading_view;

/* The setup screens (setup_wiiu.c fills it). */
enum { SETUP_SETTINGS, SETUP_CONTROLLERS, SETUP_ORDER, SETUP_MAPPING, SETUP_THEME, SETUP_PICTURE, SETUP_GAMES, SETUP_BROWSE, SETUP_FETCH };
typedef struct {
	int screen;              /* SETUP_ */
	settings_ui settings;
	picture_ui picture;
	look_ui look;
	games_ui games;
	browse_ui browse;        /* about 80 KB: setup_view is a static, never on the stack */
	controllers_ui controllers;
	order_ui order;
	mapping_ui mapping;
	char line[400];          /* what the last action did ("ROM folder: " and a full path) */
	char game[64];           /* a paused game, "" from Our ROMs */
	int glyphs;
} setup_view;

/* What the pause screens show (pause_wiiu.c fills it; it outlives each ui_show). */
typedef struct {
	pause_ui ui;
	char title[64];          /* the game */
	char autosave[4][128];   /* the menu's autosave line (pause_autosave_lines) */
	int autosave_lines, autosave_red;
	char busy[96];           /* while ui.busy: what is happening ("Saving to Slot 2...") */
	int busy_card;           /* the card being written, -1 */
	const uint16_t *thumb[CARDS]; /* each card's picture, THUMB_W x THUMB_H RGB565; NULL: none */
	unsigned thumb_gen[CARDS];    /* changes when a picture changes */
	const uint16_t *now_thumb;    /* the paused game's */
	unsigned now_gen;
	time_t now;
	int us;                  /* US clock and dates */
	int glyphs;              /* INPUT_ type pressed last, for the legend */
} pause_view;

typedef struct {
	int screen;
	char title[64];    /* the game */
	/* UI_PROMPT */
	prompt prompt;
	char line[256];    /* what A will do */
	char warning[128]; /* above the cards; "" for none */
	int glyphs;        /* INPUT_ type of the controller pressed last, for the legend; -1 GamePad */
	/* UI_MESSAGE */
	char headline[64], body[320], code[16];
	int failure;       /* red, not cream */
	char legend[64];   /* the legend's words, e.g. "Wii U Menu" for HOME */
	/* UI_PAUSE */
	const pause_view *pause;
	/* UI_ROMS, UI_SETUP, UI_LOADING */
	const roms_view *roms;
	const setup_view *setup;
	loading_view loading;
	/* Any screen: "Exit to the Wii U Menu?" over it (pause.h's exit_ask) */
	int exit_ask;      /* 1 while it asks */
	int exit_focus;    /* 0 Cancel, 1 Exit */
	int exit_glyphs;   /* INPUT_ type for its legend */
	char ask_title[96], ask_body[160], ask_action[24]; /* another question in its place ("Delete ...?"); empty: Exit */
	/* UI_CLOSING */
	int nothing_to_save; /* the pause menu already wrote everything: CLOSING..., not SAVING... */
} ui_view;

void ui_show(const ui_view *v); /* one frame on every active screen */
void ui_show_exit_ask(int focus, int glyphs); /* the last screen again, with the Exit question over it */
void ui_show_ask(const char *title, const char *body, const char *action, int focus, int glyphs); /* another question there */
void ui_redraw(void);           /* the last view again */
void ui_resume(void);           /* the game runs again: its frames show, and the next freeze takes a new one */
/* From the prompt's choice on: which screens light up and play sound. Before
 * it, both. TV only turns the GamePad screen off; GamePad only puts a card on
 * the TV saying where the game is. */
void ui_set_mode(int mode, const char *title);
int  ui_mode(void);
void ui_restore_screens(void);  /* leaving: the GamePad screen back on */
void ui_tv_title(const char *title); /* GamePad only: the TV card names this ("Our ROMs", a game) */
/* The frozen game's picture for a save state: its 4:3 middle, THUMB_W x
 * THUMB_H RGB565 (all zero when there is no frame). After a UI_PAUSE show. */
void ui_thumbnail(uint16_t *out);
/* Controller notices over the game (notices.h); any core. */
void ui_notice(int key, int kind, double secs, const char *line1, const char *line2);
void ui_notice_clear(int key);

#ifdef __cplusplus
}
#endif
#endif
