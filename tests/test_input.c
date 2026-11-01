/* PC tests for player/source/input.c: mapping, defaults, config files, ports. */
#include "input.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_failed;
#define CHECK(c) do { if (!(c)) { printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); g_failed = 1; } } while (0)

static input_raw pressed(uint32_t buttons)
{
	input_raw r;
	memset(&r, 0, sizeof(r));
	r.buttons = buttons;
	return r;
}

#define BIT(s) (1u << (s))
#define N64_BUTTONS(v) ((v) >> 16)
#define N64_X(v) ((int8_t)((v) >> 8))
#define N64_Y(v) ((int8_t)(v))

static void test_default_gamepad(void)
{
	input_map m;
	input_default_map(INPUT_GAMEPAD, &m);
	CHECK(N64_BUTTONS(input_eval(&m, &(input_raw){ .buttons = BIT(SRC_A) })) == 0x8000);
	CHECK(N64_BUTTONS(input_eval(&m, &(input_raw){ .buttons = BIT(SRC_B) })) == 0x4000);
	CHECK(N64_BUTTONS(input_eval(&m, &(input_raw){ .buttons = BIT(SRC_ZL) })) == 0x2000);
	CHECK(N64_BUTTONS(input_eval(&m, &(input_raw){ .buttons = BIT(SRC_ZR) })) == 0x2000);
	CHECK(N64_BUTTONS(input_eval(&m, &(input_raw){ .buttons = BIT(SRC_PLUS) })) == 0x1000);
	CHECK(N64_BUTTONS(input_eval(&m, &(input_raw){ .buttons = BIT(SRC_X) })) == 0x0008); /* C-Up */
	CHECK(N64_BUTTONS(input_eval(&m, &(input_raw){ .buttons = BIT(SRC_Y) })) == 0x0002); /* C-Left */
	/* Minus is not mapped by default */
	CHECK(input_eval(&m, &(input_raw){ .buttons = BIT(SRC_MINUS) }) == 0);
}

static void test_stick(void)
{
	input_map m;
	input_default_map(INPUT_GAMEPAD, &m);
	input_raw r = pressed(0);
	r.lx = 1.0f; r.ly = 0.0f;
	uint32_t v = input_eval(&m, &r);
	CHECK(N64_X(v) == m.range);
	CHECK(N64_Y(v) == 0);
	r.lx = 0.0f; r.ly = -1.0f;
	v = input_eval(&m, &r);
	CHECK(N64_Y(v) == -m.range);
	/* inside the dead zone: centred */
	r.lx = m.dead_zone * 0.5f; r.ly = 0.0f;
	CHECK(input_eval(&m, &r) == 0);
	/* just past the dead zone: small, not a jump */
	r.lx = m.dead_zone + 0.05f;
	v = input_eval(&m, &r);
	CHECK(N64_X(v) > 0 && N64_X(v) < m.range / 4);
	/* the right stick drives the C buttons past half way */
	r = pressed(0);
	r.ry = 0.9f;
	CHECK(N64_BUTTONS(input_eval(&m, &r)) == 0x0008);
	r.ry = 0.3f;
	CHECK(N64_BUTTONS(input_eval(&m, &r)) == 0);
}

static void test_many_to_many(void)
{
	input_map m;
	input_default_map(INPUT_GAMEPAD, &m);
	/* Z on two buttons, R on none, and A also presses B */
	m.sources[N64_Z] = BIT(SRC_L) | BIT(SRC_R);
	m.sources[N64_R] = 0;
	m.sources[N64_B] |= BIT(SRC_A);
	CHECK(N64_BUTTONS(input_eval(&m, &(input_raw){ .buttons = BIT(SRC_L) })) == (0x2000 | 0x0020)); /* still L too */
	CHECK(N64_BUTTONS(input_eval(&m, &(input_raw){ .buttons = BIT(SRC_R) })) == 0x2000);
	CHECK(N64_BUTTONS(input_eval(&m, &(input_raw){ .buttons = BIT(SRC_A) })) == (0x8000 | 0x4000));
	/* nothing presses R any more */
	for (int s = 0; s < SRC_COUNT; s++)
		CHECK(!(N64_BUTTONS(input_eval(&m, &(input_raw){ .buttons = BIT(s) })) & 0x0010));
	/* a button can drive the stick */
	m.sources[N64_STICK_UP] |= BIT(SRC_X);
	CHECK(N64_Y(input_eval(&m, &(input_raw){ .buttons = BIT(SRC_X) })) == m.range);
}

static void test_every_type_has_defaults(void)
{
	for (int t = 0; t < INPUT_TYPES; t++) {
		input_map m;
		input_default_map(t, &m);
		CHECK(m.sources[N64_A] != 0);
		CHECK(m.sources[N64_START] != 0);
		CHECK(m.sources[N64_STICK_UP] != 0);
		CHECK(input_type_name(t) != NULL);
	}
}

static const char *write_tmp(const char *text)
{
	static char path[64];
	snprintf(path, sizeof(path), "build/test_input.cfg");
	FILE *f = fopen(path, "w");
	fputs(text, f);
	fclose(f);
	return path;
}

static void test_config_round_trip(void)
{
	input_map maps[INPUT_TYPES];
	for (int t = 0; t < INPUT_TYPES; t++)
		input_default_map(t, &maps[t]);
	maps[INPUT_GAMEPAD].sources[N64_Z] = BIT(SRC_L) | BIT(SRC_R);
	maps[INPUT_GAMEPAD].sources[N64_R] = 0;
	maps[INPUT_PRO].dead_zone = 0.25f;
	maps[INPUT_PRO].range = 70;
	const char *path = "build/test_input_rt.cfg";
	CHECK(input_config_write(path, maps) == 0);
	input_map back[INPUT_TYPES];
	for (int t = 0; t < INPUT_TYPES; t++)
		input_default_map(t, &back[t]);
	CHECK(input_config_read(path, back) == 0);
	CHECK(memcmp(back[INPUT_GAMEPAD].sources, maps[INPUT_GAMEPAD].sources, sizeof(maps[0].sources)) == 0);
	CHECK(back[INPUT_GAMEPAD].sources[N64_R] == 0);
	CHECK(fabsf(back[INPUT_PRO].dead_zone - 0.25f) < 0.001f);
	CHECK(back[INPUT_PRO].range == 70);
}

static void test_config_read_is_forgiving(void)
{
	input_map maps[INPUT_TYPES];
	for (int t = 0; t < INPUT_TYPES; t++)
		input_default_map(t, &maps[t]);
	input_map def;
	input_default_map(INPUT_GAMEPAD, &def);
	const char *path = write_tmp(
		"# hand edited\n"
		"[gamepad]\n"
		"  z =  l, r \n"     /* spaces, commas and case do not matter */
		"R = none\n"
		"C-Up = Banana\n"     /* unknown button: line skipped */
		"Turbo = A\n"         /* unknown input: line skipped */
		"[Toaster]\n"         /* unknown section: skipped */
		"A = B\n");
	CHECK(input_config_read(path, maps) > 0); /* > 0: lines it could not use */
	CHECK(maps[INPUT_GAMEPAD].sources[N64_Z] == (BIT(SRC_L) | BIT(SRC_R)));
	CHECK(maps[INPUT_GAMEPAD].sources[N64_R] == 0);
	CHECK(maps[INPUT_GAMEPAD].sources[N64_CUP] == def.sources[N64_CUP]); /* kept */
	CHECK(maps[INPUT_GAMEPAD].sources[N64_A] == def.sources[N64_A]);     /* not the toaster's */
	CHECK(input_config_read("build/no-such-file.cfg", maps) == -1);
}

static void test_ports(void)
{
	input_ports p;
	input_ports_init(&p);
	CHECK(input_port_of(&p, 3) == -1);
	/* first press joins the next free port */
	CHECK(input_ports_press(&p, 3) == 0);
	CHECK(input_ports_press(&p, 0) == 1);
	CHECK(input_ports_press(&p, 3) == 0); /* already has one */
	CHECK(input_port_of(&p, 0) == 1);
	CHECK(input_ports_waiting(&p) == -1);
	/* a disconnect leaves the port waiting for it */
	input_ports_connected(&p, 3, 0);
	CHECK(input_ports_waiting(&p) == 0);
	input_ports_connected(&p, 3, 1);
	CHECK(input_ports_waiting(&p) == -1);
	/* or another controller takes the waiting port */
	input_ports_connected(&p, 3, 0);
	CHECK(input_ports_press(&p, 2) == 0);
	CHECK(input_port_of(&p, 3) == -1);
	CHECK(input_ports_waiting(&p) == -1);
	/* the one that left can join again, in the next free port */
	input_ports_connected(&p, 3, 1);
	CHECK(input_ports_press(&p, 3) == 2);
	/* 4 ports at most */
	CHECK(input_ports_press(&p, 4) == 3);
	CHECK(input_ports_press(&p, 1) == -1);
}

/* A game profile holds only the controller types it was made for (Zach, 2026-10-16). */
static void test_profile_sections(void)
{
	input_map all[INPUT_TYPES], game[INPUT_TYPES];
	for (int t = 0; t < INPUT_TYPES; t++) {
		input_default_map(t, &all[t]);
		game[t] = all[t];
	}
	game[INPUT_PRO].sources[N64_Z] = BIT(SRC_L);
	CHECK(input_config_write_types("build/profile.cfg", game, 1u << INPUT_PRO) == 0);
	input_map read[INPUT_TYPES];
	for (int t = 0; t < INPUT_TYPES; t++)
		read[t] = all[t];
	unsigned types = 0;
	CHECK(input_config_read_types("build/profile.cfg", read, &types) == 0);
	CHECK(types == (1u << INPUT_PRO));
	CHECK(read[INPUT_PRO].sources[N64_Z] == BIT(SRC_L));
	CHECK(read[INPUT_WIIMOTE].sources[N64_Z] == all[INPUT_WIIMOTE].sources[N64_Z]); /* untouched */
	CHECK(input_config_read_types("build/no-such.cfg", read, &types) == -1 && types == 0);
	remove("build/profile.cfg");
}

static void test_reorder(void)
{
	input_ports p;
	input_ports_init(&p);
	input_ports_press(&p, 0); /* GamePad: player 1 */
	input_ports_press(&p, 2); /* player 2 */
	input_ports_press(&p, 3); /* player 3 */
	int order[] = { 3, 0 };
	input_ports_reorder(&p, order, 2);
	CHECK(p.owner[0] == 3 && p.owner[1] == 0 && p.owner[2] == 2 && p.owner[3] == -1);
	int all[] = { 2, 3, 0 };
	input_ports_reorder(&p, all, 3);
	CHECK(p.owner[0] == 2 && p.owner[1] == 3 && p.owner[2] == 0);
	/* Zach, 2026-10-19: a controller that is gone leaves its port in Change
	 * order, or the game waits for it again */
	input_ports_connected(&p, 2, 0);
	int pad[] = { 0 };
	input_ports_reorder(&p, pad, 1);
	CHECK(p.owner[0] == 0 && p.owner[1] == 3 && p.owner[2] == -1);
	CHECK(input_ports_waiting(&p) == -1);
}

/* Zach, 2026-10-19: player 2's controller went off; A on player 1's plays
 * on, player 1 staying player 1 (it had moved into port 2); the next game
 * waited for a controller that was gone too. */
static void test_gone_controllers(void)
{
	input_ports p;
	input_ports_init(&p);
	input_ports_press(&p, 0); /* GamePad: player 1 */
	input_ports_press(&p, 1); /* Wii Remote: player 2 */
	input_ports_connected(&p, 1, 0);
	CHECK(input_ports_waiting(&p) == 1);
	CHECK(input_ports_release_waiting(&p) == 1);
	CHECK(p.owner[0] == 0 && p.owner[1] == -1 && input_ports_waiting(&p) == -1);
	CHECK(input_ports_release_waiting(&p) == 0); /* nothing waits */
	/* a controller without a port takes the free one */
	CHECK(input_ports_press(&p, 3) == 1);
	/* a game starting drops the ones that are gone and moves the rest up */
	input_ports_init(&p);
	input_ports_press(&p, 1);
	input_ports_press(&p, 2);
	input_ports_press(&p, 0);
	input_ports_connected(&p, 1, 0);
	input_ports_drop_gone(&p);
	CHECK(p.owner[0] == 2 && p.owner[1] == 0 && p.owner[2] == -1 && input_ports_waiting(&p) == -1);
	/* Zach, 2026-10-19: controllers that are on when a game starts are its
	 * players, after the ones already set */
	/* Zach, 2026-10-20: B in Change order keeps only the controllers chosen */
	int only[] = { 3 };
	input_ports_set(&p, only, 1);
	CHECK(p.owner[0] == 3 && p.owner[1] == -1 && p.owner[2] == -1 && p.owner[3] == -1);
	input_ports_set(&p, (int[]){ 2, 0 }, 2);
	int on[INPUT_CONTROLLERS] = { 1, 0, 1, 1, 0 };
	input_ports_join_all(&p, on);
	CHECK(p.owner[0] == 2 && p.owner[1] == 0 && p.owner[2] == 3 && p.owner[3] == -1);
}

static void test_names_for_people(void)
{
	char s[96];
	input_sources_text(s, sizeof(s), BIT(SRC_ZL) | BIT(SRC_ZR) | BIT(SRC_L), INPUT_PRO);
	CHECK(!strcmp(s, "L, ZL, ZR"));
	input_sources_text(s, sizeof(s), 0, INPUT_PRO);
	CHECK(!strcmp(s, "None"));
	input_sources_text(s, sizeof(s), BIT(SRC_RSTICK_UP), INPUT_PRO);
	CHECK(!strcmp(s, "Right stick up"));
	CHECK(!strcmp(input_n64_label(N64_CUP), "N64 C-Up"));
	CHECK(!strcmp(input_n64_label(N64_STICK_LEFT), "N64 Stick left"));
	input_map a, b;
	input_default_map(INPUT_PRO, &a);
	b = a;
	CHECK(input_map_equal(&a, &b));
	b.dead_zone += 0.02f;
	CHECK(!input_map_equal(&a, &b));
}

int main(void)
{
	test_default_gamepad();
	test_stick();
	test_many_to_many();
	test_every_type_has_defaults();
	test_config_round_trip();
	test_config_read_is_forgiving();
	test_ports();
	test_profile_sections();
	test_reorder();
	test_gone_controllers();
	test_names_for_people();
	if (g_failed)
		return 1;
	printf("  all passed\n");
	return 0;
}
