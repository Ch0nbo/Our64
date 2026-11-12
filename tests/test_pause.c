/* PC tests for ui/pause.c: the pause menu, the save state screen
 * and the dialogs (ux/flows/pause; rulings in docs/ux/requirements/2026-10-16-pause.md). */
#include "pause.h"
#include "prompt.h"
#include "theme.h"
#include "input.h"

#include <stdio.h>
#include <string.h>

static int g_failed, g_run;

#define CHECK(cond) do { if (!(cond)) { printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); g_failed++; return; } } while (0)

#define DAY 86400
#define NOW ((time_t)(20000 * DAY + 21 * 3600 + 40 * 60)) /* 9:40 PM */

static pause_ui g_p;

/* Cards 0-2 slots, 3 the Undo card (undo: its kind when filled). */
static void open_with(const int *status, int undo)
{
	pause_open(&g_p, 1);
	for (int c = 0; c < CARDS; c++) {
		g_p.cards[c].status = status ? status[c] : CARD_EMPTY;
		g_p.cards[c].when = NOW - 3600 * (CARDS - c);
		g_p.cards[c].slot = c < CARD_SLOTS ? c + 1 : 0;
	}
	g_p.cards[CARD_UNDO].undo = g_p.cards[CARD_UNDO].status == CARD_FILLED ? undo : 0;
}

static int ev(int e) { return pause_event(&g_p, e, -1); }

static int tap(int target)
{
	pause_event(&g_p, PEV_TOUCH_DOWN, target);
	return pause_event(&g_p, PEV_TOUCH_UP, target);
}

/* To an item wherever the grid has it: each column from its top, down. */
static void go_item(int item)
{
	for (int c = 0; c < 3; c++) {
		for (int k = 0; k < 3; k++) ev(PEV_LEFT);
		for (int k = 0; k < 4; k++) ev(PEV_UP);
		for (int k = 0; k < c; k++) ev(PEV_RIGHT);
		for (int k = 0; k < 4 && g_p.item != item; k++) ev(PEV_DOWN);
		if (g_p.item == item)
			return;
	}
}

static int has_key(int glyph, const char *label, int *dim, int type)
{
	pause_key k[8];
	int n = pause_legend(&g_p, type, k, 8);
	for (int i = 0; i < n; i++)
		if (k[i].glyph == glyph && (!label || !strcmp(k[i].label, label))) {
			if (dim) *dim = k[i].dim;
			return 1;
		}
	return 0;
}

/* ---- pause-menu ---- */

static void the_menu_opens_on_resume_every_time(void)
{
	open_with(NULL, 0);
	CHECK(g_p.screen == PAUSE_MENU && g_p.item == ITEM_RESUME);
	ev(PEV_DOWN);
	pause_open(&g_p, 1);
	CHECK(g_p.item == ITEM_RESUME);
}

/* Two columns of three (Zach, 2026-10-17): Resume, Settings, Exit to Our
 * ROMs; then Save state, Load state, Reset game. */
static void six_items_in_two_columns_of_three(void)
{
	open_with(NULL, 0);
	CHECK(ITEMS == 8 && !pause_item_shown(&g_p, ITEM_DISC)); /* seven shown: Change disc only for several discs */
	ev(PEV_UP);
	ev(PEV_LEFT);
	CHECK(g_p.item == ITEM_RESUME);
	ev(PEV_DOWN);
	CHECK(g_p.item == ITEM_SETTINGS);
	ev(PEV_DOWN);
	CHECK(g_p.item == ITEM_OPTIONS); /* Game options (2026-11-11): the first column's third, above Exit */
	CHECK(!strcmp(pause_item_name(&g_p, ITEM_OPTIONS), "Game options") && pause_item_enabled(&g_p, ITEM_OPTIONS));
	ev(PEV_DOWN);
	ev(PEV_DOWN);
	CHECK(g_p.item == ITEM_EXIT); /* stops at the column's end */
	go_item(ITEM_OPTIONS);
	CHECK(ev(PEV_OK) == PACT_OPTIONS);
	ev(PEV_RIGHT);
	CHECK(g_p.item == ITEM_RESET); /* the same row in the other column */
	ev(PEV_RIGHT);
	CHECK(g_p.item == ITEM_RESET);
	ev(PEV_UP);
	ev(PEV_UP);
	CHECK(g_p.item == ITEM_SAVE);
	ev(PEV_LEFT);
	CHECK(g_p.item == ITEM_RESUME);
	int a[4], b[4], c[4];
	pause_item_rect(&g_p, ITEM_RESUME, a);
	pause_item_rect(&g_p, ITEM_SETTINGS, b);
	pause_item_rect(&g_p, ITEM_SAVE, c);
	CHECK(a[0] == b[0] && b[1] > a[1]); /* Settings under Resume */
	CHECK(c[1] == a[1] && c[0] > a[0]);  /* Save state beside Resume */
}

static void b_and_resume_close_the_menu(void)
{
	open_with(NULL, 0);
	CHECK(ev(PEV_OK) == PACT_RESUME);
	open_with(NULL, 0);
	go_item(ITEM_RESET);
	CHECK(ev(PEV_BACK) == PACT_RESUME);
}

static void home_asks_before_the_wii_u_menu(void)
{
	pause_ask_text t;
	open_with(NULL, 0);
	go_item(ITEM_LOAD);
	CHECK(ev(PEV_HOME) == PACT_NONE && g_p.screen == PAUSE_ASK && g_p.ask == ASK_HOME && g_p.focus == 0);
	pause_ask_words(&g_p, NOW, 1, &t);
	CHECK(!strcmp(t.question, "Exit to the Wii U Menu?") && !strcmp(t.button, "Exit"));
	CHECK(!strcmp(t.body[0], "Your game is saved as always.")); /* "comrade" only in Comrade */
	theme_use(THEME_COMRADE);
	pause_ask_words(&g_p, NOW, 1, &t);
	CHECK(!strcmp(t.body[0], "Our game is saved as always, comrade"));
	theme_use(THEME_STANDARD);
	CHECK(ev(PEV_HOME) == PACT_NONE && g_p.screen == PAUSE_MENU && g_p.item == ITEM_LOAD); /* HOME again cancels */
	ev(PEV_HOME);
	CHECK(ev(PEV_BACK) == PACT_NONE && g_p.screen == PAUSE_MENU);
	ev(PEV_HOME);
	ev(PEV_RIGHT);
	CHECK(ev(PEV_OK) == PACT_EXIT_WIIU);
}

static void home_on_a_dialog_cancels_it_and_asks(void)
{
	int s[CARDS] = { CARD_FILLED };
	open_with(s, 0);
	go_item(ITEM_SAVE);
	ev(PEV_OK);
	ev(PEV_OK);
	CHECK(g_p.ask == ASK_OVERWRITE);
	CHECK(ev(PEV_HOME) == PACT_NONE && g_p.ask == ASK_HOME && g_p.ask_from == PAUSE_SLOTS);
	ev(PEV_BACK);
	CHECK(g_p.screen == PAUSE_SLOTS);
}

static void menu_items_lead_where_they_say(void)
{
	open_with(NULL, 0);
	go_item(ITEM_SAVE);
	CHECK(ev(PEV_OK) == PACT_NONE && g_p.screen == PAUSE_SLOTS && g_p.mode == SLOTS_SAVE);
	open_with(NULL, 0);
	go_item(ITEM_LOAD);
	ev(PEV_OK);
	CHECK(g_p.screen == PAUSE_SLOTS && g_p.mode == SLOTS_LOAD);
	open_with(NULL, 0);
	go_item(ITEM_RESET);
	CHECK(ev(PEV_OK) == PACT_NONE && g_p.ask == ASK_RESET && g_p.focus == 0);
	open_with(NULL, 0);
	go_item(ITEM_SETTINGS);
	CHECK(ev(PEV_OK) == PACT_SETTINGS);
	open_with(NULL, 0);
	go_item(ITEM_EXIT);
	CHECK(ev(PEV_OK) == PACT_EXIT_ROMS);
	CHECK(!strcmp(pause_item_name(&g_p, ITEM_EXIT), "Exit to Our ROMs"));
}

static void a_failed_exit_moves_to_resume_and_renames_exit(void)
{
	open_with(NULL, 0);
	go_item(ITEM_EXIT);
	pause_exit_failed(&g_p);
	CHECK(g_p.screen == PAUSE_MENU && g_p.item == ITEM_RESUME);
	CHECK(!strcmp(pause_item_name(&g_p, ITEM_EXIT), "Leave without saving"));
	CHECK(ev(PEV_OK) == PACT_RESUME); /* a reflex second A resumes */
}

static void without_save_states_their_items_do_nothing(void)
{
	pause_open(&g_p, 0);
	go_item(ITEM_SAVE);
	CHECK(g_p.refused == 0);
	CHECK(ev(PEV_OK) == PACT_NONE && g_p.screen == PAUSE_MENU);
	CHECK(g_p.refused == 1); /* counted, so the screen flashes the item: never a dead end */
	go_item(ITEM_RESET);
	CHECK(ev(PEV_OK) == PACT_NONE && g_p.screen == PAUSE_MENU);
	CHECK(!pause_item_enabled(&g_p, ITEM_LOAD) && pause_item_enabled(&g_p, ITEM_SETTINGS));
}

/* A disabled item says why when it is selected (Zach, 2026-10-23); a
 * working one has no reason. */
static void a_disabled_item_says_why(void)
{
	pause_open(&g_p, 0);
	CHECK(!strcmp(pause_item_reason(&g_p, ITEM_SAVE), "Not in this game"));
	CHECK(!strcmp(pause_item_reason(&g_p, ITEM_LOAD), pause_item_reason(&g_p, ITEM_SAVE)));
	CHECK(!strcmp(pause_item_reason(&g_p, ITEM_RESET), pause_item_reason(&g_p, ITEM_SAVE))); /* its Undo is a state */
	CHECK(!strcmp(pause_item_reason(&g_p, ITEM_SETTINGS), ""));
	pause_open(&g_p, 1);
	for (int i = 0; i < ITEMS; i++)
		CHECK(!strcmp(pause_item_reason(&g_p, i), ""));
}


/* ---- save-slots ---- */

static void save_mode_starts_on_the_last_slot_used(void)
{
	int s[CARDS] = { CARD_FILLED, CARD_EMPTY, CARD_FILLED, CARD_FILLED };
	open_with(s, UNDO_LOAD);
	g_p.cards[CARD_SLOT1].when = NOW - 10;
	g_p.cards[CARD_UNDO].when = NOW; /* the Undo card does not count */
	go_item(ITEM_SAVE);
	ev(PEV_OK);
	CHECK(g_p.card == CARD_SLOT1);
	CHECK(pause_cards_shown(&g_p) == CARD_SLOTS);
}

static void save_mode_starts_on_the_first_empty_slot_on_first_use(void)
{
	open_with(NULL, 0);
	go_item(ITEM_SAVE);
	ev(PEV_OK);
	CHECK(g_p.card == CARD_SLOT1);
}

static void load_mode_starts_on_the_newest_copy(void)
{
	int s[CARDS] = { CARD_FILLED, CARD_DAMAGED, CARD_EMPTY, CARD_FILLED };
	open_with(s, UNDO_SAVE);
	g_p.cards[CARD_SLOT2].when = NOW; /* damaged: cannot load */
	go_item(ITEM_LOAD);
	ev(PEV_OK);
	CHECK(g_p.card == CARD_UNDO && pause_cards_shown(&g_p) == CARDS);
}

static void cards_move_without_wrapping(void)
{
	open_with(NULL, 0);
	go_item(ITEM_SAVE);
	ev(PEV_OK);
	ev(PEV_LEFT);
	CHECK(g_p.card == CARD_SLOT1);
	for (int i = 0; i < 6; i++) ev(PEV_RIGHT);
	CHECK(g_p.card == CARD_SLOT3); /* no Undo card in Save mode */
	ev(PEV_BACK);
	go_item(ITEM_LOAD);
	ev(PEV_OK);
	for (int i = 0; i < 6; i++) ev(PEV_RIGHT);
	CHECK(g_p.card == CARD_UNDO);
}

static void b_goes_back_with_the_item_still_selected(void)
{
	open_with(NULL, 0);
	go_item(ITEM_LOAD);
	ev(PEV_OK);
	CHECK(ev(PEV_BACK) == PACT_NONE && g_p.screen == PAUSE_MENU && g_p.item == ITEM_LOAD);
}

static void a_on_an_empty_slot_saves_there(void)
{
	open_with(NULL, 0);
	go_item(ITEM_SAVE);
	ev(PEV_OK);
	ev(PEV_RIGHT);
	CHECK(ev(PEV_OK) == PACT_SAVE && g_p.card == CARD_SLOT2 && g_p.screen == PAUSE_SLOTS);
}

static void a_on_a_filled_slot_asks_to_overwrite_with_cancel_focused(void)
{
	int s[CARDS] = { CARD_FILLED };
	open_with(s, 0);
	go_item(ITEM_SAVE);
	ev(PEV_OK);
	CHECK(ev(PEV_OK) == PACT_NONE && g_p.ask == ASK_OVERWRITE && g_p.focus == 0 && g_p.ask_card == CARD_SLOT1);
	CHECK(ev(PEV_OK) == PACT_NONE && g_p.screen == PAUSE_SLOTS && g_p.card == CARD_SLOT1);
	ev(PEV_OK);
	ev(PEV_RIGHT);
	CHECK(g_p.focus == 1 && ev(PEV_OK) == PACT_OVERWRITE);
}

static void unloadable_slots_can_be_saved_over(void)
{
	int s[CARDS] = { CARD_OTHER_VERSION, CARD_DAMAGED };
	open_with(s, 0);
	go_item(ITEM_SAVE);
	ev(PEV_OK);
	g_p.card = CARD_SLOT1;
	CHECK(ev(PEV_OK) == PACT_NONE && g_p.ask == ASK_OVERWRITE);
	CHECK(!pause_card_kept(&g_p, CARD_SLOT1));
	pause_ask_text t;
	pause_ask_words(&g_p, NOW, 1, &t);
	CHECK(t.lines == 0); /* its picture label already says it can't load */
}

static void load_mode_asks_before_loading(void)
{
	int s[CARDS] = { CARD_FILLED, CARD_EMPTY, CARD_DAMAGED, CARD_FILLED };
	open_with(s, UNDO_RESET);
	go_item(ITEM_LOAD);
	ev(PEV_OK);
	CHECK(g_p.card == CARD_UNDO);
	CHECK(!strcmp(pause_card_name(&g_p, CARD_UNDO), "Undo reset"));
	ev(PEV_LEFT);
	CHECK(ev(PEV_OK) == PACT_NONE && g_p.screen == PAUSE_SLOTS); /* damaged */
	ev(PEV_LEFT);
	CHECK(ev(PEV_OK) == PACT_NONE && g_p.screen == PAUSE_SLOTS); /* empty */
	ev(PEV_RIGHT);
	ev(PEV_RIGHT);
	CHECK(ev(PEV_OK) == PACT_NONE && g_p.ask == ASK_LOAD && g_p.ask_card == CARD_UNDO);
	pause_ask_text t;
	pause_ask_words(&g_p, NOW, 1, &t);
	CHECK(!strcmp(t.question, "Load Undo reset?") && !strcmp(t.button, "Load Undo reset"));
	CHECK(t.lines == 0); /* gap 1: the two swap; the Undo card shows it, no words */
	ev(PEV_RIGHT);
	CHECK(ev(PEV_OK) == PACT_LOAD);
}

static void delete_works_on_filled_slots_only(void)
{
	int s[CARDS] = { CARD_FILLED, CARD_EMPTY, CARD_DAMAGED, CARD_FILLED };
	open_with(s, UNDO_LOAD);
	go_item(ITEM_LOAD);
	ev(PEV_OK);
	CHECK(g_p.card == CARD_UNDO && !pause_can_delete(&g_p) && ev(PEV_DELETE) == PACT_NONE && g_p.screen == PAUSE_SLOTS);
	ev(PEV_LEFT);
	CHECK(pause_can_delete(&g_p)); /* a damaged slot can be cleared */
	ev(PEV_LEFT);
	CHECK(!pause_can_delete(&g_p));
	ev(PEV_LEFT);
	CHECK(ev(PEV_DELETE) == PACT_NONE && g_p.ask == ASK_DELETE);
	ev(PEV_BACK);
	CHECK(g_p.screen == PAUSE_SLOTS && g_p.card == CARD_SLOT1);
}

static void reset_cancel_returns_to_reset_game(void)
{
	open_with(NULL, 0);
	go_item(ITEM_RESET);
	ev(PEV_OK);
	CHECK(ev(PEV_BACK) == PACT_NONE && g_p.screen == PAUSE_MENU && g_p.item == ITEM_RESET);
	ev(PEV_OK);
	ev(PEV_RIGHT);
	CHECK(ev(PEV_OK) == PACT_RESET);
}

static void presses_wait_while_busy(void)
{
	open_with(NULL, 0);
	go_item(ITEM_SAVE);
	ev(PEV_OK);
	CHECK(ev(PEV_OK) == PACT_SAVE);
	g_p.busy = 1;
	CHECK(ev(PEV_OK) == PACT_NONE && ev(PEV_BACK) == PACT_NONE && ev(PEV_HOME) == PACT_NONE);
	CHECK(g_p.screen == PAUSE_SLOTS);
}

static void a_failure_waits_for_ok(void)
{
	open_with(NULL, 0);
	go_item(ITEM_SAVE);
	ev(PEV_OK);
	pause_fail(&g_p, "Couldn't save to Slot 1", "Delete a save state to make room.");
	CHECK(g_p.screen == PAUSE_ASK && g_p.ask == ASK_FAILED);
	ev(PEV_RIGHT);
	CHECK(ev(PEV_OK) == PACT_NONE && g_p.screen == PAUSE_SLOTS);
}

/* ---- touch ---- */

static void a_tap_selects_and_a_second_tap_acts(void)
{
	int r[4];
	open_with(NULL, 0);
	pause_item_rect(&g_p, ITEM_SAVE, r);
	CHECK(pause_hit(&g_p, r[0] + 5, r[1] + 5) == ITEM_SAVE);
	CHECK(tap(ITEM_SAVE) == PACT_NONE && g_p.item == ITEM_SAVE && g_p.screen == PAUSE_MENU);
	CHECK(tap(ITEM_SAVE) == PACT_NONE && g_p.screen == PAUSE_SLOTS);
	CHECK(tap(CARD_SLOT3) == PACT_NONE && g_p.card == CARD_SLOT3);
	pause_event(&g_p, PEV_TOUCH_DOWN, CARD_SLOT3);
	CHECK(pause_event(&g_p, PEV_TOUCH_UP, CARD_SLOT2) == PACT_NONE && g_p.card == CARD_SLOT3); /* slid off */
	CHECK(tap(CARD_SLOT3) == PACT_SAVE);
}

static void one_tap_on_delete_does_not_delete(void)
{
	int s[CARDS] = { CARD_FILLED };
	open_with(s, 0);
	go_item(ITEM_SAVE);
	ev(PEV_OK);
	ev(PEV_DELETE);
	CHECK(tap(1) == PACT_NONE && g_p.focus == 1 && g_p.screen == PAUSE_ASK);
	CHECK(tap(1) == PACT_DELETE);
}

static void the_undo_card_cannot_be_tapped_in_save_mode(void)
{
	int r[4];
	open_with(NULL, 0);
	go_item(ITEM_SAVE);
	ev(PEV_OK);
	pause_card_rect(CARDS, CARD_UNDO, r); /* where it is in Load mode */
	CHECK(pause_hit(&g_p, r[0] + r[2] - 5, r[1] + 5) != CARD_UNDO);
}

static void the_cards_are_centred_in_both_modes(void)
{
	int first[4], last[4];
	for (int shown = CARD_SLOTS; shown <= CARDS; shown++) {
		pause_card_rect(shown, 0, first);
		pause_card_rect(shown, shown - 1, last);
		CHECK(first[0] == 854 - (last[0] + last[2])); /* the canvas is 854 wide */
	}
}

/* A game on several discs (OurPS1's .m3u): Change disc in the second
 * column's fourth place, every other item where it always is; A asks for the
 * next disc, round to the first, and the item says the disc in only once the
 * engine took it (pause_wiiu.c). One disc: no such item (hidden, not
 * disabled). What failed under the items: three columns, Change disc still
 * there (hci review 2026-10-23, #1, #8). */
static void change_disc_is_there_for_several_discs(void)
{
	pause_open(&g_p, 1);
	CHECK(!pause_item_shown(&g_p, ITEM_DISC));
	pause_open(&g_p, 1);
	g_p.discs = 3;
	g_p.disc = 1;
	CHECK(pause_item_shown(&g_p, ITEM_DISC));
	ev(PEV_DOWN);
	ev(PEV_DOWN);
	ev(PEV_DOWN);
	CHECK(g_p.item == ITEM_EXIT); /* three Downs from Resume: Exit, the first column's fourth */
	ev(PEV_RIGHT);
	CHECK(g_p.item == ITEM_DISC); /* beside it, the second column's fourth */
	CHECK(!strcmp(pause_item_name(&g_p, ITEM_DISC), "Change disc (disc 2 in)"));
	CHECK(ev(PEV_OK) == PACT_DISC && g_p.disc_wanted == 2 && g_p.disc == 1); /* disc changes when the engine took it */
	g_p.disc = 2;
	CHECK(ev(PEV_OK) == PACT_DISC && g_p.disc_wanted == 0); /* round to the first */
	ev(PEV_LEFT);
	CHECK(g_p.item == ITEM_EXIT); /* the first column's last */
	int r[4], e[4];
	pause_item_rect(&g_p, ITEM_DISC, r);
	pause_item_rect(&g_p, ITEM_RESET, e);
	CHECK(r[2] > 0 && r[0] == e[0] && r[1] > e[1]); /* under Reset game */
	CHECK(pause_hit(&g_p, r[0] + 3, r[1] + 3) == ITEM_DISC);
	/* what failed shows: three columns of three, Change disc heading the third, nothing under the third row */
	g_p.failure_shown = 1;
	int f[4];
	pause_failure_rect(f);
	pause_item_rect(&g_p, ITEM_DISC, r);
	CHECK(r[2] >= 62 && r[1] + r[3] + 6 <= f[1]);
	for (int i = 0; i < ITEMS; i++) {
		pause_item_rect(&g_p, i, e);
		CHECK(e[1] + e[3] + 6 <= f[1]); /* the panel under every item and its selection frame */
	}
	g_p.item = ITEM_LOAD;
	ev(PEV_RIGHT);
	CHECK(g_p.item == ITEM_DISC);
	g_p.failure_shown = 0;
}

static void targets_are_big_enough_to_touch(void)
{
	int r[4];
	g_p.discs = 2; /* every item shown */
	g_p.failure_shown = 0;
	for (int i = 0; i < ITEMS; i++) {
		pause_item_rect(&g_p, i, r);
		CHECK(r[2] >= 62 && r[3] >= 62);
	}
	g_p.discs = 0;
	for (int c = 0; c < CARDS; c++) {
		pause_card_rect(CARDS, c, r);
		CHECK(r[2] >= 62 && r[3] >= 62);
	}
	for (int b = 0; b < 3; b++) {
		pause_button_rect(b, r);
		CHECK(r[2] >= 62 && r[3] >= 62);
	}
}

/* ---- controllers and the legend ---- */

static void each_controller_has_its_own_buttons(void)
{
	int e[8];
	int n = pause_events(INPUT_WIIMOTE, 1u << SRC_2 | 1u << SRC_1 | 1u << SRC_MINUS, e, 8);
	CHECK(n == 3);
	int ok = 0, back = 0, del = 0;
	for (int i = 0; i < n; i++) {
		ok |= e[i] == PEV_OK;
		back |= e[i] == PEV_BACK;
		del |= e[i] == PEV_DELETE;
	}
	CHECK(ok && back && del);
	CHECK(pause_events(INPUT_GAMEPAD, 1u << SRC_MINUS, e, 8) == 0);
	CHECK(pause_events(INPUT_PRO, 1u << SRC_Y, e, 8) == 1 && e[0] == PEV_DELETE);
	CHECK(pause_events(INPUT_CLASSIC, 1u << SRC_LSTICK_DOWN, e, 8) == 1 && e[0] == PEV_DOWN);
}

static void the_legend_names_what_acts(void)
{
	int s[CARDS] = { CARD_FILLED, CARD_EMPTY, CARD_DAMAGED };
	int dim = 0;
	open_with(s, 0);
	CHECK(has_key(GLYPH_HOME, "Wii U Menu", NULL, INPUT_GAMEPAD) && has_key(GLYPH_B, "Resume", NULL, INPUT_GAMEPAD));
	CHECK(has_key(GLYPH_A, "OK", NULL, INPUT_GAMEPAD));
	go_item(ITEM_SAVE);
	ev(PEV_OK);
	CHECK(has_key(GLYPH_A, "Overwrite", &dim, INPUT_GAMEPAD) && !dim);
	CHECK(has_key(GLYPH_Y, "Delete", NULL, INPUT_GAMEPAD));
	CHECK(has_key(GLYPH_MINUS, "Delete", NULL, INPUT_WIIMOTE) && has_key(GLYPH_2, "Overwrite", NULL, INPUT_WIIMOTE));
	CHECK(has_key(GLYPH_B, "Back", NULL, INPUT_GAMEPAD) && has_key(GLYPH_1, "Back", NULL, INPUT_WIIMOTE));
	ev(PEV_RIGHT);
	CHECK(has_key(GLYPH_A, "Save here", &dim, INPUT_GAMEPAD) && !dim);
	CHECK(!has_key(GLYPH_Y, NULL, NULL, INPUT_GAMEPAD));
	ev(PEV_BACK);
	go_item(ITEM_LOAD);
	ev(PEV_OK);
	g_p.card = CARD_SLOT3;
	CHECK(has_key(GLYPH_A, "Load", &dim, INPUT_GAMEPAD) && dim);
	g_p.card = CARD_SLOT1;
	ev(PEV_OK);
	CHECK(has_key(GLYPH_B, "Cancel", NULL, INPUT_GAMEPAD) && has_key(GLYPH_A, "OK", NULL, INPUT_GAMEPAD));
	ev(PEV_HOME);
	CHECK(!has_key(GLYPH_HOME, NULL, NULL, INPUT_GAMEPAD) && has_key(GLYPH_B, "Cancel", NULL, INPUT_GAMEPAD));
}

static void the_legend_puts_home_left_and_a_right(void)
{
	pause_key k[8];
	open_with(NULL, 0);
	int n = pause_legend(&g_p, INPUT_GAMEPAD, k, 8);
	CHECK(n >= 3 && k[0].glyph == GLYPH_HOME && k[n - 1].glyph == GLYPH_A);
}

/* ---- words ---- */

static void cards_say_when_or_why_not(void)
{
	char s[96];
	pause_card c = { CARD_FILLED, NOW - 120, 1, 0 };
	pause_card_detail(s, sizeof(s), &c, NOW, 1);
	CHECK(!strcmp(s, "today 9:38 PM"));
	c.when = NOW - DAY;
	pause_card_detail(s, sizeof(s), &c, NOW, 1);
	CHECK(!strcmp(s, "yesterday 9:40 PM"));
	c.status = CARD_EMPTY;
	pause_card_detail(s, sizeof(s), &c, NOW, 1);
	CHECK(!strcmp(s, "Empty"));
	c.status = CARD_DAMAGED;
	pause_card_detail(s, sizeof(s), &c, NOW, 1);
	CHECK(!strcmp(s, "Damaged: can't load"));
	c.status = CARD_OTHER_VERSION;
	pause_card_detail(s, sizeof(s), &c, NOW, 1);
	CHECK(!strcmp(s, "Made by another Our64 version: can't load"));
	c.status = CARD_OTHER_GAME;
	pause_card_detail(s, sizeof(s), &c, NOW, 1);
	CHECK(!strcmp(s, "Made for another game: can't load"));
}

static void the_undo_card_is_named_for_what_it_undoes(void)
{
	char s[96];
	int st[CARDS] = { CARD_EMPTY, CARD_EMPTY, CARD_EMPTY, CARD_FILLED };
	open_with(st, UNDO_SAVE);
	g_p.cards[CARD_UNDO].slot = 2;
	CHECK(!strcmp(pause_card_name(&g_p, CARD_UNDO), "Undo save"));
	pause_card_description(s, sizeof(s), &g_p, CARD_UNDO);
	CHECK(!strcmp(s, "Slot 2 before you saved over it"));
	g_p.cards[CARD_UNDO].undo = UNDO_LOAD;
	CHECK(!strcmp(pause_card_name(&g_p, CARD_UNDO), "Undo load"));
	pause_card_description(s, sizeof(s), &g_p, CARD_UNDO);
	CHECK(!strcmp(s, "Before your last load"));
	g_p.cards[CARD_UNDO].undo = UNDO_RESET;
	pause_card_description(s, sizeof(s), &g_p, CARD_UNDO);
	CHECK(!strcmp(s, "Before the reset"));
	g_p.cards[CARD_UNDO].status = CARD_EMPTY;
	g_p.cards[CARD_UNDO].undo = 0;
	CHECK(!strcmp(pause_card_name(&g_p, CARD_UNDO), "Undo"));
	CHECK(!strcmp(pause_card_name(&g_p, CARD_SLOT3), "Slot 3"));
}

static void dialogs_name_the_action_and_its_target(void)
{
	int s[CARDS] = { CARD_FILLED, CARD_FILLED, CARD_EMPTY, CARD_FILLED };
	pause_ask_text t;
	open_with(s, UNDO_LOAD);
	go_item(ITEM_SAVE);
	ev(PEV_OK);
	CHECK(g_p.card == CARD_SLOT2);
	ev(PEV_OK);
	pause_ask_words(&g_p, NOW, 1, &t);
	CHECK(!strcmp(t.question, "Overwrite Slot 2?") && !strcmp(t.button, "Overwrite Slot 2"));
	CHECK(t.lines == 0); /* the Undo card speaks for itself: only Delete warns */
	CHECK(t.thumbs == 2 && !strcmp(t.thumb_label[0], "Now") && strstr(t.thumb_label[1], "Slot") == NULL);
	ev(PEV_BACK);
	ev(PEV_DELETE);
	pause_ask_words(&g_p, NOW, 1, &t);
	CHECK(!strcmp(t.question, "Delete Slot 2?") && !strcmp(t.button, "Delete Slot 2"));
	CHECK(!strcmp(t.body[0], "You can't undo this"));
	ev(PEV_BACK);
	ev(PEV_BACK);
	go_item(ITEM_RESET);
	ev(PEV_OK);
	pause_ask_words(&g_p, NOW, 1, &t);
	CHECK(!strcmp(t.question, "Reset the game?") && !strcmp(t.button, "Reset game"));
	CHECK(t.lines == 2 && !strcmp(t.body[0], "Back to the Title Screen") &&
	      !strcmp(t.body[1], "Undo Reset in Load State screen") && t.thumb_label[0][0] == '\0');
}

static void the_load_dialog_has_no_body_text(void)
{
	int s[CARDS] = { CARD_FILLED, CARD_EMPTY, CARD_EMPTY, CARD_FILLED };
	pause_ask_text t;
	open_with(s, UNDO_RESET);
	go_item(ITEM_LOAD);
	ev(PEV_OK);
	g_p.card = CARD_SLOT1;
	ev(PEV_OK);
	pause_ask_words(&g_p, NOW, 1, &t);
	CHECK(!strcmp(t.question, "Load Slot 1?"));
	CHECK(t.lines == 0);
	CHECK(t.thumbs == 1 && strstr(t.thumb_label[0], "Slot") == NULL);
}

static void the_autosave_line_says_how_long_ago(void)
{
	char l[4][128];
	int red;
	pause_autosave a = { 0 };
	a.last_ok = NOW - 120;
	int n = pause_autosave_lines(&a, NOW, 1, l, &red);
	CHECK(n == 1 && !red && !strcmp(l[0], "Autosaved: 2 minutes ago"));
	a.last_ok = NOW - 20;
	pause_autosave_lines(&a, NOW, 0, l, &red);
	CHECK(!strcmp(l[0], "Autosaved: just now"));
	a.last_ok = NOW - 3 * 3600 - 50;
	pause_autosave_lines(&a, NOW, 1, l, &red);
	CHECK(!strcmp(l[0], "Autosaved: 3 hours ago"));
	a.from_launch = 1;
	a.last_ok = NOW - 86400 - 10;
	pause_autosave_lines(&a, NOW, 1, l, &red);
	CHECK(!strcmp(l[0], "Autosaved: 1 day ago"));
	a.last_ok = 0;
	pause_autosave_lines(&a, NOW, 1, l, &red);
	CHECK(!strcmp(l[0], "Autosaved: not yet"));
	a.writing = 1;
	pause_autosave_lines(&a, NOW, 1, l, &red);
	CHECK(!strcmp(l[0], "Saving to SD card...") && !red);
}

/* Auto-Resume off: no autosave line at all; an in-game save that failed still says so. */
static void with_auto_resume_off_only_in_game_saves_are_named(void)
{
	char l[4][128];
	int red;
	pause_autosave a = { 0 };
	a.off = 1;
	CHECK(pause_autosave_lines(&a, NOW, 1, l, &red) == 0 && !red);
	a.writing = 1; /* an in-game save going out */
	CHECK(pause_autosave_lines(&a, NOW, 1, l, &red) == 1 && !strcmp(l[0], "Saving to SD card..."));
	a.writing = 0;
	a.ingame_failed = NOW - 60;
	int n = pause_autosave_lines(&a, NOW, 1, l, &red);
	CHECK(red && n == 2 && !strcmp(l[0], "Your in-game save from 9:39 PM is not on the SD card."));
	CHECK(strstr(l[1], "OUR-0201") != NULL);
	a.exit_failed = 1;
	n = pause_autosave_lines(&a, NOW, 1, l, &red);
	CHECK(n == 3 && strstr(l[2], "Leave without saving") && !strstr(l[2], "autosave"));
}

static void a_failed_write_says_what_is_safe_and_what_to_do(void)
{
	char l[4][128];
	int red;
	pause_autosave a = { 0 };
	a.last_ok = NOW - 120;
	a.ingame_failed = NOW - 60;
	int n = pause_autosave_lines(&a, NOW, 1, l, &red);
	CHECK(red && n == 3);
	CHECK(!strcmp(l[0], "Your in-game save from 9:39 PM is not on the SD card."));
	CHECK(strstr(l[1], "9:38 PM") != NULL);
	CHECK(strstr(l[2], "OUR-0201") != NULL && strstr(l[2], "lock") != NULL);
	a.ingame_failed = 0;
	a.autosave_failed = 1;
	pause_autosave_lines(&a, NOW, 1, l, &red);
	CHECK(red && strstr(l[0], "autosave") != NULL);
	a.exit_failed = 1; /* the last good autosave named once, in the Leave line (hci review 2026-10-23) */
	CHECK(pause_autosave_lines(&a, NOW, 1, l, &red) == 3);
	CHECK(strstr(l[2], "Leave without saving") != NULL && strstr(l[2], "9:38 PM") != NULL);
	CHECK(strstr(l[1], "9:38 PM") == NULL);
}

#define RUN(fn) do { g_run++; int before = g_failed; fn(); if (g_failed == before) printf("  ok   %s\n", #fn); } while (0)

/* Zach, 2026-10-19: HOME asks before the Wii U Menu on every screen. */
static void home_outside_a_game_asks_with_cancel_first(void)
{
	int focus = 0;
	CHECK(exit_ask_event(&focus, PEV_OK, -1) == -1); /* a reflex A stays */
	CHECK(exit_ask_event(&focus, PEV_RIGHT, -1) == 0 && focus == 1);
	CHECK(exit_ask_event(&focus, PEV_OK, -1) == 1);
	focus = 1;
	CHECK(exit_ask_event(&focus, PEV_HOME, -1) == -1);
	CHECK(exit_ask_event(&focus, PEV_BACK, -1) == -1);
	int r[4];
	pause_button_rect(1, r);
	CHECK(exit_ask_hit(r[0] + 2, r[1] + 2) == 1);
	pause_button_rect(0, r);
	CHECK(exit_ask_hit(r[0] + 2, r[1] + 2) == 0 && exit_ask_hit(0, 0) == -1);
	focus = 0;
	CHECK(exit_ask_event(&focus, PEV_TOUCH_UP, 1) == 1);
	pause_key k[4];
	CHECK(exit_ask_legend(INPUT_PRO, k, 4) == 3 && k[0].glyph == GLYPH_B && !strcmp(k[2].label, "OK"));
}

int main(void)
{
	setvbuf(stdout, NULL, _IOLBF, 0);
	RUN(the_menu_opens_on_resume_every_time);
	RUN(six_items_in_two_columns_of_three);
	RUN(b_and_resume_close_the_menu);
	RUN(home_asks_before_the_wii_u_menu);
	RUN(a_disabled_item_says_why);
	RUN(home_on_a_dialog_cancels_it_and_asks);
	RUN(menu_items_lead_where_they_say);
	RUN(a_failed_exit_moves_to_resume_and_renames_exit);
	RUN(without_save_states_their_items_do_nothing);
	RUN(save_mode_starts_on_the_last_slot_used);
	RUN(save_mode_starts_on_the_first_empty_slot_on_first_use);
	RUN(load_mode_starts_on_the_newest_copy);
	RUN(cards_move_without_wrapping);
	RUN(b_goes_back_with_the_item_still_selected);
	RUN(a_on_an_empty_slot_saves_there);
	RUN(a_on_a_filled_slot_asks_to_overwrite_with_cancel_focused);
	RUN(unloadable_slots_can_be_saved_over);
	RUN(load_mode_asks_before_loading);
	RUN(delete_works_on_filled_slots_only);
	RUN(reset_cancel_returns_to_reset_game);
	RUN(presses_wait_while_busy);
	RUN(a_failure_waits_for_ok);
	RUN(a_tap_selects_and_a_second_tap_acts);
	RUN(one_tap_on_delete_does_not_delete);
	RUN(the_undo_card_cannot_be_tapped_in_save_mode);
	RUN(the_cards_are_centred_in_both_modes);
	RUN(change_disc_is_there_for_several_discs);
	RUN(targets_are_big_enough_to_touch);
	RUN(each_controller_has_its_own_buttons);
	RUN(the_legend_names_what_acts);
	RUN(the_legend_puts_home_left_and_a_right);
	RUN(cards_say_when_or_why_not);
	RUN(the_undo_card_is_named_for_what_it_undoes);
	RUN(dialogs_name_the_action_and_its_target);
	RUN(the_load_dialog_has_no_body_text);
	RUN(the_autosave_line_says_how_long_ago);
	RUN(with_auto_resume_off_only_in_game_saves_are_named);
	RUN(a_failed_write_says_what_is_safe_and_what_to_do);
	RUN(home_outside_a_game_asks_with_cancel_first);
	printf("%d of %d failed\n", g_failed, g_run);
	return g_failed != 0;
}
