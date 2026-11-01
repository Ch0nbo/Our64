/* Our64 player: controllers. Plain C, so tests/ runs it on the PC; reading
 * the Wii U's controllers is input_wiiu.c's business.
 *
 * Every controller is read into the same set of sources (its buttons, and
 * each stick direction as 0..1). A mapping says, for each N64 input, which
 * sources press it: any number, none included. A source presses one input
 * (Zach, 2026-10-17): the defaults keep to that, and Edit buttons moves a
 * button it captures off any other input. */
#ifndef OUR64_INPUT_H
#define OUR64_INPUT_H
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
	SRC_A, SRC_B, SRC_X, SRC_Y, SRC_L, SRC_R, SRC_ZL, SRC_ZR, SRC_PLUS, SRC_MINUS,
	SRC_UP, SRC_DOWN, SRC_LEFT, SRC_RIGHT, SRC_LSTICK_PRESS, SRC_RSTICK_PRESS,
	SRC_1, SRC_2, SRC_C, SRC_Z, /* Wii Remote; Nunchuk */
	SRC_LSTICK_UP, SRC_LSTICK_DOWN, SRC_LSTICK_LEFT, SRC_LSTICK_RIGHT,
	SRC_RSTICK_UP, SRC_RSTICK_DOWN, SRC_RSTICK_LEFT, SRC_RSTICK_RIGHT,
	SRC_COUNT
};

enum {
	N64_A, N64_B, N64_Z, N64_START, N64_DUP, N64_DDOWN, N64_DLEFT, N64_DRIGHT, N64_L, N64_R,
	N64_CUP, N64_CDOWN, N64_CLEFT, N64_CRIGHT,
	N64_STICK_UP, N64_STICK_DOWN, N64_STICK_LEFT, N64_STICK_RIGHT,
	N64_INPUTS
};

enum { INPUT_GAMEPAD, INPUT_PRO, INPUT_CLASSIC, INPUT_NUNCHUK, INPUT_WIIMOTE, INPUT_TYPES };

typedef struct {
	uint32_t sources[N64_INPUTS]; /* bit per SRC_ */
	float dead_zone;              /* of each stick, 0..1 of its reach */
	int range;                    /* N64 stick value at full reach */
} input_map;

/* One read of a controller: buttons as SRC_ bits (the Wii Remote's D-pad
 * already turned for how it is held), sticks -1..1 with up positive. */
typedef struct {
	uint32_t buttons;
	float lx, ly, rx, ry;
} input_raw;

void input_default_map(int type, input_map *m);
const char *input_type_name(int type);
/* The N64 controller state, as Not64's BUTTONS.Value: buttons << 16, stick x << 8, stick y. */
uint32_t input_eval(const input_map *m, const input_raw *r);

/* config/controllers.cfg and per-game profiles: plain text, one [section]
 * per controller type, one "N64 input = buttons" line per input. read keeps
 * what the file does not set; it returns the number of lines it could not
 * use (0 when all were good), or -1 when there is no file. */
int input_config_read(const char *path, input_map maps[INPUT_TYPES]);
int input_config_write(const char *path, const input_map maps[INPUT_TYPES]);
/* The same, saying which types' sections the file has (*types: bit per
 * INPUT_ type), and writing only the types asked for: a game profile holds
 * only the types it was made for. */
int input_config_read_types(const char *path, input_map maps[INPUT_TYPES], unsigned *types);
int input_config_write_types(const char *path, const input_map maps[INPUT_TYPES], unsigned types);
int input_map_equal(const input_map *a, const input_map *b);

/* For people: "N64 C-Up"; "L, ZL, ZR" or "None"; "Right stick up". */
const char *input_n64_label(int n64);
const char *input_source_label(int src);
void input_sources_text(char *out, unsigned size, uint32_t sources, int type);

/* N64 ports. Controller ids: 0 the GamePad, 1-4 the Wii Remote channels. A
 * controller's first press puts it in a port: one waiting for its lost
 * controller first, else the next free one. */
#define INPUT_CONTROLLERS 5
#define INPUT_PORTS 4
typedef struct {
	int owner[INPUT_PORTS];               /* controller id, -1 when free */
	int connected[INPUT_CONTROLLERS];
} input_ports;
void input_ports_init(input_ports *p);
int  input_ports_press(input_ports *p, int id);   /* its port, or -1 when all are taken */
void input_ports_connected(input_ports *p, int id, int on);
int  input_port_of(const input_ports *p, int id); /* -1 when none */
int  input_ports_waiting(const input_ports *p);   /* first port whose controller is gone, or -1 */
/* Change order: the controllers in ids become players 1, 2, ... in turn;
 * the rest still connected keep their old order after them, and the ones
 * that are gone leave their ports. */
void input_ports_reorder(input_ports *p, const int *ids, int n);
/* Change order ended with B: the controllers in ids become players 1, 2, ...
 * and every other controller leaves its port (Zach, 2026-10-20). */
void input_ports_set(input_ports *p, const int *ids, int n);
/* A game starting: controllers that are gone leave their ports, and the
 * rest move up in order, so no game starts waiting for one. */
void input_ports_drop_gone(input_ports *p);
/* Each controller on (on[id]) without a port takes the next free one, in id order. */
void input_ports_join_all(input_ports *p, const int *on);
/* A port waits for its lost controller and a player goes on without it
 * (Zach, 2026-10-19): every port whose controller is gone is free again.
 * How many were freed. */
int  input_ports_release_waiting(input_ports *p);

#ifdef __cplusplus
}
#endif
#endif
