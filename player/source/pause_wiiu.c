/* Our64 player: the pause menu on the console (ux/flows/pause). It runs on
 * the CPU core with the game frozen at an instruction boundary (states_wiiu.c
 * captured it into the pause buffer), as run_prompt runs the launch prompt:
 * read the controllers, update pause.c's model, show a frame, every 16 ms.
 *
 * Writes go through the SD writer, and the screen says what is happening
 * while presses wait. Load and Reset first keep the game now as the Undo
 * card; if that fails, nothing changes. Exit to Our ROMs, and HOME then
 * Exit, write everything, and stay on the menu once if a write failed,
 * saying what is not on the card (Exit then reads "Leave without saving"). */
#include "pause_wiiu.h"
#include "player.h"
#include "input.h"
#include "ui.h"
#include "report.h"
#include "writer.h"

#include <coreinit/thread.h>
#include <coreinit/time.h>

#include <stdio.h>
#include <string.h>
#include <time.h>

static pause_view g_view;
static uint16_t g_thumbs[CARDS][THUMB_W * THUMB_H] __attribute__((aligned(64)));
static unsigned g_gen;
static int g_exit_failed; /* Exit found failed writes once: the next Exit closes anyway */

static void show(void)
{
	pause_view *pv = &g_view;
	pv->now = time(NULL);
	pause_autosave a;
	states_autosave(&a);
	a.exit_failed = g_exit_failed;
	pv->autosave_lines = pause_autosave_lines(&a, pv->now, pv->us, pv->autosave, &pv->autosave_red);
	pv->ui.failure_shown = pv->autosave_red; /* the panel under the items: the grid makes room (pause.h) */
	if (pv->ui.screen == PAUSE_SLOTS && pv->ui.mode == SLOTS_LOAD && !pv->ui.line[0] && !pv->ui.busy) {
		int empty = 1;
		for (int c = 0; c < CARD_SLOTS; c++)
			empty &= pv->ui.cards[c].status == CARD_EMPTY;
		if (empty)
			snprintf(pv->ui.line, sizeof(pv->ui.line), "No save states yet: go back and choose Save state");
	}
	ui_view v;
	memset(&v, 0, sizeof(v));
	v.screen = UI_PAUSE;
	snprintf(v.title, sizeof(v.title), "%.63s", pv->title);
	v.pause = pv;
	ui_show(&v);
}

static void scan(int card)
{
	int pic = states_card_scan(card, &g_view.ui.cards[card], g_thumbs[card]);
	g_view.thumb[card] = pic ? g_thumbs[card] : NULL;
	g_view.thumb_gen[card] = ++g_gen;
}

/* Until the writer has run the job: the screen keeps saying what is
 * happening; presses made meanwhile are dropped. */
static int wait_job(const char *what)
{
	snprintf(g_view.busy, sizeof(g_view.busy), "%s", what);
	g_view.ui.busy = 1;
	OSTime t0 = OSGetSystemTime();
	while (states_job_running()) {
		show();
		player_menu_wait();
		input_read();
	}
	input_take_home();
	g_view.ui.busy = 0;
	g_view.busy_card = -1;
	report_line("PAUSE: %s took %.0f ms", what, OSTicksToMicroseconds(OSGetSystemTime() - t0) / 1e3);
	return states_job_result();
}

static void busy_frame(const char *what)
{
	snprintf(g_view.busy, sizeof(g_view.busy), "%s", what);
	g_view.ui.busy = 1;
	show();
}

/* A failure card, in the log too (the public build keeps lines with a code). */
static void fail(const char *title, const char *body)
{
	report_line("ERROR %s: %s", title, body);
	pause_fail(&g_view.ui, title, body);
}

#define SD_FIX "The SD card may be full or locked: delete a save state to make room, or check the card, then try again. OUR-0202"

static void backup_failed(void)
{
	fail("Couldn't keep a backup first",
	           "So nothing was changed. The SD card may be full or locked: delete a save state to make room, "
	           "or check the card, then try again. OUR-0202");
}

/* Saves the paused game to a slot (A on an empty slot, or Overwrite). */
static void save_slot(int card, int overwrite)
{
	char what[64];
	const int keep_old = overwrite && pause_card_kept(&g_view.ui, card);
	snprintf(what, sizeof(what), "Saving to %s...", pause_card_name(&g_view.ui, card));
	g_view.busy_card = card;
	states_card_save(card, keep_old, 0);
	int r = wait_job(what);
	scan(card);
	if (keep_old)
		scan(CARD_UNDO);
	if (overwrite)
		pause_back_to_slots(&g_view.ui);
	if (r == STATEFILE_OK) {
		snprintf(g_view.ui.line, sizeof(g_view.ui.line), keep_old ? "Saved to %s. Its old save state is Undo save."
		                                                          : "Saved to %s", pause_card_name(&g_view.ui, card));
	} else if (r == STATEFILE_BACKUP_FAILED) {
		backup_failed();
	} else {
		char title[64];
		snprintf(title, sizeof(title), "Couldn't save to %s", pause_card_name(&g_view.ui, card));
		fail(title, SD_FIX);
	}
}

static void delete_slot(int card)
{
	busy_frame("Deleting...");
	int r = states_card_delete(card);
	g_view.ui.busy = 0;
	scan(card);
	pause_back_to_slots(&g_view.ui);
	if (r == 0) {
		snprintf(g_view.ui.line, sizeof(g_view.ui.line), "%s deleted", pause_card_name(&g_view.ui, card));
	} else {
		char title[64];
		snprintf(title, sizeof(title), "Couldn't delete %s", pause_card_name(&g_view.ui, card));
		fail(title, "The SD card may be locked or removed: check it, then try again. OUR-0202");
	}
}

/* Read the copy first, so loading the Undo card swaps the two; then keep
 * the game now as Undo load; only then may go() stop and the copy take over. */
static int load_card(int card)
{
	char what[96];
	snprintf(what, sizeof(what), "Reading %s...", pause_card_name(&g_view.ui, card));
	busy_frame(what);
	int r = states_card_read(card);
	g_view.ui.busy = 0;
	if (r != STATEFILE_OK) {
		char title[64];
		scan(card);
		if (g_view.ui.cards[card].status == CARD_FILLED)
			g_view.ui.cards[card].status = CARD_DAMAGED; /* the header was fine, the state was not */
		pause_back_to_slots(&g_view.ui);
		snprintf(title, sizeof(title), "%s can't be loaded", pause_card_name(&g_view.ui, card));
		fail(title, "It is damaged or was made by another Our64 version. Nothing was changed. OUR-0302");
		return -1;
	}
	states_card_save(CARD_UNDO, 0, UNDO_LOAD);
	if (wait_job("SAVING...") != STATEFILE_OK) {
		scan(CARD_UNDO);
		backup_failed();
		return -1;
	}
	snprintf(what, sizeof(what), "Loading %s...", pause_card_name(&g_view.ui, card));
	busy_frame(what);
	report_line("PAUSE: loading %s", card_file(card));
	return PAUSE_LOAD;
}

static int reset_game(void)
{
	states_card_save(CARD_UNDO, 0, UNDO_RESET);
	if (wait_job("SAVING...") != STATEFILE_OK) {
		scan(CARD_UNDO);
		backup_failed();
		return -1;
	}
	busy_frame("Resetting...");
	report_line("PAUSE: reset");
	return PAUSE_RESET;
}

/* Everything to the SD card under "SAVING...". A failed write
 * keeps the player on the menu once, saying what is not on the card; a
 * power press cannot wait for that. where: LEAVE_. */
static int leave(int where)
{
	const int system = where == LEAVE_SYSTEM;
	saves_flush();
	ui_view v;
	memset(&v, 0, sizeof(v));
	v.screen = UI_CLOSING;
	v.nothing_to_save = !system && !states_writing(); /* the menu's resume point is already on the card */
	snprintf(v.title, sizeof(v.title), "%s", g_view.title);
	ui_show(&v);
	OSTime t0 = OSGetSystemTime();
	writer_drain();
	pause_autosave a;
	states_autosave(&a);
	const int failed = a.autosave_failed || a.ingame_failed;
	report_line("PAUSE: %s after %.0f ms of writes%s",
	            system ? "closing for the system" : where == LEAVE_ROMS ? "Exit to Our ROMs" : "Exit to the Wii U Menu",
	            OSTicksToMicroseconds(OSGetSystemTime() - t0) / 1e3, failed ? ", some failed" : "");
	if (failed && !system && !g_exit_failed) {
		g_exit_failed = 1;
		pause_exit_failed(&g_view.ui);
		input_take_home();
		return -1;
	}
	player_left(where, g_exit_failed);
	return where == LEAVE_ROMS ? PAUSE_ROMS : PAUSE_EXIT;
}

/* What the model asked for; -1 while the menu stays open. */
static int act(int what)
{
	pause_ui *p = &g_view.ui;
	switch (what) {
	case PACT_RESUME: return PAUSE_RESUME;
	case PACT_EXIT_ROMS: return leave(LEAVE_ROMS);
	case PACT_EXIT_WIIU: return leave(LEAVE_WIIU);
	case PACT_SETTINGS:
		settings_run(1);
		input_take_home();
		g_view.ui.screen = PAUSE_MENU; /* back here, Settings still selected */
		if (settings_take_home()) /* HOME there: asks here before the Wii U Menu */
			pause_event(&g_view.ui, PEV_HOME, -1);
		break;
	case PACT_OPTIONS: {
		const int r = game_options_run(player_rom_file(), g_view.title, g_view.glyphs, 1, 0);
		const int home = r == 1;
		input_take_home();
		if (r == 3) { /* a new resolution, restart accepted: out as for Our ROMs, and straight back in */
			player_restart_request();
			return leave(LEAVE_ROMS);
		}
		g_view.ui.screen = PAUSE_MENU; /* back here, Game options still selected */
		if (home) /* HOME there: asks here before the Wii U Menu */
			pause_event(&g_view.ui, PEV_HOME, -1);
		break;
	}
	case PACT_SAVE: save_slot(p->card, 0); break;
	case PACT_OVERWRITE: save_slot(p->ask_card, 1); break;
	case PACT_DELETE: delete_slot(p->ask_card); break;
	case PACT_LOAD: return load_card(p->ask_card);
	case PACT_RESET: return reset_game();
	}
	return -1;
}

static const char *const g_done_name[] = { "resume", "load", "reset", "exit", "Our ROMs" };

int pause_run(int captured)
{
	pause_view *pv = &g_view;
	memset(pv, 0, sizeof(*pv));
	pause_open(&pv->ui, captured);
	pv->busy_card = -1;
	pv->us = player_us();
	snprintf(pv->title, sizeof(pv->title), "%s", player_title());
	g_exit_failed = 0;
	player_paused(1);
	input_rumble_hold(1);
	input_menu m;
	input_menu_state(&m);
	pv->glyphs = m.last_id >= 0 && m.type[m.last_id] >= 0 ? m.type[m.last_id] : INPUT_GAMEPAD;
	OSTime t0 = OSGetSystemTime();
	show(); /* the first frame freezes the game's last one behind the menu */
	if (captured) {
		ui_thumbnail(states_now_thumb());
		pv->now_thumb = states_now_thumb();
		pv->now_gen = ++g_gen;
		for (int c = 0; c < CARDS; c++)
			scan(c);
	}
	report_line("PAUSE: menu up in %.0f ms (cards read too)", OSTicksToMicroseconds(OSGetSystemTime() - t0) / 1e3);
	report_save();
	input_take_home(); /* the press that opened it */
	int touching = 0, touch_target = -1, done = -1;
	while (done < 0) {
		show();
		player_menu_wait();
		input_read();
		input_menu_state(&m);
		if (player_closing()) {
			done = leave(LEAVE_SYSTEM);
			break;
		}
		int what = PACT_NONE;
		if (input_take_home())
			what = pause_event(&pv->ui, PEV_HOME, -1);
		for (int id = 0; id < 5 && what == PACT_NONE; id++) {
			if (m.type[id] < 0 || !m.pressed[id])
				continue;
			int events[8];
			int n = pause_events(m.type[id], m.pressed[id], events, 8);
			for (int i = 0; i < n && what == PACT_NONE; i++)
				what = pause_event(&pv->ui, events[i], -1);
		}
		if (what == PACT_NONE && m.touch && !touching) {
			touching = 1;
			touch_target = pause_hit(&pv->ui, m.touch_x, m.touch_y);
			pause_event(&pv->ui, PEV_TOUCH_DOWN, touch_target);
		} else if (what == PACT_NONE && m.touch) {
			touch_target = pause_hit(&pv->ui, m.touch_x, m.touch_y);
		} else if (what == PACT_NONE && touching) {
			touching = 0;
			what = pause_event(&pv->ui, PEV_TOUCH_UP, touch_target);
		}
		if (m.last_id >= 0 && m.type[m.last_id] >= 0)
			pv->glyphs = m.type[m.last_id];
		if (what != PACT_NONE)
			done = act(what);
	}
	report_line("PAUSE: menu closed (%s) after %.1f s", g_done_name[done],
	            OSTicksToMicroseconds(OSGetSystemTime() - t0) / 1e6);
	report_save();
	input_withhold(); /* the press that closed it never reaches the game */
	player_paused(0);
	input_rumble_hold(0);
	if (done == PAUSE_RESUME)
		ui_resume();
	return done;
}

/* HOME outside a game: "Exit to the Wii U Menu?" over the screen shown last,
 * until Cancel or Exit (pause.h). 1: leave. The system closing Our64 leaves. */
int player_ask_exit(void) { return player_ask(NULL, NULL, NULL); }

/* A question over the screen shown last, Cancel focused: title, one body line
 * and the action's button (NULL title: the Exit question). 1 for the action;
 * HOME or B cancels. */
int player_ask(const char *title, const char *body, const char *action)
{
	input_menu m;
	input_menu_state(&m);
	int glyphs = m.last_id >= 0 && m.type[m.last_id] >= 0 ? m.type[m.last_id] : INPUT_GAMEPAD;
	int focus = 0, r = 0, touching = 0, target = -1;
	input_take_home();
	while (r == 0) {
		ui_show_ask(title, body, action, focus, glyphs);
		player_menu_wait();
		input_read();
		input_menu_state(&m);
		if (player_closing())
			return title == NULL; /* closing: leave, never act */
		if (input_take_home())
			r = exit_ask_event(&focus, PEV_HOME, -1);
		for (int id = 0; id < INPUT_CONTROLLERS && r == 0; id++) {
			if (m.type[id] < 0 || !m.pressed[id])
				continue;
			glyphs = m.type[id];
			int ev[8];
			int n = pause_events(m.type[id], m.pressed[id], ev, 8);
			for (int k = 0; k < n && r == 0; k++)
				r = exit_ask_event(&focus, ev[k], -1);
		}
		if (r == 0 && m.touch) {
			target = exit_ask_hit(m.touch_x, m.touch_y);
			if (!touching)
				r = exit_ask_event(&focus, PEV_TOUCH_DOWN, target);
			touching = 1;
		} else if (r == 0 && touching) {
			touching = 0;
			r = exit_ask_event(&focus, PEV_TOUCH_UP, target);
		}
	}
	input_withhold(); /* the answering press does nothing on the screen below */
	report_line("UI: %s asked, %s", title ? title : "HOME", r > 0 ? (action ? action : "Exit") : "Cancel");
	return r > 0;
}
