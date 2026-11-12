/* Our64 player: the pause menu, the save state screen and their dialogs:
 * what is selected, what each press does, where things are for touch, and
 * every word on them. Plain C, so tests/ runs it on the PC; drawing is
 * ui_wiiu.cpp's business, the loop and the SD card pause_wiiu.c's.
 *
 * Three kinds of save are kept apart (Zach, 2026-10-16): the game's own
 * in-game saves, save states (three slots and the Undo card here), and the
 * autosave (the resume point). The words keep them apart too: "save state"
 * for what a slot holds, "in-game saves" for the game's own, "autosave" for
 * the resume point, which shares no word with Resume.
 *
 * Layout (Zach, 2026-10-17): six items in two columns of three, no
 * description area; HOME asks before leaving for the Wii U Menu. A game on
 * several discs (OurPS1's .m3u) has a seventh, Change disc, in the second
 * column's fourth place (every other item where it always is); while the
 * panel of what failed takes the room under the items, the items are three
 * columns of three and Change disc heads the third (hci review 2026-10-23). */
#ifndef OUR64_PAUSE_H
#define OUR64_PAUSE_H
#include <stdint.h>
#include <time.h>
#include "states.h"
#ifdef __cplusplus
extern "C" {
#endif

enum { PAUSE_MENU, PAUSE_SLOTS, PAUSE_ASK };
/* Two columns of three (Zach, 2026-10-17), the first column first: Resume,
 * Settings, Exit to Our ROMs; then Save state, Load state, Reset game.
 * Change disc only for a game on several discs (pause_ui.discs). */
/* Game options (2026-11-11): the running game's own settings, the first
 * column's third, above Exit. */
enum { ITEM_RESUME, ITEM_SETTINGS, ITEM_EXIT, ITEM_SAVE, ITEM_LOAD, ITEM_RESET, ITEM_DISC, ITEM_OPTIONS, ITEMS };
#define ITEM_ROWS 3 /* items in a column, without Change disc */
enum { SLOTS_SAVE, SLOTS_LOAD };
enum { CARD_EMPTY, CARD_FILLED, CARD_DAMAGED, CARD_OTHER_VERSION, CARD_OTHER_GAME };
/* Dialogs. ASK_HOME: "Exit to the Wii U Menu?". ASK_FAILED: what went wrong, one OK. */
enum { ASK_OVERWRITE, ASK_LOAD, ASK_RESET, ASK_DELETE, ASK_HOME, ASK_FAILED };

typedef struct {
	int status;   /* CARD_ */
	time_t when;  /* when it was saved */
	int slot;     /* the slot (1-3) it was saved in, 0 none */
	int undo;     /* the Undo card: UNDO_ kind, 0 when empty */
} pause_card;

typedef struct {
	int screen;        /* PAUSE_ */
	int item;          /* the menu's selection */
	int mode;          /* SLOTS_ */
	int card;          /* the selected card */
	int ask, ask_card, focus, ask_from; /* the dialog, its card, 0 Cancel / 1 the action, the screen under it */
	int touched;       /* where a touch went down (pause_hit), -1 */
	int refused;       /* A on something that does nothing now, counted: the screen flashes it */
	int busy;          /* a write or a load is running: presses are ignored */
	int states_on;     /* save states work this session */
	int exit_failed;   /* a write failed on Exit: Exit reads "Leave without saving" */
	int discs, disc;   /* the game's discs (Change disc shows for 2 or more) and the one in, from 0 */
	int disc_wanted;   /* PACT_DISC: the disc to put in; disc changes only when the engine took it */
	int failure_shown; /* the panel of what failed is under the items: three columns, no fourth row */
	pause_card cards[CARDS];
	char line[160];    /* save-slots: what the last action did ("Saved to Slot 2") */
	char failed_title[64], failed_body[320]; /* ASK_FAILED */
} pause_ui;

/* The menu opens on Resume, every time. */
void pause_open(pause_ui *p, int states_on);

enum { PEV_UP, PEV_DOWN, PEV_LEFT, PEV_RIGHT, PEV_OK, PEV_BACK, PEV_DELETE, PEV_HOME, PEV_TOUCH_DOWN, PEV_TOUCH_UP };
/* What the caller must do; the card is p->card (PACT_SAVE) or p->ask_card. */
enum { PACT_NONE, PACT_RESUME, PACT_SAVE, PACT_OVERWRITE, PACT_LOAD, PACT_DELETE, PACT_RESET,
       PACT_EXIT_ROMS, PACT_EXIT_WIIU, PACT_SETTINGS, PACT_OPTIONS,
       PACT_DISC /* put in p->disc_wanted; the caller sets p->disc once the engine took it */ };
/* target: for touches, pause_hit at the touch, -1 for none. A tap selects;
 * a tap on the selected thing acts as A. A touch counts only when it goes
 * down and comes up on the same thing. */
int pause_event(pause_ui *p, int event, int target);
/* The events one controller's presses make (SRC_ bits from input_menu): its
 * own buttons, e.g. 2 for OK and 1 for Back on a Wii Remote held sideways,
 * and minus for Delete where there is no Y. Returns the count. */
int pause_events(int type, uint32_t pressed, int *out, int max);
void pause_back_to_slots(pause_ui *p);   /* after a slot action: the same card stays selected */
void pause_fail(pause_ui *p, const char *title, const char *body); /* ASK_FAILED over the current screen */
void pause_exit_failed(pause_ui *p);     /* Exit found failed writes: back on the menu, Resume selected */
int  pause_card_kept(const pause_ui *p, int card); /* loadable, so overwriting it keeps it as Undo save */

/* ---- layout, on the 854x480 canvas ---- */
/* -1 in r[2] for an item the menu does not show (Change disc for one disc). */
void pause_item_rect(const pause_ui *p, int item, int r[4]);
int  pause_item_shown(const pause_ui *p, int item);
/* Where the panel of what failed goes: under the items, above the legend. */
void pause_failure_rect(int r[4]);
void pause_card_rect(int shown, int card, int r[4]); /* one row of shown (pause_cards_shown), centred */
void pause_button_rect(int button, int r[4]); /* 0 Cancel, 1 the action, 2 OK alone (centred) */
int  pause_cards_shown(const pause_ui *p);  /* 3 in Save mode, 4 in Load mode */
int  pause_hit(const pause_ui *p, int x, int y); /* item, card or button there, -1 */

/* ---- words ---- */
const char *pause_item_name(const pause_ui *p, int item);
int  pause_item_enabled(const pause_ui *p, int item);
/* Why an item does nothing now, in a few words on the item while it is selected; "" when it works. */
const char *pause_item_reason(const pause_ui *p, int item);
const char *pause_card_name(const pause_ui *p, int card); /* "Slot 2", "Undo load" */
/* "today 9:40 PM", "Empty", "Damaged: can't load", ... */
void pause_card_detail(char *out, unsigned size, const pause_card *c, time_t now, int us);
/* The line under the cards for the selected Undo card: "Slot 2 as it was before you saved over it". */
void pause_card_description(char *out, unsigned size, const pause_ui *p, int card);
/* The legend's A word on the slot screen: "Save here", "Overwrite", "Load";
 * *dim when A does nothing on the selected card. */
const char *pause_slot_ok(const pause_ui *p, int *dim);
int  pause_can_delete(const pause_ui *p); /* Y Delete acts on the selected card */

typedef struct {
	char question[64], button[48];
	char body[3][128];
	int lines;
	char thumb_label[2][64]; /* Overwrite: the new one ("Now") and the old one; others: the card */
	int thumbs;              /* 0, 1 or 2 pictures */
} pause_ask_text;
void pause_ask_words(const pause_ui *p, time_t now, int us, pause_ask_text *t);

/* The legend: buttons that act on this screen and selection, HOME and B on
 * the left, A on the right, with the glyphs of controller type. */
enum { GLYPH_DPAD, GLYPH_A, GLYPH_B, GLYPH_Y, GLYPH_HOME, GLYPH_MINUS, GLYPH_1, GLYPH_2, GLYPH_PLUS, GLYPH_X, GLYPH_L, GLYPH_R };
typedef struct { int glyph; const char *label; int dim; } pause_key;
int pause_legend(const pause_ui *p, int type, pause_key *keys, int max);

/* HOME outside a game asks too, over whatever screen is up (Zach,
 * 2026-10-19: every HOME press warns, not only the pause menu's): "Exit to
 * the Wii U Menu?" with Cancel (focus 0, first, so a reflex A changes
 * nothing) and Exit (1). exit_ask_event returns 1 to leave, -1 to stay, 0
 * while it asks; HOME on the question cancels it, as in the pause menu.
 * Touch targets are exit_ask_hit's: 0 Cancel, 1 Exit, -1 neither. */
int exit_ask_event(int *focus, int event, int target);
int exit_ask_hit(int x, int y);
int exit_ask_legend(int type, pause_key *keys, int max);

/* The autosave line: in the pause header when all is well (one line), or
 * what failed and what is safe, in red under the items. */
typedef struct {
	time_t last_ok;      /* last autosave on the SD, 0 none */
	int from_launch;     /* last_ok is the one this game started from */
	int writing;         /* an autosave or in-game save is going to the SD now */
	int autosave_failed; /* the newest autosave did not reach the SD */
	time_t ingame_failed;/* an in-game save that did not reach the SD, 0 none */
	int exit_failed;     /* Exit found failed writes: leaving now is "Leave without saving" */
	int off;             /* Auto-Resume is off: no autosave line, only in-game save failures */
} pause_autosave;
/* Up to 4 lines; returns the count. *red: a failure. */
int pause_autosave_lines(const pause_autosave *a, time_t now, int us, char lines[4][128], int *red);

#ifdef __cplusplus
}
#endif
#endif
