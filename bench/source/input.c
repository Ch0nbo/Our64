#include "input.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>

typedef struct { uint8_t kind; uint32_t from, to; uint32_t buttons; int8_t x, y; } input_cmd;
enum { CMD_MASH, CMD_PRESS, CMD_STICK };

#define MAX_CMDS 128
static input_cmd g_cmds[MAX_CMDS];
static int g_ncmds;
static uint32_t g_captures[INPUT_MAX_CAPTURES];
static int g_ncaptures;

/* N64 button bits (high half of BUTTONS.Value). */
static const struct { const char *name; uint16_t bit; } k_buttons[] = {
	{"A", 0x8000}, {"B", 0x4000}, {"Z", 0x2000}, {"START", 0x1000},
	{"DU", 0x0800}, {"DD", 0x0400}, {"DL", 0x0200}, {"DR", 0x0100},
	{"L", 0x0020}, {"R", 0x0010}, {"CU", 0x0008}, {"CD", 0x0004}, {"CL", 0x0002}, {"CR", 0x0001},
};

static uint32_t parse_buttons(char *s)
{
	uint32_t b = 0;
	for (char *tok = strtok(s, "+"); tok; tok = strtok(NULL, "+"))
		for (unsigned i = 0; i < sizeof(k_buttons) / sizeof(k_buttons[0]); i++)
			if (!strcasecmp(tok, k_buttons[i].name)) b |= k_buttons[i].bit;
	return b;
}

int input_load(const char *sd_root, const char *rom_path, uint32_t default_capture_vi)
{
	g_ncmds = g_ncaptures = 0;
	char name[128], path[384], line[160];
	const char *base = strrchr(rom_path, '/');
	snprintf(name, sizeof(name), "%s", base ? base + 1 : rom_path);
	char *dot = strrchr(name, '.');
	if (dot) *dot = 0;
	snprintf(path, sizeof(path), "%s/roms/%s.input", sd_root, name);
	FILE *f = fopen(path, "r");
	while (f && fgets(line, sizeof(line), f)) {
		char word[16], arg[64];
		unsigned a = 0, b = 0;
		int x = 0, y = 0;
		if (line[0] == '#' || sscanf(line, "%15s", word) != 1) continue;
		input_cmd c = {0};
		if (!strcasecmp(word, "mash") && sscanf(line, "%*s %u %u", &a, &b) == 2) {
			c.kind = CMD_MASH; c.from = a; c.to = b;
		} else if (!strcasecmp(word, "press") && sscanf(line, "%*s %u %63s %u", &a, arg, &b) >= 2) {
			c.kind = CMD_PRESS; c.from = a; c.to = a + (b ? b : 4); c.buttons = parse_buttons(arg);
		} else if (!strcasecmp(word, "stick") && sscanf(line, "%*s %u %u %d %d", &a, &b, &x, &y) == 4) {
			c.kind = CMD_STICK; c.from = a; c.to = b; c.x = (int8_t)x; c.y = (int8_t)y;
		} else if (!strcasecmp(word, "capture") && sscanf(line, "%*s %u", &a) == 1) {
			if (g_ncaptures < INPUT_MAX_CAPTURES) g_captures[g_ncaptures++] = a;
			continue;
		} else {
			continue;
		}
		if (g_ncmds < MAX_CMDS) g_cmds[g_ncmds++] = c;
	}
	if (f) fclose(f);
	if (g_ncaptures == 0) g_captures[g_ncaptures++] = default_capture_vi;
	return g_ncaptures;
}

uint32_t input_capture_vi(int index)
{
	return index < g_ncaptures ? g_captures[index] : 0;
}

uint32_t input_keys(uint32_t vi)
{
	uint32_t buttons = 0;
	int8_t x = 0, y = 0;
	for (int i = 0; i < g_ncmds; i++) {
		const input_cmd *c = &g_cmds[i];
		if (vi < c->from || vi >= c->to) continue;
		switch (c->kind) {
		case CMD_MASH: {
			const uint32_t t = (vi - c->from) % 60;
			if (t < 4) buttons |= 0x1000;                 /* START */
			else if (t >= 30 && t < 34) buttons |= 0x8000; /* A */
			break;
		}
		case CMD_PRESS: buttons |= c->buttons; break;
		case CMD_STICK: x = c->x; y = c->y; break;
		}
	}
	return buttons << 16 | (uint32_t)(uint8_t)x << 8 | (uint8_t)y;
}
