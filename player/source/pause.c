/* Our64 player: the pause menu, save state screen and dialogs (see pause.h). */
#include "pause.h"
#include "input.h"
#include "prompt.h"
#include "theme.h"

#include <stdio.h>
#include <string.h>

#define BIT(s) (1u << (s))

/* ---- layout ---- */

/* Six items in two columns of three under the header (Zach, 2026-10-17). */
#define ITEM_X 40
#define ITEM_Y 136
#define ITEM_W 381
#define ITEM_H 62
#define ITEM_STEP_X 393
#define ITEM_STEP_Y 70
#define FAILED_STEP_Y 66 /* rows while the failure panel shows: room under them for it */

/* Cards in one row under the header, centred: three slots, then the Undo card in Load mode. */
#define CARD_W 186
#define CARD_H 160
#define CARD_GAP 14
#define CARD_Y 136

/* Dialog buttons: Cancel on the left, the action on the right, 22 above the dialog's bottom (404). */
#define BUTTON_Y 320
#define BUTTON_W 280
#define BUTTON_H 62

int pause_item_shown(const pause_ui *p, int item)
{
	return item >= 0 && item < ITEMS && (item != ITEM_DISC || p->discs > 1);
}

/* The grid: each column's items top to bottom. Change disc is the second
 * column's fourth, or, while what failed takes the fourth row's room, the
 * third column's first. */
#define COLUMNS 3
static const int g_columns[COLUMNS][4] = { { ITEM_RESUME, ITEM_OPTIONS, ITEM_SETTINGS, ITEM_EXIT },
                                           { ITEM_SAVE, ITEM_LOAD, ITEM_RESET, ITEM_DISC },
                                           { -1, -1, -1, -1 } };
/* Game options is a fourth row too: with the panel up it heads the third column, Change disc under it. */
static const int g_columns_failed[COLUMNS][4] = { { ITEM_RESUME, ITEM_SETTINGS, ITEM_EXIT, -1 },
                                                  { ITEM_SAVE, ITEM_LOAD, ITEM_RESET, -1 },
                                                  { ITEM_OPTIONS, ITEM_DISC, -1, -1 } };
static const int (*columns(const pause_ui *p))[4]
{
	return p->failure_shown ? g_columns_failed : g_columns;
}
static int column_count(const pause_ui *p) { return columns(p) == g_columns_failed ? 3 : 2; }

/* item's column and row as shown, 0 when it is shown. */
static int place(const pause_ui *p, int item, int *col, int *row)
{
	for (int c = 0; c < column_count(p); c++) {
		int shown = 0;
		for (int i = 0; i < 4; i++) {
			const int it = columns(p)[c][i];
			if (it < 0 || !pause_item_shown(p, it))
				continue;
			if (it == item) {
				*col = c;
				*row = shown;
				return 0;
			}
			shown++;
		}
	}
	return -1;
}

/* The item at col, row as shown, or -1. */
static int item_at(const pause_ui *p, int col, int row)
{
	int shown = 0;
	for (int i = 0; i < 4; i++) {
		const int it = columns(p)[col][i];
		if (it >= 0 && pause_item_shown(p, it) && shown++ == row)
			return it;
	}
	return -1;
}

void pause_item_rect(const pause_ui *p, int item, int r[4])
{
	int col = 0, row = 0;
	const int shown = !place(p, item, &col, &row);
	/* three columns share the two's width; the failure panel's rows step closer (pause_failure_rect) */
	const int cols = column_count(p), gap = ITEM_STEP_X - ITEM_W;
	const int w = cols == 3 ? (2 * ITEM_W + gap - 2 * gap) / 3 : ITEM_W;
	const int step_y = p->failure_shown ? FAILED_STEP_Y : ITEM_STEP_Y;
	r[0] = ITEM_X + col * (w + gap);
	r[1] = ITEM_Y + row * step_y;
	r[2] = shown ? w : -1;
	r[3] = ITEM_H;
}

void pause_failure_rect(int r[4])
{
	r[0] = ITEM_X;
	r[1] = ITEM_Y + 2 * FAILED_STEP_Y + ITEM_H + 8; /* under the third row and its selection frame */
	r[2] = ITEM_W * 2 + ITEM_STEP_X - ITEM_W;
	r[3] = 410 - r[1]; /* to just above the legend */
}

void pause_card_rect(int shown, int card, int r[4])
{
	int x0 = (UI_W - (shown * CARD_W + (shown - 1) * CARD_GAP)) / 2;
	r[0] = x0 + card * (CARD_W + CARD_GAP);
	r[1] = CARD_Y;
	r[2] = CARD_W;
	r[3] = CARD_H;
}

void pause_button_rect(int button, int r[4])
{
	r[0] = button == 2 ? (UI_W - BUTTON_W) / 2 : UI_W / 2 + (button ? 20 : -20 - BUTTON_W);
	r[1] = BUTTON_Y;
	r[2] = BUTTON_W;
	r[3] = BUTTON_H;
}

int pause_cards_shown(const pause_ui *p) { return p->mode == SLOTS_LOAD ? CARDS : CARD_SLOTS; }

static int inside(int x, int y, const int r[4]) { return x >= r[0] && x < r[0] + r[2] && y >= r[1] && y < r[1] + r[3]; }

int pause_hit(const pause_ui *p, int x, int y)
{
	int r[4];
	switch (p->screen) {
	case PAUSE_MENU:
		for (int i = 0; i < ITEMS; i++) {
			pause_item_rect(p, i, r);
			if (r[2] > 0 && inside(x, y, r))
				return i;
		}
		break;
	case PAUSE_SLOTS:
		for (int c = 0; c < pause_cards_shown(p); c++) {
			pause_card_rect(pause_cards_shown(p), c, r);
			if (inside(x, y, r))
				return c;
		}
		break;
	case PAUSE_ASK:
		if (p->ask == ASK_FAILED) {
			pause_button_rect(2, r);
			return inside(x, y, r) ? 0 : -1;
		}
		for (int b = 0; b < 2; b++) {
			pause_button_rect(b, r);
			if (inside(x, y, r))
				return b;
		}
		break;
	}
	return -1;
}

/* ---- what presses do ---- */

void pause_open(pause_ui *p, int states_on)
{
	memset(p, 0, sizeof(*p));
	p->screen = PAUSE_MENU;
	p->item = ITEM_RESUME;
	p->touched = -1;
	p->ask_card = -1;
	p->states_on = states_on;
}

int pause_item_enabled(const pause_ui *p, int item)
{
	return p->states_on || (item != ITEM_SAVE && item != ITEM_LOAD && item != ITEM_RESET);
}

/* Save state, Load state and Reset game (whose Undo is a state) are off
 * when states did not start for the game: no memory, a state too big, or
 * the core would not give one (the log says which). */
const char *pause_item_reason(const pause_ui *p, int item)
{
	return pause_item_enabled(p, item) ? "" : "Not in this game";
}

int pause_card_kept(const pause_ui *p, int card) { return card >= 0 && card < CARDS && p->cards[card].status == CARD_FILLED; }

/* Save mode: the slot saved to last, else the first empty one. Load mode:
 * the newest copy that loads. So a player who saves before every try finds
 * their slot selected, and one coming back to load finds the latest. */
static int start_card(const pause_ui *p)
{
	int best = -1, first_empty = -1;
	int n = pause_cards_shown(p);
	for (int c = 0; c < n; c++) {
		if (p->cards[c].status == CARD_EMPTY && first_empty < 0 && c < CARD_SLOTS)
			first_empty = c;
		if (p->cards[c].status == CARD_FILLED && (best < 0 || p->cards[c].when > p->cards[best].when))
			best = c;
	}
	if (best >= 0)
		return best;
	if (p->mode == SLOTS_SAVE && first_empty >= 0)
		return first_empty;
	return CARD_SLOT1;
}

static void open_slots(pause_ui *p, int mode)
{
	p->screen = PAUSE_SLOTS;
	p->mode = mode;
	p->line[0] = 0;
	p->card = start_card(p);
}

static void ask(pause_ui *p, int what, int card)
{
	if (p->screen != PAUSE_ASK)
		p->ask_from = p->screen;
	p->screen = PAUSE_ASK;
	p->ask = what;
	p->ask_card = card;
	p->focus = 0; /* Cancel: a reflex A changes nothing */
}

void pause_fail(pause_ui *p, const char *title, const char *body)
{
	ask(p, ASK_FAILED, p->ask_card);
	snprintf(p->failed_title, sizeof(p->failed_title), "%s", title);
	snprintf(p->failed_body, sizeof(p->failed_body), "%s", body);
}

void pause_exit_failed(pause_ui *p)
{
	p->exit_failed = 1;
	p->screen = PAUSE_MENU;
	p->item = ITEM_RESUME;
}

void pause_back_to_slots(pause_ui *p)
{
	p->screen = PAUSE_SLOTS;
	if (p->ask_card >= 0)
		p->card = p->ask_card;
}

int pause_can_delete(const pause_ui *p)
{
	return p->card >= 0 && p->card < CARD_SLOTS && p->cards[p->card].status != CARD_EMPTY;
}

const char *pause_slot_ok(const pause_ui *p, int *dim)
{
	int status = p->cards[p->card].status;
	*dim = 0;
	if (p->mode == SLOTS_SAVE)
		return status == CARD_EMPTY ? "Save here" : "Overwrite";
	*dim = status != CARD_FILLED;
	return "Load";
}

static int menu_event(pause_ui *p, int event)
{
	int col = 0, row = 0;
	place(p, p->item, &col, &row);
	switch (event) {
	case PEV_LEFT:
	case PEV_RIGHT: {
		/* the next column's item on the same row, or its last when it is shorter */
		const int to = event == PEV_LEFT ? col - 1 : col + 1;
		if (to < 0 || to >= column_count(p))
			break;
		int it = -1;
		for (int r = row; r >= 0 && it < 0; r--)
			it = item_at(p, to, r);
		if (it >= 0)
			p->item = it;
		break;
	}
	case PEV_UP:
		if (row > 0) {
			p->item = item_at(p, col, row - 1);
		} else { /* the column's first: round to its last (Zach, 2026-11-11) */
			int last = row;
			while (item_at(p, col, last + 1) >= 0)
				last++;
			p->item = item_at(p, col, last);
		}
		break;
	case PEV_DOWN:
		p->item = item_at(p, col, row + 1) >= 0 ? item_at(p, col, row + 1) : item_at(p, col, 0);
		break;
	case PEV_BACK:
		return PACT_RESUME;
	case PEV_OK:
		if (!pause_item_enabled(p, p->item)) {
			p->refused++;
			break;
		}
		switch (p->item) {
		case ITEM_RESUME: return PACT_RESUME;
		case ITEM_EXIT: return PACT_EXIT_ROMS;
		case ITEM_SETTINGS: return PACT_SETTINGS;
		case ITEM_OPTIONS: return PACT_OPTIONS;
		case ITEM_SAVE: open_slots(p, SLOTS_SAVE); break;
		case ITEM_LOAD: open_slots(p, SLOTS_LOAD); break;
		case ITEM_RESET: ask(p, ASK_RESET, -1); break;
		case ITEM_DISC:
			p->disc_wanted = (p->disc + 1) % p->discs;
			return PACT_DISC;
		}
		break;
	}
	return PACT_NONE;
}

static int slots_event(pause_ui *p, int event)
{
	int c = p->card;
	switch (event) {
	case PEV_LEFT:
		if (c > 0)
			p->card--;
		break;
	case PEV_RIGHT:
		if (c < pause_cards_shown(p) - 1)
			p->card++;
		break;
	case PEV_BACK:
		p->screen = PAUSE_MENU;
		p->line[0] = 0;
		break;
	case PEV_OK:
		if (p->mode == SLOTS_SAVE) {
			p->line[0] = 0;
			if (p->cards[c].status == CARD_EMPTY)
				return PACT_SAVE;
			ask(p, ASK_OVERWRITE, c);
		} else if (p->cards[c].status == CARD_FILLED) {
			p->line[0] = 0;
			ask(p, ASK_LOAD, c);
		}
		break;
	case PEV_DELETE:
		if (pause_can_delete(p)) {
			p->line[0] = 0;
			ask(p, ASK_DELETE, c);
		}
		break;
	}
	return PACT_NONE;
}

static int ask_event(pause_ui *p, int event)
{
	static const int act[] = { PACT_OVERWRITE, PACT_LOAD, PACT_RESET, PACT_DELETE, PACT_EXIT_WIIU };
	switch (event) {
	case PEV_LEFT:
		p->focus = 0;
		break;
	case PEV_RIGHT:
		if (p->ask != ASK_FAILED)
			p->focus = 1;
		break;
	case PEV_BACK:
		p->screen = p->ask_from;
		break;
	case PEV_OK:
		if (p->ask == ASK_FAILED || p->focus == 0) {
			p->screen = p->ask_from;
			break;
		}
		return act[p->ask];
	}
	return PACT_NONE;
}

static int selected(const pause_ui *p, int target)
{
	switch (p->screen) {
	case PAUSE_MENU: return p->item == target;
	case PAUSE_SLOTS: return p->card == target;
	default: return p->focus == target;
	}
}

static void select_target(pause_ui *p, int target)
{
	switch (p->screen) {
	case PAUSE_MENU: p->item = target; break;
	case PAUSE_SLOTS: p->card = target; break;
	default: p->focus = target; break;
	}
}

int pause_event(pause_ui *p, int event, int target)
{
	if (p->busy)
		return PACT_NONE;
	if (event == PEV_HOME) {
		/* HOME asks before the Wii U Menu; on that question it cancels (Zach, 2026-10-17). */
		if (p->screen == PAUSE_ASK && p->ask == ASK_HOME)
			p->screen = p->ask_from;
		else
			ask(p, ASK_HOME, p->screen == PAUSE_ASK ? p->ask_card : -1);
		return PACT_NONE;
	}
	if (event == PEV_TOUCH_DOWN) {
		p->touched = target;
		return PACT_NONE;
	}
	if (event == PEV_TOUCH_UP) {
		int was = p->touched;
		p->touched = -1;
		if (was < 0 || was != target)
			return PACT_NONE;
		if (!selected(p, target)) {
			select_target(p, target);
			return PACT_NONE;
		}
		event = PEV_OK;
	}
	switch (p->screen) {
	case PAUSE_MENU: return menu_event(p, event);
	case PAUSE_SLOTS: return slots_event(p, event);
	default: return ask_event(p, event);
	}
}

int pause_events(int type, uint32_t pressed, int *out, int max)
{
	const int wiimote = type == INPUT_WIIMOTE, no_y = wiimote || type == INPUT_NUNCHUK;
	const struct { uint32_t bits; int event; } map[] = {
		{ BIT(SRC_UP) | BIT(SRC_LSTICK_UP), PEV_UP },
		{ BIT(SRC_DOWN) | BIT(SRC_LSTICK_DOWN), PEV_DOWN },
		{ BIT(SRC_LEFT) | BIT(SRC_LSTICK_LEFT), PEV_LEFT },
		{ BIT(SRC_RIGHT) | BIT(SRC_LSTICK_RIGHT), PEV_RIGHT },
		{ BIT(SRC_A) | (wiimote ? BIT(SRC_2) : 0), PEV_OK },
		{ BIT(SRC_B) | (wiimote ? BIT(SRC_1) : 0), PEV_BACK },
		{ no_y ? BIT(SRC_MINUS) : BIT(SRC_Y), PEV_DELETE },
	};
	int n = 0;
	for (unsigned i = 0; i < sizeof(map) / sizeof(map[0]) && n < max; i++)
		if (pressed & map[i].bits)
			out[n++] = map[i].event;
	return n;
}

/* ---- words ---- */

static const char *const g_item[ITEMS] = { "Resume", "Settings", "Exit to Our ROMs", "Save state", "Load state",
                                           "Reset game", "Change disc", "Game options" };

const char *pause_item_name(const pause_ui *p, int item)
{
	if (item == ITEM_EXIT && p->exit_failed)
		return "Leave without saving";
	if (item == ITEM_DISC) { /* the disc in now; A puts the next one in */
		static char name[48];
		snprintf(name, sizeof(name), "Change disc (disc %d in)", p->disc + 1);
		return name;
	}
	return item >= 0 && item < ITEMS ? g_item[item] : "";
}

const char *pause_card_name(const pause_ui *p, int card)
{
	static const char *const slot[CARD_SLOTS] = { "Slot 1", "Slot 2", "Slot 3" };
	if (card >= 0 && card < CARD_SLOTS)
		return slot[card];
	if (card != CARD_UNDO)
		return "";
	switch (p->cards[CARD_UNDO].status == CARD_FILLED ? p->cards[CARD_UNDO].undo : 0) {
	case UNDO_SAVE: return "Undo save";
	case UNDO_LOAD: return "Undo load";
	case UNDO_RESET: return "Undo reset";
	default: return "Undo";
	}
}

void pause_card_detail(char *out, unsigned size, const pause_card *c, time_t now, int us)
{
	switch (c->status) {
	case CARD_FILLED:
		if (c->when)
			ui_when(out, size, now, c->when, us);
		else
			snprintf(out, size, "time unknown");
		break;
	case CARD_DAMAGED: snprintf(out, size, "Damaged: can't load"); break;
	case CARD_OTHER_VERSION: snprintf(out, size, "Made by another Our64 version: can't load"); break;
	case CARD_OTHER_GAME: snprintf(out, size, "Made for another game: can't load"); break;
	default: snprintf(out, size, "Empty"); break;
	}
}

void pause_card_description(char *out, unsigned size, const pause_ui *p, int card)
{
	const pause_card *c = &p->cards[CARD_UNDO];
	out[0] = 0;
	if (card != CARD_UNDO)
		return;
	if (c->status != CARD_FILLED) {
		snprintf(out, size, "Fills when you overwrite, load or reset");
		return;
	}
	switch (c->undo) {
	case UNDO_LOAD: snprintf(out, size, "Before your last load"); break;
	case UNDO_RESET: snprintf(out, size, "Before the reset"); break;
	default:
		if (c->slot >= 1 && c->slot <= CARD_SLOTS)
			snprintf(out, size, "Slot %d before you saved over it", c->slot);
		else
			snprintf(out, size, "A slot before you saved over it");
		break;
	}
}

/* A thumbnail's label: the card's time alone, since the question names the card. */
static void card_when(char *out, unsigned size, const pause_ui *p, int card, time_t now, int us)
{
	pause_card_detail(out, size, &p->cards[card], now, us);
}

void pause_ask_words(const pause_ui *p, time_t now, int us, pause_ask_text *t)
{
	memset(t, 0, sizeof(*t));
	const int c = p->ask_card;
	const char *name = pause_card_name(p, c);
	switch (p->ask) {
	case ASK_OVERWRITE:
		snprintf(t->question, sizeof(t->question), "Overwrite %s?", name);
		snprintf(t->button, sizeof(t->button), "Overwrite %s", name);
		snprintf(t->thumb_label[0], sizeof(t->thumb_label[0]), "Now");
		card_when(t->thumb_label[1], sizeof(t->thumb_label[1]), p, c, now, us);
		t->thumbs = 2;
		break;
	case ASK_LOAD:
		snprintf(t->question, sizeof(t->question), "Load %s?", name);
		snprintf(t->button, sizeof(t->button), "Load %s", name);
		card_when(t->thumb_label[0], sizeof(t->thumb_label[0]), p, c, now, us);
		t->thumbs = 1;
		break;
	case ASK_RESET:
		snprintf(t->question, sizeof(t->question), "Reset the game?");
		snprintf(t->button, sizeof(t->button), "Reset game");
		snprintf(t->body[t->lines++], sizeof(t->body[0]), "Back to the Title Screen");
		snprintf(t->body[t->lines++], sizeof(t->body[0]), "Undo Reset in Load State screen");
		t->thumbs = 1;
		break;
	case ASK_DELETE:
		snprintf(t->question, sizeof(t->question), "Delete %s?", name);
		snprintf(t->button, sizeof(t->button), "Delete %s", name);
		snprintf(t->body[t->lines++], sizeof(t->body[0]), "You can't undo this");
		card_when(t->thumb_label[0], sizeof(t->thumb_label[0]), p, c, now, us);
		t->thumbs = 1;
		break;
	case ASK_HOME:
		snprintf(t->question, sizeof(t->question), "Exit to the Wii U Menu?");
		snprintf(t->button, sizeof(t->button), "Exit");
		snprintf(t->body[t->lines++], sizeof(t->body[0]), "%s",
		         p->exit_failed ? "The SD card failed: leaving now loses play since the last autosave."
		                        : theme_comrade() ? "Our game is saved as always, comrade" : "Your game is saved as always.");
		break;
	default:
		snprintf(t->question, sizeof(t->question), "%s", p->failed_title);
		snprintf(t->button, sizeof(t->button), "OK");
		snprintf(t->body[t->lines++], sizeof(t->body[0]), "%.127s", p->failed_body);
		break;
	}
}

int pause_legend(const pause_ui *p, int type, pause_key *keys, int max)
{
	if (p->busy || max < 5)
		return 0;
	const int wiimote = type == INPUT_WIIMOTE;
	const int ok = wiimote ? GLYPH_2 : GLYPH_A, back = wiimote ? GLYPH_1 : GLYPH_B;
	const int del = wiimote || type == INPUT_NUNCHUK ? GLYPH_MINUS : GLYPH_Y;
	int n = 0;
	if (!(p->screen == PAUSE_ASK && p->ask == ASK_HOME))
		keys[n++] = (pause_key){ GLYPH_HOME, "Wii U Menu", 0 };
	switch (p->screen) {
	case PAUSE_MENU:
		keys[n++] = (pause_key){ back, "Resume", 0 };
		keys[n++] = (pause_key){ GLYPH_DPAD, "Choose", 0 };
		keys[n++] = (pause_key){ ok, "OK", !pause_item_enabled(p, p->item) };
		break;
	case PAUSE_SLOTS: {
		keys[n++] = (pause_key){ back, "Back", 0 };
		keys[n++] = (pause_key){ GLYPH_DPAD, "Choose", 0 };
		if (pause_can_delete(p))
			keys[n++] = (pause_key){ del, "Delete", 0 };
		int dim;
		const char *label = pause_slot_ok(p, &dim);
		keys[n++] = (pause_key){ ok, label, dim };
		break;
	}
	default:
		if (p->ask == ASK_FAILED) {
			keys[n++] = (pause_key){ back, "Back", 0 };
		} else {
			keys[n++] = (pause_key){ back, "Cancel", 0 };
			keys[n++] = (pause_key){ GLYPH_DPAD, "Choose", 0 };
		}
		keys[n++] = (pause_key){ ok, "OK", 0 };
		break;
	}
	return n;
}

/* A time today as its clock, else with its day. */
static void stamp(char *out, unsigned size, time_t t, time_t now, int us)
{
	if (t <= now && now / 86400 == t / 86400)
		ui_clock(out, size, t, us);
	else
		ui_when(out, size, now, t, us);
}

int pause_autosave_lines(const pause_autosave *a, time_t now, int us, char lines[4][128], int *red)
{
	char t[48];
	int n = 0;
	*red = 0;
	if (a->writing) {
		snprintf(lines[n++], 128, "Saving to SD card...");
		return n;
	}
	if (a->off) { /* Auto-Resume off: no autosave to name, only an in-game save that failed */
		if (!a->ingame_failed)
			return 0;
		*red = 1;
		stamp(t, sizeof(t), a->ingame_failed, now, us);
		snprintf(lines[n++], 128, "Your in-game save from %s is not on the SD card.", t);
		snprintf(lines[n++], 128, "Check the card's lock switch or free space on a computer. OUR-0201");
		if (a->exit_failed)
			snprintf(lines[n++], 128, "Leave without saving: that in-game save is lost.");
		return n;
	}
	if (a->ingame_failed || a->autosave_failed) {
		*red = 1;
		if (a->ingame_failed) {
			stamp(t, sizeof(t), a->ingame_failed, now, us);
			snprintf(lines[n++], 128, "Your in-game save from %s is not on the SD card.", t);
		} else {
			snprintf(lines[n++], 128, "The newest autosave did not reach the SD card.");
		}
		if (a->last_ok)
			stamp(t, sizeof(t), a->last_ok, now, us);
		/* the last good autosave once: the Leave line names it when it shows (hci review 2026-10-23, #19) */
		if (a->last_ok && !a->exit_failed)
			snprintf(lines[n++], 128, "Last good autosave: %s.", t);
		else if (!a->last_ok)
			snprintf(lines[n++], 128, "No autosave is on the SD card.");
		snprintf(lines[n++], 128, "Check the card's lock switch or free space on a computer. OUR-0201");
		if (a->exit_failed) {
			if (a->last_ok)
				snprintf(lines[n++], 128, "Leave without saving: this game resumes from the %s autosave.", t);
			else
				snprintf(lines[n++], 128, "Leave without saving: this game starts at its title screen next time.");
		}
		return n;
	}
	if (!a->last_ok) {
		snprintf(lines[n++], 128, "Autosaved: not yet");
		return n;
	}
	/* How long ago, not the clock time (Zach, 2026-10-18). */
	long age = (long)(now - a->last_ok);
	static const struct { long s; const char *unit; } units[] = {
		{ 86400, "day" }, { 3600, "hour" }, { 60, "minute" },
	};
	if (age < 60) {
		snprintf(lines[n++], 128, "Autosaved: just now");
		return n;
	}
	for (unsigned u = 0; u < sizeof(units) / sizeof(units[0]); u++)
		if (age >= units[u].s) {
			long k = age / units[u].s;
			snprintf(lines[n++], 128, "Autosaved: %ld %s%s ago", k, units[u].unit, k == 1 ? "" : "s");
			break;
		}
	return n;
}

/* ---- the Exit question outside a game ---- */

int exit_ask_event(int *focus, int event, int target)
{
	switch (event) {
	case PEV_HOME:
	case PEV_BACK:
		return -1;
	case PEV_LEFT: *focus = 0; return 0;
	case PEV_RIGHT: *focus = 1; return 0;
	case PEV_OK: return *focus ? 1 : -1;
	case PEV_TOUCH_DOWN:
		if (target >= 0)
			*focus = target;
		return 0;
	case PEV_TOUCH_UP:
		if (target < 0)
			return 0;
		*focus = target;
		return target ? 1 : -1;
	}
	return 0;
}

int exit_ask_hit(int x, int y)
{
	int r[4];
	for (int b = 0; b < 2; b++) {
		pause_button_rect(b, r);
		if (inside(x, y, r))
			return b;
	}
	return -1;
}

int exit_ask_legend(int type, pause_key *keys, int max)
{
	if (max < 3)
		return 0;
	const int wiimote = type == INPUT_WIIMOTE;
	keys[0] = (pause_key){ wiimote ? GLYPH_1 : GLYPH_B, "Cancel", 0 };
	keys[1] = (pause_key){ GLYPH_DPAD, "Choose", 0 };
	keys[2] = (pause_key){ wiimote ? GLYPH_2 : GLYPH_A, "OK", 0 };
	return 3;
}
