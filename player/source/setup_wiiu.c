/* Our64 player: Our Settings, Our Controllers, Change order and Edit
 * buttons on the console (ux/flows/setup). Each runs on the CPU core like
 * the pause menu: read the controllers, update setup.c's model, show a
 * frame, every 16 ms. Settings go to config/settings.cfg, buttons to
 * config/controllers.cfg or the game's profile, the moment they are applied
 * or saved; a failed write says so with OUR-0203 and changes nothing. */
#include "pause_wiiu.h"
#include "theme.h"
#include "setup.h"
#include "player.h"
#include "input.h"
#include "ui.h"
#include "report.h"
#include "saves.h"

#include <coreinit/thread.h>
#include <coreinit/time.h>

#include <stdio.h>
#include <string.h>

#define BIT(s) (1u << (s))

static setup_view g_view;
static our_settings g_settings;
static char g_settings_path[300];
static int g_from_pause;
static int g_home_asked; /* HOME on a setup screen: the caller handles it */

int settings_take_home(void)
{
	int h = g_home_asked;
	g_home_asked = 0;
	return h;
}

void setup_start(const char *root, const our_settings *s)
{
	g_settings = *s;
	snprintf(g_settings_path, sizeof(g_settings_path), "%s/config/settings.cfg", root);
	for (int p = 0; p < INPUT_PORTS; p++)
		input_rumble_allow(p, g_settings.rumble[p]);
}

const our_settings *setup_settings(void) { return &g_settings; }

int setup_save_settings(void)
{
	char text[2048];
	int n = settings_format_all(text, sizeof(text), &g_settings);
	int r = savefile_write(g_settings_path, text, (unsigned)n);
	report_line("SETTINGS: %s", r ? "NOT written" : "written");
	return r;
}

static double now_s(void) { return (double)OSTicksToMicroseconds(OSGetSystemTime()) / 1e6; }

static void show(void)
{
	ui_view u;
	memset(&u, 0, sizeof(u));
	u.screen = UI_SETUP;
	u.setup = &g_view;
	ui_show(&u);
}

/* One frame of input: the events of every controller in order, and touch. */
typedef struct {
	int events[24], targets[24], n;
	input_menu m;
} frame_input;

/* events: how a controller's presses become events (pause_events, or Edit buttons' own). */
static void read_frame_with(frame_input *f, int (*events)(int type, uint32_t pressed, int *out, int max),
                            int (*hit)(int x, int y), int *touching, int *target)
{
	player_menu_wait();
	input_read();
	input_menu_state(&f->m);
	f->n = 0;
	if (input_take_home())
		f->events[f->n] = PEV_HOME, f->targets[f->n++] = -1;
	for (int id = 0; id < INPUT_CONTROLLERS; id++) {
		if (f->m.type[id] < 0 || !f->m.pressed[id])
			continue;
		g_view.glyphs = f->m.type[id];
		int ev[8];
		int k = events(f->m.type[id], f->m.pressed[id], ev, 8);
		for (int i = 0; i < k && f->n < 23; i++)
			f->events[f->n] = ev[i], f->targets[f->n++] = -1;
	}
	if (f->m.touch && !*touching) {
		*touching = 1;
		*target = hit ? hit(f->m.touch_x, f->m.touch_y) : -1;
		f->events[f->n] = PEV_TOUCH_DOWN, f->targets[f->n++] = *target;
	} else if (f->m.touch) {
		*target = hit ? hit(f->m.touch_x, f->m.touch_y) : -1;
	} else if (*touching) {
		*touching = 0;
		f->events[f->n] = PEV_TOUCH_UP, f->targets[f->n++] = *target;
	}
}

static void read_frame(frame_input *f, int (*hit)(int x, int y), int *touching, int *target)
{
	read_frame_with(f, pause_events, hit, touching, target);
}

/* HOME on a setup screen: from Our ROMs it asks "Exit to the Wii U Menu?"
 * here, over the screen, and Cancel stays on it; from a paused game the pause
 * menu asks once the setup screens close. */
static int home_ok(void) { return g_from_pause || player_ask_exit(); }

static void line(const char *s) { snprintf(g_view.line, sizeof(g_view.line), "%s", s); }

static void write_failed(void)
{
	report_line("ERROR settings not written: OUR-0203");
	line("Couldn't save to the SD card: check its lock switch or free space. OUR-0203");
}

/* ---- Change order ---- */

/* 1: HOME, and leaving was confirmed (or the pause menu asks). */
static int run_order(void)
{
	order_open(&g_view.order, input_connected_count());
	g_view.screen = SETUP_ORDER;
	input_menu m;
	input_read();
	input_take_home();
	for (;;) {
		show();
		player_menu_wait();
		input_read();
		input_menu_state(&m);
		if (player_closing())
			break;
		if (input_take_home() && home_ok()) {
			g_view.screen = SETUP_CONTROLLERS;
			return 1;
		}
		int done = 0;
		for (int id = 0; id < INPUT_CONTROLLERS && !done; id++) {
			if (m.type[id] < 0 || !m.pressed[id])
				continue;
			if (m.pressed[id] & (BIT(SRC_B) | (m.type[id] == INPUT_WIIMOTE ? BIT(SRC_1) : 0))) {
				if (g_view.order.count == 0)
					goto cancel;
				/* B with players set: just those play, the others leave (Zach, 2026-10-20) */
				input_set_players(g_view.order.ids, g_view.order.count);
				line(g_view.order.count == 1 ? "Only that controller plays now: Player 1." : "Only those controllers play now.");
				g_view.screen = SETUP_CONTROLLERS;
				return 0;
			}
			if (m.pressed[id] & (BIT(SRC_A) | (m.type[id] == INPUT_WIIMOTE ? BIT(SRC_2) : 0)))
				done = order_press(&g_view.order, id);
		}
		if (done) {
			input_reorder(g_view.order.ids, g_view.order.count);
			line("Order changed. Rumble stays with each player number.");
			g_view.screen = SETUP_CONTROLLERS;
			return 0;
		}
	}
cancel:
	line("Order kept as it was.");
	g_view.screen = SETUP_CONTROLLERS;
	return 0;
}

/* ---- Edit buttons ---- */

/* What mapping.c asked for. Every change is written at once: the menus read
 * the controllers' own buttons, never the mapping, so none locks a player
 * out. 1 to leave, 2 to leave for HOME (confirmed), else 0. */
static int mapping_act(mapping_ui *mp, int type, int act)
{
	switch (act) {
	case MACT_BACK: return 1;
	case MACT_HOME: return home_ok() ? 2 : 0;
	case MACT_WRITE:
		if (input_map_save(type, &mp->edit, mp->tab == MAP_TAB_GAME)) {
			report_line("ERROR buttons not written: OUR-0203");
			mapping_fail(mp, "Couldn't save your buttons", "Check the SD card's lock switch or free space, then try again. OUR-0203");
		} else
			mapping_written(mp);
		break;
	case MACT_REMOVE_PROFILE:
		if (input_profile_remove(type)) {
			report_line("ERROR profile not removed: OUR-0203");
			mapping_fail(mp, "Couldn't remove the profile", "Check the SD card's lock switch, then try again. OUR-0203");
		} else
			mapping_profile_removed(mp);
		break;
	}
	return 0;
}

static int run_mapping(int port)
{
	int owner = input_port_owner(port);
	int type = input_controller_type(owner);
	if (type < 0)
		return 0;
	input_map all, in_use;
	int profile;
	input_map_all(type, &all);
	input_map_get(type, &in_use, &profile);
	mapping_ui *mp = &g_view.mapping;
	mapping_open(mp, type, &all, profile ? &in_use : NULL, g_from_pause ? g_view.game : NULL);
	g_view.screen = SETUP_MAPPING;
	frame_input f;
	memset(&f, 0, sizeof(f));
	int touching = 0, target = -1, leave = 0;
	while (!leave) {
		show();
		read_frame_with(&f, mapping_events, NULL, &touching, &target);
		if (player_closing())
			return 1;
		const int capturing = mp->screen == MAP_CAPTURE; /* this frame's presses are for the capture only */
		if (capturing) {
			for (int id = 0; id < INPUT_CONTROLLERS; id++)
				if (input_controller_type(id) == type && f.m.sources[id])
					mapping_act(mp, type, mapping_capture(mp, f.m.sources[id] & ~0u));
			mapping_tick(mp, now_s());
		}
		for (int i = 0; i < f.n && !leave; i++) {
			if (capturing && (f.events[i] != PEV_HOME || mp->screen != MAP_CAPTURE))
				continue; /* captured above; HOME cancels only a capture still waiting */
			if (f.events[i] == PEV_TOUCH_DOWN || f.events[i] == PEV_TOUCH_UP)
				f.targets[i] = target = mapping_hit(mp, f.m.touch_x, f.m.touch_y);
			leave = mapping_act(mp, type, mapping_event(mp, f.events[i], f.targets[i], now_s()));
		}
	}
	g_view.screen = SETUP_CONTROLLERS;
	input_withhold();
	return leave == 2;
}

/* ---- Our Controllers ---- */

static int run_controllers(void)
{
	controllers_ui *c = &g_view.controllers;
	controllers_open(c);
	g_view.screen = SETUP_CONTROLLERS;
	g_view.line[0] = 0;
	frame_input f;
	memset(&f, 0, sizeof(f));
	int touching = 0, target = -1;
	for (;;) {
		for (int p = 0; p < INPUT_PORTS; p++) {
			setup_port *sp = &c->ports[p];
			input_port_info(p, &sp->type, &sp->battery, &sp->charging, &sp->gone);
			c->rumble[p] = g_settings.rumble[p];
		}
		show();
		read_frame(&f, controllers_hit, &touching, &target);
		if (player_closing())
			return 1;
		for (int i = 0; i < f.n; i++) {
			switch (controllers_event(c, f.events[i], f.targets[i])) {
			case CACT_BACK: return 0;
			case CACT_HOME:
				if (home_ok())
					return 1;
				break;
			case CACT_RUMBLE: {
				int p = c->card;
				g_settings.rumble[p] = !g_settings.rumble[p];
				if (setup_save_settings()) {
					g_settings.rumble[p] = !g_settings.rumble[p];
					write_failed();
				} else {
					input_rumble_allow(p, g_settings.rumble[p]); /* the motor only: the Rumble Pak stays (gamedb.h) */
					line(g_settings.rumble[p] ? "Rumble on." : "Rumble off.");
				}
				break;
			}
			case CACT_EDIT:
				if (run_mapping(c->card))
					return 1;
				break;
			case CACT_ORDER:
				if (run_order())
					return 1;
				break;
			}
		}
	}
}

/* ---- Theme: the colors and Our ROMs' layout, each saved and shown at once ---- */

static int run_look(void)
{
	look_ui *l = &g_view.look;
	look_open(l, g_settings.theme, g_settings.layout);
	g_view.screen = SETUP_THEME;
	g_view.line[0] = 0;
	frame_input f;
	memset(&f, 0, sizeof(f));
	int touching = 0, target = -1;
	for (;;) {
		show();
		read_frame(&f, look_hit, &touching, &target);
		if (player_closing())
			return 1;
		for (int i = 0; i < f.n; i++) {
			int act = look_event(l, f.events[i], f.targets[i]);
			switch (act) {
			case LACT_BACK: return 0;
			case LACT_HOME:
				if (home_ok())
					return 1;
				break;
			case LACT_COLORS:
			case LACT_LAYOUT: {
				const our_settings keep = g_settings;
				if (act == LACT_COLORS)
					g_settings.theme = l->pick[LOOK_COLORS];
				else
					g_settings.layout = l->pick[LOOK_LAYOUT];
				if (setup_save_settings()) {
					g_settings = keep;
					l->pick[LOOK_COLORS] = l->theme;
					l->pick[LOOK_LAYOUT] = l->layout;
					write_failed();
				} else {
					look_applied(l);
					theme_use(l->theme); /* the next frame is in it */
					theme_use_layout(l->layout);
					if (act == LACT_COLORS)
						snprintf(g_view.line, sizeof(g_view.line), "Colors: %s", theme_name(l->theme));
					else
						snprintf(g_view.line, sizeof(g_view.line), "Our ROMs: %s", layout_name(l->layout));
				}
				break;
			}
			}
		}
	}
}

/* ---- Our Settings ---- */

static void apply_mode(int mode)
{
	ui_set_mode(mode, g_from_pause ? player_title() : "Our ROMs");
}

void settings_run(int from_pause)
{
	g_from_pause = from_pause;
	snprintf(g_view.game, sizeof(g_view.game), "%s", from_pause ? player_title() : "");
	settings_ui *s = &g_view.settings;
	input_menu m;
	input_menu_state(&m);
	settings_open(s, ui_mode(), g_settings.picture, m.type[0] >= 0);
	/* Picture is applied once, as the settings close: each change rebuilds
	 * the graphics core's frame buffers, a wait the player should not get at
	 * every press while trying both (Zach, 2026-10-27). */
	const int picture_at_open = g_settings.picture;
	g_view.screen = SETUP_SETTINGS;
	g_view.line[0] = 0;
	frame_input f;
	memset(&f, 0, sizeof(f));
	f.m = m;
	int touching = 0, target = -1, home = 0;
	input_take_home();
	while (!home) {
		s->gamepad = f.m.type[0] >= 0;
		show();
		read_frame(&f, settings_hit, &touching, &target);
		if (player_closing())
			break;
		for (int i = 0; i < f.n && !home; i++) {
			switch (settings_event(s, f.events[i], f.targets[i])) {
			case SACT_BACK:
				goto done;
			case SACT_HOME:
				home = home_ok();
				break;
			case SACT_MODE: {
				int keep = g_settings.mode;
				g_settings.mode = s->pick[SET_SCREEN];
				if (setup_save_settings()) {
					g_settings.mode = keep;
					s->pick[SET_SCREEN] = s->mode;
					write_failed();
				} else {
					settings_applied(s);
					apply_mode(s->mode);
					snprintf(g_view.line, sizeof(g_view.line), "Screen mode: %s", mode_name(s->mode));
				}
				break;
			}
			case SACT_PICTURE: {
				int keep = g_settings.picture;
				g_settings.picture = s->pick[SET_PICTURE];
				if (setup_save_settings()) {
					g_settings.picture = keep;
					s->pick[SET_PICTURE] = s->picture;
					write_failed();
				} else {
					settings_applied(s);
					snprintf(g_view.line, sizeof(g_view.line), "Picture: %s%s", picture_name(s->picture),
					         from_pause ? ", on Resume" : "");
				}
				break;
			}
			case SACT_THEME:
				if (run_look())
					home = 1;
				g_view.screen = SETUP_SETTINGS;
				g_view.line[0] = 0;
				break;
			case SACT_CONTROLLERS:
				if (run_controllers())
					home = 1;
				g_view.screen = SETUP_SETTINGS;
				g_view.line[0] = 0;
				break;
			}
		}
	}
done:
	if (g_settings.picture != picture_at_open)
		video_set_picture(g_settings.picture == PICTURE_WIDE);
	input_withhold();
	if (home) /* HOME means Wii U Menu: the caller asks first (a paused game) or leaves */
		g_home_asked = 1;
}
