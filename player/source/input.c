/* Our64 player: controllers (see input.h). */
#include "input.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#define BIT(s) (1u << (s))

static const char *const g_src_name[SRC_COUNT] = {
	"A", "B", "X", "Y", "L", "R", "ZL", "ZR", "+", "-",
	"Up", "Down", "Left", "Right", "LStick-Press", "RStick-Press",
	"1", "2", "C", "Z",
	"LStick-Up", "LStick-Down", "LStick-Left", "LStick-Right",
	"RStick-Up", "RStick-Down", "RStick-Left", "RStick-Right",
};

static const char *const g_n64_name[N64_INPUTS] = {
	"A", "B", "Z", "Start", "D-Up", "D-Down", "D-Left", "D-Right", "L", "R",
	"C-Up", "C-Down", "C-Left", "C-Right",
	"Stick-Up", "Stick-Down", "Stick-Left", "Stick-Right",
};

/* Section names in the file, and names for people. */
static const char *const g_type_key[INPUT_TYPES] = { "gamepad", "pro", "classic", "nunchuk", "wiimote" };
static const char *const g_type_name[INPUT_TYPES] = {
	"GamePad", "Pro Controller", "Classic Controller", "Wii Remote + Nunchuk", "Wii Remote",
};

/* N64 button bits in BUTTONS.Value >> 16, by N64_ input (sticks have none). */
static const uint16_t g_n64_bit[N64_INPUTS] = {
	0x8000, 0x4000, 0x2000, 0x1000, 0x0800, 0x0400, 0x0200, 0x0100, 0x0020, 0x0010,
	0x0008, 0x0004, 0x0002, 0x0001,
};

const char *input_type_name(int type)
{
	return type >= 0 && type < INPUT_TYPES ? g_type_name[type] : NULL;
}

/* Modelled on the Wii U Virtual Console's N64 layout: the right stick and
 * X / Y are the C buttons, ZL and ZR both Z. */
void input_default_map(int type, input_map *m)
{
	memset(m, 0, sizeof(*m));
	m->dead_zone = 0.10f;
	m->range = 80; /* about what a real N64 stick reaches */
	uint32_t *s = m->sources;
	switch (type) {
	case INPUT_GAMEPAD:
	case INPUT_PRO:
	case INPUT_CLASSIC:
	default:
		s[N64_A] = BIT(SRC_A);
		s[N64_B] = BIT(SRC_B);
		s[N64_Z] = BIT(SRC_ZL) | BIT(SRC_ZR);
		s[N64_START] = BIT(SRC_PLUS);
		s[N64_DUP] = BIT(SRC_UP);
		s[N64_DDOWN] = BIT(SRC_DOWN);
		s[N64_DLEFT] = BIT(SRC_LEFT);
		s[N64_DRIGHT] = BIT(SRC_RIGHT);
		s[N64_L] = BIT(SRC_L);
		s[N64_R] = BIT(SRC_R);
		s[N64_CUP] = BIT(SRC_RSTICK_UP) | BIT(SRC_X);
		s[N64_CDOWN] = BIT(SRC_RSTICK_DOWN);
		s[N64_CLEFT] = BIT(SRC_RSTICK_LEFT) | BIT(SRC_Y);
		s[N64_CRIGHT] = BIT(SRC_RSTICK_RIGHT);
		s[N64_STICK_UP] = BIT(SRC_LSTICK_UP);
		s[N64_STICK_DOWN] = BIT(SRC_LSTICK_DOWN);
		s[N64_STICK_LEFT] = BIT(SRC_LSTICK_LEFT);
		s[N64_STICK_RIGHT] = BIT(SRC_LSTICK_RIGHT);
		break;
	case INPUT_NUNCHUK: /* the Nunchuk's stick and Z, the C buttons on the D-pad */
		s[N64_A] = BIT(SRC_A);
		s[N64_B] = BIT(SRC_B);
		s[N64_Z] = BIT(SRC_Z);
		s[N64_START] = BIT(SRC_PLUS);
		s[N64_L] = BIT(SRC_MINUS);
		s[N64_R] = BIT(SRC_C);
		s[N64_CUP] = BIT(SRC_UP);
		s[N64_CDOWN] = BIT(SRC_DOWN);
		s[N64_CLEFT] = BIT(SRC_LEFT);
		s[N64_CRIGHT] = BIT(SRC_RIGHT);
		s[N64_DUP] = BIT(SRC_1);
		s[N64_DDOWN] = BIT(SRC_2);
		s[N64_STICK_UP] = BIT(SRC_LSTICK_UP);
		s[N64_STICK_DOWN] = BIT(SRC_LSTICK_DOWN);
		s[N64_STICK_LEFT] = BIT(SRC_LSTICK_LEFT);
		s[N64_STICK_RIGHT] = BIT(SRC_LSTICK_RIGHT);
		break;
	case INPUT_WIIMOTE: /* held sideways: the D-pad is the stick, 2 and 1 are A and B */
		s[N64_A] = BIT(SRC_2);
		s[N64_B] = BIT(SRC_1);
		s[N64_Z] = BIT(SRC_B);
		s[N64_START] = BIT(SRC_PLUS);
		s[N64_L] = BIT(SRC_MINUS);
		s[N64_R] = BIT(SRC_A);
		s[N64_STICK_UP] = BIT(SRC_UP);
		s[N64_STICK_DOWN] = BIT(SRC_DOWN);
		s[N64_STICK_LEFT] = BIT(SRC_LEFT);
		s[N64_STICK_RIGHT] = BIT(SRC_RIGHT);
		break;
	}
}

/* One stick through the dead zone: the rest of its reach scaled to 0..1, so
 * leaving the dead zone starts small instead of jumping. */
static void stick(float x, float y, float dead, float *out_x, float *out_y)
{
	float mag = sqrtf(x * x + y * y);
	if (mag <= dead || dead >= 1.0f) {
		*out_x = *out_y = 0;
		return;
	}
	float scaled = (mag - dead) / (1.0f - dead);
	if (scaled > 1.0f)
		scaled = 1.0f;
	*out_x = x / mag * scaled;
	*out_y = y / mag * scaled;
}

static float pos(float v) { return v > 0 ? v : 0; }

uint32_t input_eval(const input_map *m, const input_raw *r)
{
	float value[SRC_COUNT];
	for (int s = 0; s < SRC_COUNT; s++)
		value[s] = (r->buttons & BIT(s)) ? 1.0f : 0.0f;
	float lx, ly, rx, ry;
	stick(r->lx, r->ly, m->dead_zone, &lx, &ly);
	stick(r->rx, r->ry, m->dead_zone, &rx, &ry);
	value[SRC_LSTICK_UP] = pos(ly);
	value[SRC_LSTICK_DOWN] = pos(-ly);
	value[SRC_LSTICK_LEFT] = pos(-lx);
	value[SRC_LSTICK_RIGHT] = pos(lx);
	value[SRC_RSTICK_UP] = pos(ry);
	value[SRC_RSTICK_DOWN] = pos(-ry);
	value[SRC_RSTICK_LEFT] = pos(-rx);
	value[SRC_RSTICK_RIGHT] = pos(rx);

	float input[N64_INPUTS];
	for (int i = 0; i < N64_INPUTS; i++) {
		float v = 0;
		for (int s = 0; s < SRC_COUNT; s++)
			if ((m->sources[i] & BIT(s)) && value[s] > v)
				v = value[s];
		input[i] = v;
	}
	uint32_t buttons = 0;
	for (int i = 0; i < N64_STICK_UP; i++)
		if (input[i] > 0.5f)
			buttons |= g_n64_bit[i];
	int range = m->range < 0 ? 0 : m->range > 127 ? 127 : m->range;
	int x = (int)lroundf((input[N64_STICK_RIGHT] - input[N64_STICK_LEFT]) * range);
	int y = (int)lroundf((input[N64_STICK_UP] - input[N64_STICK_DOWN]) * range);
	return buttons << 16 | (uint32_t)(uint8_t)(int8_t)x << 8 | (uint8_t)(int8_t)y;
}

/* ---- the config file ---- */

static char *trim(char *s)
{
	while (isspace((unsigned char)*s))
		s++;
	char *e = s + strlen(s);
	while (e > s && isspace((unsigned char)e[-1]))
		*--e = 0;
	return s;
}

static int lookup(const char *const *names, int count, const char *word)
{
	for (int i = 0; i < count; i++)
		if (!strcasecmp(names[i], word))
			return i;
	return -1;
}

/* "ZL ZR", "l, r" or "none" into SRC_ bits; -1 when a name is unknown. */
static int parse_sources(char *text, uint32_t *out)
{
	uint32_t bits = 0;
	int words = 0;
	for (char *w = strtok(text, " ,\t"); w; w = strtok(NULL, " ,\t")) {
		words++;
		if (!strcasecmp(w, "none"))
			continue;
		int s = lookup(g_src_name, SRC_COUNT, w);
		if (s < 0)
			return -1;
		bits |= BIT(s);
	}
	if (!words)
		return -1;
	*out = bits;
	return 0;
}

int input_config_read(const char *path, input_map maps[INPUT_TYPES])
{
	unsigned types;
	return input_config_read_types(path, maps, &types);
}

int input_config_read_types(const char *path, input_map maps[INPUT_TYPES], unsigned *types)
{
	*types = 0;
	FILE *f = fopen(path, "r");
	if (!f)
		return -1;
	char line[256];
	int type = -1, bad = 0;
	while (fgets(line, sizeof(line), f)) {
		char *l = trim(line);
		if (!*l || *l == '#')
			continue;
		if (*l == '[') {
			char *end = strchr(l, ']');
			if (end)
				*end = 0;
			type = lookup(g_type_key, INPUT_TYPES, trim(l + 1));
			if (type < 0 || !end)
				bad++;
			else
				*types |= 1u << type;
			continue;
		}
		char *eq = strchr(l, '=');
		if (type < 0 || !eq) {
			bad++;
			continue;
		}
		*eq = 0;
		char *key = trim(l), *val = trim(eq + 1);
		input_map *m = &maps[type];
		int n = lookup(g_n64_name, N64_INPUTS, key);
		if (n >= 0) {
			uint32_t bits;
			if (parse_sources(val, &bits))
				bad++;
			else
				m->sources[n] = bits;
		} else if (!strcasecmp(key, "Dead zone")) {
			float pct;
			if (sscanf(val, "%f", &pct) == 1 && pct >= 0 && pct < 100)
				m->dead_zone = pct / 100.0f;
			else
				bad++;
		} else if (!strcasecmp(key, "Range")) {
			int r;
			if (sscanf(val, "%d", &r) == 1 && r > 0 && r <= 127)
				m->range = r;
			else
				bad++;
		} else {
			bad++;
		}
	}
	fclose(f);
	return bad;
}

int input_config_write(const char *path, const input_map maps[INPUT_TYPES])
{
	return input_config_write_types(path, maps, (1u << INPUT_TYPES) - 1);
}

int input_config_write_types(const char *path, const input_map maps[INPUT_TYPES], unsigned types)
{
	FILE *f = fopen(path, "w");
	if (!f)
		return -1;
	fprintf(f, "# Our64 controller mapping. Each line: an N64 input = the buttons that press it,\n"
	           "# separated by spaces, or none. A button may press several inputs.\n"
	           "# Buttons: ");
	for (int s = 0; s < SRC_COUNT; s++)
		fprintf(f, "%s%s", g_src_name[s], s + 1 < SRC_COUNT ? " " : "\n");
	for (int t = 0; t < INPUT_TYPES; t++) {
		if (!(types & (1u << t)))
			continue;
		const input_map *m = &maps[t];
		fprintf(f, "\n[%s]\n# %s\n", g_type_key[t], g_type_name[t]);
		for (int i = 0; i < N64_INPUTS; i++) {
			fprintf(f, "%s =", g_n64_name[i]);
			if (!m->sources[i])
				fprintf(f, " none");
			for (int s = 0; s < SRC_COUNT; s++)
				if (m->sources[i] & BIT(s))
					fprintf(f, " %s", g_src_name[s]);
			fprintf(f, "\n");
		}
		fprintf(f, "Dead zone = %g%%\nRange = %d\n", m->dead_zone * 100.0f, m->range);
	}
	return fclose(f) ? -1 : 0;
}

int input_map_equal(const input_map *a, const input_map *b)
{
	return !memcmp(a->sources, b->sources, sizeof(a->sources)) && fabsf(a->dead_zone - b->dead_zone) < 0.001f &&
	       a->range == b->range;
}

/* ---- names for people ---- */

static const char *const g_src_label[SRC_COUNT] = {
	"A", "B", "X", "Y", "L", "R", "ZL", "ZR", "+", "-",
	"D-pad up", "D-pad down", "D-pad left", "D-pad right", "Left stick press", "Right stick press",
	"1", "2", "C", "Z",
	"Left stick up", "Left stick down", "Left stick left", "Left stick right",
	"Right stick up", "Right stick down", "Right stick left", "Right stick right",
};

static const char *const g_n64_label[N64_INPUTS] = {
	"N64 A", "N64 B", "N64 Z", "N64 Start", "N64 D-Up", "N64 D-Down", "N64 D-Left", "N64 D-Right", "N64 L", "N64 R",
	"N64 C-Up", "N64 C-Down", "N64 C-Left", "N64 C-Right",
	"N64 Stick up", "N64 Stick down", "N64 Stick left", "N64 Stick right",
};

const char *input_n64_label(int n64) { return n64 >= 0 && n64 < N64_INPUTS ? g_n64_label[n64] : ""; }
const char *input_source_label(int src) { return src >= 0 && src < SRC_COUNT ? g_src_label[src] : ""; }

void input_sources_text(char *out, unsigned size, uint32_t sources, int type)
{
	(void)type; /* the names are the same on every controller that has the button */
	int n = 0;
	out[0] = 0;
	for (int s = 0; s < SRC_COUNT; s++)
		if (sources & BIT(s))
			n += snprintf(out + n, n < (int)size ? size - n : 0, "%s%s", n ? ", " : "", g_src_label[s]);
	if (!sources)
		snprintf(out, size, "None");
}

/* ---- ports ---- */

void input_ports_reorder(input_ports *p, const int *ids, int n)
{
	int owner[INPUT_PORTS], k = 0;
	for (int i = 0; i < n && k < INPUT_PORTS; i++)
		owner[k++] = ids[i];
	for (int port = 0; port < INPUT_PORTS && k < INPUT_PORTS; port++) {
		int id = p->owner[port], taken = 0;
		for (int i = 0; i < n; i++)
			taken |= ids[i] == id;
		if (id >= 0 && !taken && p->connected[id]) /* a controller that is gone leaves its port */
			owner[k++] = id;
	}
	while (k < INPUT_PORTS)
		owner[k++] = -1;
	memcpy(p->owner, owner, sizeof(owner));
}

void input_ports_init(input_ports *p)
{
	for (int i = 0; i < INPUT_PORTS; i++)
		p->owner[i] = -1;
	for (int i = 0; i < INPUT_CONTROLLERS; i++)
		p->connected[i] = 1;
}

int input_port_of(const input_ports *p, int id)
{
	for (int i = 0; i < INPUT_PORTS; i++)
		if (p->owner[i] == id)
			return i;
	return -1;
}

int input_ports_waiting(const input_ports *p)
{
	for (int i = 0; i < INPUT_PORTS; i++)
		if (p->owner[i] >= 0 && !p->connected[p->owner[i]])
			return i;
	return -1;
}

void input_ports_connected(input_ports *p, int id, int on)
{
	if (id >= 0 && id < INPUT_CONTROLLERS)
		p->connected[id] = on;
}

void input_ports_set(input_ports *p, const int *ids, int n)
{
	for (int port = 0; port < INPUT_PORTS; port++)
		p->owner[port] = port < n ? ids[port] : -1;
}

void input_ports_drop_gone(input_ports *p)
{
	int owner[INPUT_PORTS], k = 0;
	for (int port = 0; port < INPUT_PORTS; port++)
		if (p->owner[port] >= 0 && p->connected[p->owner[port]])
			owner[k++] = p->owner[port];
	while (k < INPUT_PORTS)
		owner[k++] = -1;
	memcpy(p->owner, owner, sizeof(owner));
}

void input_ports_join_all(input_ports *p, const int *on)
{
	for (int id = 0; id < INPUT_CONTROLLERS; id++)
		if (on[id] && input_port_of(p, id) < 0)
			input_ports_press(p, id);
}

int input_ports_release_waiting(input_ports *p)
{
	int freed = 0;
	for (int port = 0; port < INPUT_PORTS; port++)
		if (p->owner[port] >= 0 && !p->connected[p->owner[port]]) {
			p->owner[port] = -1;
			freed++;
		}
	return freed;
}

int input_ports_press(input_ports *p, int id)
{
	if (id < 0 || id >= INPUT_CONTROLLERS)
		return -1;
	p->connected[id] = 1;
	int port = input_port_of(p, id);
	if (port >= 0)
		return port;
	port = input_ports_waiting(p);
	if (port < 0)
		for (int i = 0; i < INPUT_PORTS && port < 0; i++)
			if (p->owner[i] < 0)
				port = i;
	if (port >= 0)
		p->owner[port] = id;
	return port;
}
