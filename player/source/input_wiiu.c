/* Our64 player: the Wii U's controllers into input.c. Once per VI, on the
 * CPU core, the GamePad (VPAD) and the four Wii Remote channels (KPAD: Wii
 * Remote, + Nunchuk, Classic Controller, Pro Controller; Bloopair and HID
 * plugins show their pads as these) are read into input_raw, put in N64
 * ports, and turned into each port's N64 controller state.
 *
 * Mappings: input.c's defaults, then sd:/wiiu/our64/config/controllers.cfg
 * (written with the defaults when missing, for editing), then the game's
 * profile config/profiles/<game id>.cfg when there is one.
 *
 * Ports: none while the screen mode prompt runs; the controller that answers
 * it is Player 1 (input_claim), and others join on their first press. Each
 * join, disconnect and return is a notice at the top of the game (ui.h). */
#include "input.h"
#include "player.h"
#include "report.h"
#include "ui.h"

#include <padscore/kpad.h>
#include <padscore/wpad.h>
#include <vpad/input.h>

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#define BIT(s) (1u << (s))

static input_map g_maps[INPUT_TYPES];     /* in use: the game's profile where it has one, else all games */
static input_map g_all[INPUT_TYPES];      /* config/controllers.cfg */
static input_map g_profile[INPUT_TYPES];  /* config/profiles/<game id>.cfg */
static unsigned g_profile_types;          /* the types the game's profile has */
static char g_cfg_path[384], g_profile_path[384];
static input_ports g_ports;
static uint32_t g_keys[INPUT_PORTS];
static uint32_t g_last[INPUT_CONTROLLERS]; /* buttons at the last read, for presses */
static int g_type[INPUT_CONTROLLERS];      /* INPUT_ type, -1 when not connected */
static int g_last_type[INPUT_CONTROLLERS]; /* the type it had before it disconnected */
static int g_home, g_capture, g_flipq_press;
static int g_home_held[INPUT_CONTROLLERS]; /* HOME was down at the last read */
static int g_joining;                      /* presses put controllers in ports */
static uint32_t g_withheld[INPUT_CONTROLLERS]; /* buttons kept from the game until released */
static uint32_t g_menu_last[INPUT_CONTROLLERS]; /* menu bits at the last read */
static uint32_t g_menu_pressed[INPUT_CONTROLLERS];
static uint32_t g_src_last[INPUT_CONTROLLERS], g_src_pressed[INPUT_CONTROLLERS]; /* every source, both sticks */
static int g_last_id = -1;                 /* the controller pressed last */
static int g_touch, g_touch_x, g_touch_y;  /* GamePad touch on the UI canvas */
static int g_battery_told[INPUT_CONTROLLERS];
static int g_vpad_battery;
static void motors(void);

static const struct { uint32_t vpad; int src; } g_vpad_bits[] = {
	{ VPAD_BUTTON_A, SRC_A }, { VPAD_BUTTON_B, SRC_B }, { VPAD_BUTTON_X, SRC_X }, { VPAD_BUTTON_Y, SRC_Y },
	{ VPAD_BUTTON_L, SRC_L }, { VPAD_BUTTON_R, SRC_R }, { VPAD_BUTTON_ZL, SRC_ZL }, { VPAD_BUTTON_ZR, SRC_ZR },
	{ VPAD_BUTTON_PLUS, SRC_PLUS }, { VPAD_BUTTON_MINUS, SRC_MINUS },
	{ VPAD_BUTTON_UP, SRC_UP }, { VPAD_BUTTON_DOWN, SRC_DOWN }, { VPAD_BUTTON_LEFT, SRC_LEFT },
	{ VPAD_BUTTON_RIGHT, SRC_RIGHT }, { VPAD_BUTTON_STICK_L, SRC_LSTICK_PRESS },
	{ VPAD_BUTTON_STICK_R, SRC_RSTICK_PRESS },
};

/* Classic and Pro share their bit layout. */
static const struct { uint32_t bit; int src; } g_classic_bits[] = {
	{ WPAD_PRO_BUTTON_A, SRC_A }, { WPAD_PRO_BUTTON_B, SRC_B }, { WPAD_PRO_BUTTON_X, SRC_X },
	{ WPAD_PRO_BUTTON_Y, SRC_Y }, { WPAD_PRO_BUTTON_L, SRC_L }, { WPAD_PRO_BUTTON_R, SRC_R },
	{ WPAD_PRO_BUTTON_ZL, SRC_ZL }, { WPAD_PRO_BUTTON_ZR, SRC_ZR }, { WPAD_PRO_BUTTON_PLUS, SRC_PLUS },
	{ WPAD_PRO_BUTTON_MINUS, SRC_MINUS }, { WPAD_PRO_BUTTON_UP, SRC_UP }, { WPAD_PRO_BUTTON_DOWN, SRC_DOWN },
	{ WPAD_PRO_BUTTON_LEFT, SRC_LEFT }, { WPAD_PRO_BUTTON_RIGHT, SRC_RIGHT },
	{ WPAD_PRO_BUTTON_STICK_L, SRC_LSTICK_PRESS }, { WPAD_PRO_BUTTON_STICK_R, SRC_RSTICK_PRESS },
};

void input_init(void)
{
	for (int t = 0; t < INPUT_TYPES; t++)
		input_default_map(t, &g_maps[t]);
	KPADInit();
	WPADEnableURCC(TRUE);
	WPADEnableWiiRemote(TRUE);
	input_ports_init(&g_ports);
	for (int i = 0; i < INPUT_CONTROLLERS; i++) {
		g_type[i] = -1;
		g_last[i] = g_menu_last[i] = g_src_last[i] = 0xFFFFFFFF; /* buttons held at start are not presses */
		g_home_held[i] = 1;
	}
}

static void use_maps(void)
{
	for (int t = 0; t < INPUT_TYPES; t++)
		g_maps[t] = g_profile_types & (1u << t) ? g_profile[t] : g_all[t];
}

/* game_id NULL: Our ROMs, no game (all-games buttons only). */
int input_start(const char *root, const char *game_id)
{
	for (int t = 0; t < INPUT_TYPES; t++)
		input_default_map(t, &g_all[t]);
	char dir[256];
	snprintf(dir, sizeof(dir), "%s/config", root);
	mkdir(dir, 0777);
	snprintf(g_cfg_path, sizeof(g_cfg_path), "%s/controllers.cfg", dir);
	int bad = input_config_read(g_cfg_path, g_all);
	if (bad < 0) {
		if (input_config_write(g_cfg_path, g_all))
			report_line("INPUT: could not write the default mapping to %s", g_cfg_path);
	} else if (bad) {
		report_line("INPUT: %d lines of %s not understood and skipped", bad, g_cfg_path);
	}
	g_profile_types = 0;
	g_profile_path[0] = 0;
	if (game_id) {
		snprintf(g_profile_path, sizeof(g_profile_path), "%s/profiles", dir);
		mkdir(g_profile_path, 0777);
		snprintf(g_profile_path, sizeof(g_profile_path), "%s/profiles/%s.cfg", dir, game_id);
		for (int t = 0; t < INPUT_TYPES; t++)
			g_profile[t] = g_all[t];
		bad = input_config_read_types(g_profile_path, g_profile, &g_profile_types);
		if (bad >= 0)
			report_line("INPUT: this game's profile has %u controller types%s", (unsigned)__builtin_popcount(g_profile_types),
			            bad ? " (some lines skipped)" : "");
	}
	use_maps();
	return 0;
}

void input_map_get(int type, input_map *m, int *profile)
{
	*profile = (g_profile_types >> type) & 1;
	*m = *profile ? g_profile[type] : g_all[type];
}

void input_map_all(int type, input_map *m) { *m = g_all[type]; }

int input_map_save(int type, const input_map *m, int profile)
{
	int r;
	if (profile) {
		if (!g_profile_path[0])
			return -1;
		input_map keep = g_profile[type];
		unsigned keep_types = g_profile_types;
		g_profile[type] = *m;
		g_profile_types |= 1u << type;
		r = input_config_write_types(g_profile_path, g_profile, g_profile_types);
		if (r) {
			g_profile[type] = keep;
			g_profile_types = keep_types;
		}
	} else {
		input_map keep = g_all[type];
		g_all[type] = *m;
		r = input_config_write(g_cfg_path, g_all);
		if (r)
			g_all[type] = keep;
	}
	report_line("INPUT: %s buttons for the %s %s", r ? "could not save" : "saved", input_type_name(type),
	            profile ? "in this game's profile" : "for all games");
	use_maps();
	return r;
}

int input_profile_remove(int type)
{
	if (!(g_profile_types & (1u << type)))
		return 0;
	unsigned left = g_profile_types & ~(1u << type);
	int r = left ? input_config_write_types(g_profile_path, g_profile, left) : (remove(g_profile_path) ? -1 : 0);
	if (!r)
		g_profile_types = left;
	report_line("INPUT: %s profile for the %s %s", r ? "could not remove the" : "removed the", input_type_name(type), "");
	use_maps();
	return r;
}

/* ---- for Our Controllers ---- */

void input_port_info(int port, int *type, int *battery, int *charging, int *gone)
{
	int id = g_ports.owner[port];
	*type = id >= 0 ? g_type[id] : -1;
	*gone = id >= 0 && g_type[id] < 0;
	if (*gone)
		*type = g_last_type[id];
	*charging = 0;
	*battery = -1;
	if (id == 0 && g_type[0] >= 0) {
		*charging = g_vpad_battery == 0;
		*battery = g_vpad_battery <= 0 ? -1 : (g_vpad_battery * 4 + 5) / 6; /* 1-6 to 1-4 bars */
	} else if (id > 0 && g_type[id] >= 0) {
		int level = WPADGetBatteryLevel((WPADChan)(id - 1)); /* 0-4 */
		*battery = level < 0 ? -1 : level > 4 ? 4 : level;
	}
}

int input_port_owner(int port) { return port >= 0 && port < INPUT_PORTS ? g_ports.owner[port] : -1; }
int input_controller_type(int id) { return id >= 0 && id < INPUT_CONTROLLERS ? g_type[id] : -1; }

int input_connected_count(void)
{
	int n = 0;
	for (int id = 0; id < INPUT_CONTROLLERS; id++)
		n += g_type[id] >= 0;
	return n;
}

static void ports_changed(void);

void input_reorder(const int *ids, int n)
{
	input_ports_reorder(&g_ports, ids, n);
	ports_changed();
}

/* Left out by Change order's B: not made a player as a game starts, until
 * a press on it joins it again. */
static int g_benched[INPUT_CONTROLLERS];

void input_set_players(const int *ids, int n)
{
	input_ports_set(&g_ports, ids, n);
	for (int id = 0; id < INPUT_CONTROLLERS; id++)
		g_benched[id] = input_port_of(&g_ports, id) < 0;
	ports_changed();
}

static void ports_changed(void)
{
	for (int port = 0; port < INPUT_PORTS; port++) {
		player_port_present(port, g_ports.owner[port] >= 0);
		int id = g_ports.owner[port];
		if (id >= 0)
			g_withheld[id] = g_last[id]; /* the press that claimed a number does not reach the game */
	}
	report_line("INPUT: new order: %d %d %d %d", g_ports.owner[0], g_ports.owner[1], g_ports.owner[2], g_ports.owner[3]);
}

static int read_gamepad(input_raw *r, uint32_t *home)
{
	VPADStatus s;
	VPADReadError e;
	VPADRead(VPAD_CHAN_0, &s, 1, &e);
	if (e == VPAD_READ_INVALID_CONTROLLER)
		return -1;
	if (e != VPAD_READ_SUCCESS)
		return 1; /* no new sample: keep the last state */
	memset(r, 0, sizeof(*r));
	for (unsigned i = 0; i < sizeof(g_vpad_bits) / sizeof(g_vpad_bits[0]); i++)
		if (s.hold & g_vpad_bits[i].vpad)
			r->buttons |= BIT(g_vpad_bits[i].src);
	r->lx = s.leftStick.x;
	r->ly = s.leftStick.y;
	r->rx = s.rightStick.x;
	r->ry = s.rightStick.y;
	*home = s.hold & VPAD_BUTTON_HOME;
	g_vpad_battery = s.battery;
	VPADTouchData t;
	VPADGetTPCalibratedPoint(VPAD_CHAN_0, &t, &s.tpNormal);
	g_touch = t.touched && t.validity == VPAD_VALID;
	if (g_touch) { /* calibrated points are 1280x720 */
		g_touch_x = t.x * 854 / 1280;
		g_touch_y = t.y * 480 / 720;
	}
	return 0;
}

static int g_ext[INPUT_CONTROLLERS]; /* KPAD's extension type, for the log */

static int read_kpad(int chan, input_raw *r, uint32_t *home, int *type)
{
	KPADStatus s;
	KPADError e;
	if (KPADReadEx((KPADChan)chan, &s, 1, &e) == 0 || e != KPAD_ERROR_OK)
		return e == KPAD_ERROR_NO_SAMPLES ? 1 : -1;
	g_ext[chan + 1] = s.extensionType;
	memset(r, 0, sizeof(*r));
	switch (s.extensionType) {
	case WPAD_EXT_CLASSIC:
	case WPAD_EXT_MPLUS_CLASSIC:
	case WPAD_EXT_PRO_CONTROLLER: {
		int pro = s.extensionType == WPAD_EXT_PRO_CONTROLLER;
		uint32_t h = pro ? s.pro.hold : s.classic.hold;
		KPADVec2D ls = pro ? s.pro.leftStick : s.classic.leftStick;
		KPADVec2D rs = pro ? s.pro.rightStick : s.classic.rightStick;
		for (unsigned i = 0; i < sizeof(g_classic_bits) / sizeof(g_classic_bits[0]); i++)
			if (h & g_classic_bits[i].bit)
				r->buttons |= BIT(g_classic_bits[i].src);
		r->lx = ls.x; r->ly = ls.y; r->rx = rs.x; r->ry = rs.y;
		/* The first 60 changes of a Classic Controller's buttons each session go to the log
		 * with the analog triggers: what an adapter (Zach's raphnet N64 pad) sends for each button. */
		static uint32_t last_hold[INPUT_CONTROLLERS];
		static int logged;
		if (!pro && h != last_hold[chan + 1] && logged < 60) {
			logged++;
			report_line("INPUT: Classic Controller %d: buttons 0x%05X, triggers L %.2f R %.2f", chan + 1, (unsigned)h,
			            s.classic.leftTrigger, s.classic.rightTrigger);
		}
		last_hold[chan + 1] = h;
		*home = (h & WPAD_PRO_BUTTON_HOME) || (s.hold & WPAD_BUTTON_HOME);
		*type = pro ? INPUT_PRO : INPUT_CLASSIC;
		return 0;
	}
	case WPAD_EXT_NUNCHUK:
	case WPAD_EXT_MPLUS_NUNCHUK:
		if (s.nunchuk.hold & WPAD_NUNCHUK_BUTTON_Z) r->buttons |= BIT(SRC_Z);
		if (s.nunchuk.hold & WPAD_NUNCHUK_BUTTON_C) r->buttons |= BIT(SRC_C);
		r->lx = s.nunchuk.stick.x;
		r->ly = s.nunchuk.stick.y;
		*type = INPUT_NUNCHUK;
		break;
	default:
		*type = INPUT_WIIMOTE;
		break;
	}
	/* Wii Remote buttons. Alone it is held sideways, so its D-pad turns a quarter. */
	uint32_t h = s.hold;
	int side = *type == INPUT_WIIMOTE;
	static const struct { uint32_t bit; int up, side; } wpad[] = {
		{ WPAD_BUTTON_UP, SRC_UP, SRC_LEFT }, { WPAD_BUTTON_DOWN, SRC_DOWN, SRC_RIGHT },
		{ WPAD_BUTTON_LEFT, SRC_LEFT, SRC_DOWN }, { WPAD_BUTTON_RIGHT, SRC_RIGHT, SRC_UP },
		{ WPAD_BUTTON_A, SRC_A, SRC_A }, { WPAD_BUTTON_B, SRC_B, SRC_B }, { WPAD_BUTTON_1, SRC_1, SRC_1 },
		{ WPAD_BUTTON_2, SRC_2, SRC_2 }, { WPAD_BUTTON_PLUS, SRC_PLUS, SRC_PLUS },
		{ WPAD_BUTTON_MINUS, SRC_MINUS, SRC_MINUS },
	};
	for (unsigned i = 0; i < sizeof(wpad) / sizeof(wpad[0]); i++)
		if (h & wpad[i].bit)
			r->buttons |= BIT(side ? wpad[i].side : wpad[i].up);
	*home = h & WPAD_BUTTON_HOME;
	return 0;
}

/* Player notices only while a game runs (Zach, 2026-10-19: "Player 1:
 * GamePad is back" came up as every game opened, after a blip on the menus). */
static int g_notices;
static int g_gone_told[INPUT_CONTROLLERS]; /* its "disconnected" notice was shown */

void input_set_notices(int on)
{
	g_notices = on;
	if (!on)
		for (int port = 0; port < INPUT_PORTS; port++)
			ui_notice_clear(UI_KEY_PORT(port));
}

static void port_notice(int port, int kind, double secs, const char *line1, const char *line2)
{
	if (g_notices)
		ui_notice(UI_KEY_PORT(port), kind, secs, line1, line2);
}

static void name_of(char *out, unsigned size, int port, int type, const char *tail)
{
	snprintf(out, size, "Player %d: %s%s", port + 1, input_type_name(type), tail);
}

/* What a menu reads: buttons, and the left stick as directions. */
static uint32_t menu_bits(const input_raw *r)
{
	uint32_t b = r->buttons;
	if (r->lx < -0.5f) b |= BIT(SRC_LSTICK_LEFT);
	if (r->lx > 0.5f) b |= BIT(SRC_LSTICK_RIGHT);
	if (r->ly > 0.5f) b |= BIT(SRC_LSTICK_UP);
	if (r->ly < -0.5f) b |= BIT(SRC_LSTICK_DOWN);
	return b;
}

/* Every source as a button: both sticks past half way in each direction. */
static uint32_t all_bits(const input_raw *r)
{
	uint32_t b = menu_bits(r);
	if (r->rx < -0.5f) b |= BIT(SRC_RSTICK_LEFT);
	if (r->rx > 0.5f) b |= BIT(SRC_RSTICK_RIGHT);
	if (r->ry > 0.5f) b |= BIT(SRC_RSTICK_UP);
	if (r->ry < -0.5f) b |= BIT(SRC_RSTICK_DOWN);
	return b;
}

static int battery_low(int id)
{
	if (id == 0)
		return g_vpad_battery == 1; /* 1: critical, 6: full; 0 is charging on some systems */
	return WPADGetBatteryLevel((WPADChan)(id - 1)) <= 1;
}

static void join(int id, int type)
{
	int before = -1;
	int port = input_ports_waiting(&g_ports);
	if (port >= 0)
		before = g_ports.owner[port];
	port = input_ports_press(&g_ports, id);
	if (port < 0)
		return; /* four players already */
	g_benched[id] = 0;
	g_withheld[id] = g_last[id]; /* the joining press is not a game press */
	char text[NOTICE_TEXT];
	name_of(text, sizeof(text), port, type, "");
	report_line("INPUT: %s joined port %d%s", input_type_name(type), port + 1,
	            before >= 0 ? ", taking it from the controller that left" : "");
	port_notice(port, NOTICE_INFO, 3, text, "");
	player_port_present(port, 1);
}

/* A port waits for its lost controller, and A on a controller already
 * playing goes on without it (Zach, 2026-10-19: player 2's Wii Remote went
 * off; player 1 plays on as player 1). The port is free for the next
 * controller to join; the game still sees a controller there, idle. Only a
 * controller without a port takes the waiting one (join). */
static void play_on(int id, int type)
{
	for (int port = 0; port < INPUT_PORTS; port++)
		if (g_ports.owner[port] >= 0 && !g_ports.connected[g_ports.owner[port]])
			ui_notice_clear(UI_KEY_PORT(port));
	int freed = input_ports_release_waiting(&g_ports);
	if (!freed)
		return;
	g_withheld[id] = g_last[id];
	report_line("INPUT: A on the %s (player %d): playing on without the %d controller%s that left",
	            input_type_name(type), input_port_of(&g_ports, id) + 1, freed, freed > 1 ? "s" : "");
}

void input_game_start(void)
{
	input_ports_drop_gone(&g_ports);
	/* Controllers that are on are players, as plugged-in controllers are on an
	 * N64 at power on (Zach, 2026-10-19): games that look for controllers only
	 * as they start see them. Quietly, in controller order after the players
	 * already set. */
	int on[INPUT_CONTROLLERS];
	for (int id = 0; id < INPUT_CONTROLLERS; id++) {
		on[id] = g_type[id] >= 0 && !g_benched[id];
		if (on[id] && input_port_of(&g_ports, id) < 0)
			g_withheld[id] = g_last[id];
	}
	input_ports_join_all(&g_ports, on);
	for (int port = 0; port < INPUT_PORTS; port++)
		player_port_present(port, g_ports.owner[port] >= 0);
	report_line("INPUT: players at the start: %d %d %d %d (0 is the GamePad)", g_ports.owner[0], g_ports.owner[1],
	            g_ports.owner[2], g_ports.owner[3]);
}

void input_read(void)
{
	static input_raw raw[INPUT_CONTROLLERS];
	for (int id = 0; id < INPUT_CONTROLLERS; id++) {
		uint32_t home = 0;
		int type = INPUT_GAMEPAD;
		int r = id == 0 ? read_gamepad(&raw[id], &home) : read_kpad(id - 1, &raw[id], &home, &type);
		g_menu_pressed[id] = g_src_pressed[id] = 0;
		if (r < 0) {
			if (g_type[id] >= 0) {
				int port = input_port_of(&g_ports, id);
				if (port >= 0) {
					char text[NOTICE_TEXT], how[NOTICE_TEXT];
					snprintf(text, sizeof(text), "Player %d's controller disconnected", port + 1);
					snprintf(how, sizeof(how), "Press A to reconnect or continue.");
					report_line("INPUT: port %d's %s disconnected", port + 1, input_type_name(g_type[id]));
					g_gone_told[id] = g_notices;
					port_notice(port, NOTICE_WARN, -1, text, how);
				}
				g_last_type[id] = g_type[id];
				g_type[id] = -1;
				g_battery_told[id] = 0;
				input_ports_connected(&g_ports, id, 0);
			}
			memset(&raw[id], 0, sizeof(raw[id]));
			if (id == 0)
				g_touch = 0;
			continue;
		}
		if (r > 0)
			continue; /* no new sample: the last one stands (and an unknown controller stays unknown) */
		if (g_type[id] != type) {
			int port = input_port_of(&g_ports, id);
			int back = g_type[id] < 0 && port >= 0;
			g_type[id] = type;
			input_ports_connected(&g_ports, id, 1);
			if (port >= 0 && (!back || g_gone_told[id])) { /* "is back" answers a "disconnected" shown */
				char text[NOTICE_TEXT];
				name_of(text, sizeof(text), port, type, back ? " is back" : "");
				report_line("INPUT: port %d is a %s%s (extension %d)", port + 1, input_type_name(type),
				            back ? " again" : "", g_ext[id]);
				port_notice(port, NOTICE_INFO, 3, text, "");
			}
		}
		uint32_t pressed = raw[id].buttons & ~g_last[id];
		g_last[id] = raw[id].buttons;
		uint32_t menu = menu_bits(&raw[id]);
		g_menu_pressed[id] = menu & ~g_menu_last[id];
		g_menu_last[id] = menu;
		uint32_t src = all_bits(&raw[id]);
		g_src_pressed[id] = src & ~g_src_last[id];
		g_src_last[id] = src;
		g_withheld[id] &= raw[id].buttons; /* a withheld button counts again once released */
		if (pressed)
			g_last_id = id;
		if (home && !g_home_held[id])
			g_home = 1; /* a press, not a hold: HOME opens the pause menu and closes it again */
		g_home_held[id] = home != 0;
#ifndef OUR64_RELEASE
		if (id == 0 && (pressed & BIT(SRC_MINUS)) && (raw[0].buttons & BIT(SRC_ZL)) && (raw[0].buttons & BIT(SRC_ZR)))
			g_capture = 1; /* debug frame capture: - with ZL and ZR held */
		if (id == 0 && (pressed & BIT(SRC_MINUS)) && (raw[0].buttons & BIT(SRC_L)) && (raw[0].buttons & BIT(SRC_R)))
			g_flipq_press = 1; /* the GPU flip queue on or off: - with L and R held */
#endif
		if (pressed && g_joining && input_port_of(&g_ports, id) < 0)
			join(id, type);
		else if ((g_menu_pressed[id] & BIT(SRC_A)) && input_ports_waiting(&g_ports) >= 0)
			play_on(id, type);
		int port = input_port_of(&g_ports, id);
		if (port >= 0 && !g_battery_told[id] && battery_low(id)) {
			char text[NOTICE_TEXT];
			snprintf(text, sizeof(text), "Player %d's controller battery is low", port + 1);
			report_line("INPUT: %s", text);
			ui_notice(UI_KEY_BATTERY(id), NOTICE_WARN, 6, text, "");
			g_battery_told[id] = 1;
		}
	}
	motors();
	for (int port = 0; port < INPUT_PORTS; port++) {
		int id = g_ports.owner[port];
		if (id < 0 || g_type[id] < 0) {
			g_keys[port] = 0;
			continue;
		}
		input_raw r = raw[id];
		r.buttons &= ~g_withheld[id];
		g_keys[port] = input_eval(&g_maps[g_type[id]], &r);
	}
}

void input_set_joining(int on) { g_joining = on; }

/* ---- Rumble Pak ---- */

static volatile int g_motor_want[INPUT_PORTS]; /* the game's Rumble Pak motor, per N64 port */
static int g_rumble_allowed[INPUT_PORTS] = { 1, 1, 1, 1 }; /* the player's Rumble setting */
static int g_motor_on[INPUT_CONTROLLERS];
static int g_motors_held;                       /* a menu is up: every motor off */

/* Not64's PIF code, when the game writes the Rumble Pak's motor register. */
void our64_rumble(int port, int on)
{
	if (port >= 0 && port < INPUT_PORTS)
		g_motor_want[port] = on;
}

void input_rumble_allow(int port, int on)
{
	if (port >= 0 && port < INPUT_PORTS)
		g_rumble_allowed[port] = on;
}

void input_rumble_hold(int held) { g_motors_held = held; }

/* A game ended, loaded or reset: no motor the old one turned on keeps running. */
void input_rumble_reset(void)
{
	for (int p = 0; p < INPUT_PORTS; p++)
		g_motor_want[p] = 0;
}

/* Each read: the GamePad's motor plays a short pattern, renewed while on;
 * a Wii Remote or Pro Controller's is switched. */
static void motors(void)
{
	static uint8_t pattern[15] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
	                               0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
	for (int id = 0; id < INPUT_CONTROLLERS; id++) {
		int port = input_port_of(&g_ports, id);
		int want = !g_motors_held && port >= 0 && g_type[id] >= 0 && g_motor_want[port] && g_rumble_allowed[port];
		if (id == 0) {
			if (want)
				VPADControlMotor(VPAD_CHAN_0, pattern, sizeof(pattern));
			else if (g_motor_on[0])
				VPADStopMotor(VPAD_CHAN_0);
		} else if (want != g_motor_on[id]) {
			WPADControlMotor((WPADChan)(id - 1), want);
		}
		g_motor_on[id] = want;
	}
}

void input_withhold(void)
{
	for (int id = 0; id < INPUT_CONTROLLERS; id++)
		g_withheld[id] = g_type[id] >= 0 ? g_last[id] : 0;
	g_home = 0;
}

void input_claim(int id)
{
	if (id < 0 || id >= INPUT_CONTROLLERS || g_type[id] < 0)
		return;
	g_withheld[id] = g_last[id];
	int port = input_ports_press(&g_ports, id);
	if (port >= 0) {
		report_line("INPUT: %s is player %d", input_type_name(g_type[id]), port + 1);
		player_port_present(port, 1);
	}
}

void input_menu_state(input_menu *m)
{
	for (int id = 0; id < INPUT_CONTROLLERS; id++) {
		m->type[id] = g_type[id];
		m->pressed[id] = g_type[id] >= 0 ? g_menu_pressed[id] : 0;
		m->held[id] = g_type[id] >= 0 ? g_menu_last[id] : 0;
		m->sources[id] = g_type[id] >= 0 ? g_src_pressed[id] : 0;
	}
	m->last_id = g_last_id;
	m->touch = g_touch;
	m->touch_x = g_touch_x;
	m->touch_y = g_touch_y;
}

uint32_t input_keys(int port) { return port >= 0 && port < INPUT_PORTS ? g_keys[port] : 0; }
int input_waiting_port(void) { return input_ports_waiting(&g_ports); }

int input_take_home(void)
{
	int h = g_home;
	g_home = 0;
	return h;
}

int input_take_flipq(void)
{
	int c = g_flipq_press;
	g_flipq_press = 0;
	return c;
}

int input_take_capture(void)
{
	int c = g_capture;
	g_capture = 0;
	return c;
}
