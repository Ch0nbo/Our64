/* Our64 player: the Transfer Pak, its Game Boy cartridge, and Goomba Color's
 * saves (tpak.h). */
#include "tpak.h"
#include "minilzo.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- the Game Boy cartridge (mupen64plus's gb_cart.c, without the clock,
 * camera and rumble cartridges) ---- */

void gb_title(const uint8_t *rom, char out[16])
{
	memcpy(out, rom + 0x134, 15);
	out[15] = '\0';
}

uint32_t gb_ram_size(const uint8_t *rom)
{
	static const uint32_t sizes[] = { 0, 0x800, 0x2000, 0x8000, 0x20000, 0x10000 };
	if (rom[0x147] == 0x05 || rom[0x147] == 0x06) /* MBC2: 512 half-bytes inside the mapper */
		return 512;
	return rom[0x149] < 6 ? sizes[rom[0x149]] : 0;
}

int gb_cart_init(gb_cart *c, const uint8_t *rom, uint32_t rom_size, uint8_t *ram, uint32_t ram_size, const char **why)
{
	memset(c, 0, sizeof(*c));
	c->rom = rom, c->rom_size = rom_size;
	c->ram = ram, c->ram_size = ram_size;
	c->rom_bank = 1;
	switch (rom[0x147]) {
	case 0x00: case 0x08: case 0x09: c->mbc = GB_NO_MBC; break;
	case 0x01: case 0x02: case 0x03: c->mbc = GB_MBC1; break;
	case 0x05: case 0x06: c->mbc = GB_MBC2; break;
	case 0x0F: case 0x10: case 0x11: case 0x12: case 0x13: c->mbc = GB_MBC3; break;
	case 0x19: case 0x1A: case 0x1B: case 0x1C: case 0x1D: case 0x1E: c->mbc = GB_MBC5; break;
	default:
		if (why)
			*why = "a Game Boy cartridge type Our64 cannot read yet";
		return -1;
	}
	return 0;
}

static uint8_t rom_at(const gb_cart *c, uint32_t a)
{
	return c->rom_size ? c->rom[a % c->rom_size] : 0xFF;
}

/* The RAM byte at an address in 0xA000-0xBFFF, or NULL (disabled, absent, a clock register). */
static uint8_t *ram_at(gb_cart *c, uint16_t address)
{
	if (!c->ram || !c->ram_size || (c->mbc != GB_NO_MBC && !c->ram_enable))
		return NULL;
	uint32_t a = address - 0xA000;
	switch (c->mbc) {
	case GB_MBC1: a += (c->mbc1_mode ? c->ram_bank & 3 : 0) * 0x2000; break;
	case GB_MBC2: a &= 0x1FF; break;
	case GB_MBC3:
		if (c->ram_bank > 3)
			return NULL; /* the clock: no clock here */
		a += c->ram_bank * 0x2000;
		break;
	case GB_MBC5: a += (c->ram_bank & 0x0F) * 0x2000; break;
	}
	return &c->ram[a % c->ram_size];
}

static uint8_t read_byte(gb_cart *c, uint16_t address)
{
	if (address < 0x4000)
		return rom_at(c, address);
	if (address < 0x8000)
		return rom_at(c, (address - 0x4000) + (c->mbc == GB_NO_MBC ? 1 : c->rom_bank) * 0x4000u);
	if (address >= 0xA000 && address < 0xC000) {
		const uint8_t *p = ram_at(c, address);
		if (!p)
			return c->mbc == GB_MBC3 && c->ram_enable && c->ram_bank > 3 ? 0x00 : 0xFF;
		return c->mbc == GB_MBC2 ? (*p & 0x0F) | 0xF0 : *p;
	}
	return 0xFF;
}

static void write_byte(gb_cart *c, uint16_t address, uint8_t v)
{
	if (address >= 0xA000 && address < 0xC000) {
		uint8_t *p = ram_at(c, address);
		if (p) {
			uint8_t nv = c->mbc == GB_MBC2 ? v & 0x0F : v;
			if (*p != nv)
				c->ram_written = 1;
			*p = nv;
		}
		return;
	}
	if (address >= 0x8000)
		return;
	switch (c->mbc) {
	case GB_MBC1:
		if (address < 0x2000)
			c->ram_enable = (v & 0x0F) == 0x0A;
		else if (address < 0x4000)
			c->rom_bank = (c->rom_bank & ~0x1Fu) | ((v & 0x1F) ? (v & 0x1F) : 1);
		else if (address < 0x6000) {
			if (c->mbc1_mode)
				c->ram_bank = v & 3;
			else
				c->rom_bank = (c->rom_bank & 0x1F) | ((v & 3u) << 5);
		} else {
			c->mbc1_mode = v & 1;
			if (c->mbc1_mode)
				c->rom_bank &= 0x1F;
			else
				c->ram_bank = 0;
		}
		break;
	case GB_MBC2:
		if (address < 0x4000) {
			if (address & 0x100)
				c->rom_bank = (v & 0x0F) ? (v & 0x0F) : 1;
			else
				c->ram_enable = (v & 0x0F) == 0x0A;
		}
		break;
	case GB_MBC3:
		if (address < 0x2000)
			c->ram_enable = (v & 0x0F) == 0x0A;
		else if (address < 0x4000)
			c->rom_bank = (v & 0x7F) ? (v & 0x7F) : 1;
		else if (address < 0x6000)
			c->ram_bank = v;
		break; /* 0x6000: the clock latch */
	case GB_MBC5:
		if (address < 0x2000)
			c->ram_enable = (v & 0x0F) == 0x0A;
		else if (address < 0x3000)
			c->rom_bank = (c->rom_bank & 0x100) | v;
		else if (address < 0x4000)
			c->rom_bank = (c->rom_bank & 0xFF) | ((v & 1u) << 8);
		else if (address < 0x6000)
			c->ram_bank = v & 0x0F;
		break;
	}
}

void gb_cart_read(gb_cart *c, uint16_t address, uint8_t *data, unsigned size)
{
	for (unsigned i = 0; i < size; i++)
		data[i] = read_byte(c, (uint16_t)(address + i));
}

void gb_cart_write(gb_cart *c, uint16_t address, const uint8_t *data, unsigned size)
{
	if (address < 0x8000) { /* a mapper register: the last byte is the one that stays */
		write_byte(c, address, data[size - 1]);
		return;
	}
	for (unsigned i = 0; i < size; i++)
		write_byte(c, (uint16_t)(address + i), data[i]);
}

/* ---- the Transfer Pak (mupen64plus's transferpak.c) ---- */

void tpak_power_on(tpak *t, gb_cart *cart)
{
	t->enabled = 0;
	t->bank = 0;
	t->cart_enabled = 0;
	t->reset_state = 3;
	t->cart = cart;
}

static uint16_t cart_address(const tpak *t, uint16_t address)
{
	return (uint16_t)((address & 0x3FFF) | (t->bank * 0x4000));
}

void tpak_read(tpak *t, uint16_t address, uint8_t data[32])
{
	uint8_t v = 0;
	switch (address >> 12) {
	case 0x8: /* the cartridge's power */
		memset(data, t->enabled ? 0x84 : 0x00, 32);
		break;
	case 0xB: /* status: access mode, reset, power */
		if (t->cart && t->cart_enabled)
			v |= 0x01;
		v |= (uint8_t)((t->reset_state & 3) << 2);
		if (t->enabled)
			v |= 0x80;
		if (t->cart_enabled && t->reset_state == 3)
			t->reset_state = 2;
		else if (!t->cart_enabled && t->reset_state == 2)
			t->reset_state = 1;
		else if (!t->cart_enabled && t->reset_state == 1)
			t->reset_state = 0;
		memset(data, v, 32);
		break;
	case 0xC: case 0xD: case 0xE: case 0xF:
		if (t->enabled && t->cart)
			gb_cart_read(t->cart, cart_address(t, address), data, 32);
		else
			memset(data, 0, 32);
		break;
	default:
		memset(data, 0, 32);
	}
}

void tpak_write(tpak *t, uint16_t address, const uint8_t data[32])
{
	const uint8_t v = data[31];
	switch (address >> 12) {
	case 0x8:
		if (v == 0xFE)
			t->enabled = 0;
		else if (v == 0x84) {
			if (!t->enabled) {
				t->bank = 3;
				t->cart_enabled = 0;
				t->reset_state = 0;
			}
			t->enabled = 1;
		}
		break;
	case 0xA:
		if (t->enabled)
			t->bank = v > 3 ? 0 : v;
		break;
	case 0xB:
		if (t->enabled) {
			t->reset_state = 3;
			t->cart_enabled = 1;
		}
		break;
	case 0xC: case 0xD: case 0xE: case 0xF:
		if (t->enabled && t->cart)
			gb_cart_write(t->cart, cart_address(t, address), data, 32);
		break;
	}
}

/* ---- Goomba Color's SRAM (as libertyernie's goombasav reads it, and as
 * Jagoomba Color's sram.c writes it). Little-endian, from the GBA. ---- */

#define GOOMBA_STATEID  0x57A731D8u
#define GOOMBA_STATEID2 0x57A731D9u
#define SH_SIZE 48 /* stateheader: u16 size, u16 type, u32 uncompressed_size, u32 framecount, u32 checksum, char title[32] */
enum { SH_STATE = 0, SH_SRAM = 1, SH_CONFIG = 2, SH_PALETTE = 5 };

static uint32_t le16(const uint8_t *p) { return p[0] | (p[1] << 8); }
static uint32_t le32(const uint8_t *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }
static void put16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *p, uint32_t v) { put16(p, v); put16(p + 2, v >> 16); }

int goomba_is_sram(const uint8_t *sram)
{
	uint32_t id = le32(sram);
	return id == GOOMBA_STATEID || id == GOOMBA_STATEID2;
}

/* Where the game Goomba ran last keeps its save uncompressed: 0xE000, or
 * 0x6000 when the SRAM is 32 KB (mirrored at 0x8000). The headers end there. */
static uint32_t save_start(const uint8_t *sram)
{
	return memcmp(sram, sram + 0x8000, 0x8000) ? 0xE000 : 0x6000;
}

static int plausible(const uint8_t *sh)
{
	uint32_t type = le16(sh + 2);
	if (type == 3 || type == 4 || type > 5)
		return 0;
	return le16(sh) >= SH_SIZE && (type == SH_CONFIG || le32(sh + 4) != 0);
}

typedef struct {
	uint32_t config;  /* offset of the config header, 0: none */
	uint32_t save;    /* offset of the game's save header, 0: none */
	uint32_t end;     /* offset after the last header */
	uint32_t start;   /* save_start */
} goomba_map;

static void map(const uint8_t *sram, const char *title, goomba_map *m)
{
	memset(m, 0, sizeof(*m));
	m->start = save_start(sram);
	uint32_t at = 4;
	while (at + SH_SIZE <= m->start && plausible(sram + at)) {
		const uint8_t *sh = sram + at;
		char t[16];
		memcpy(t, sh + 16, 15);
		t[15] = '\0';
		if (le16(sh + 2) == SH_CONFIG && !m->config)
			m->config = at;
		else if (le16(sh + 2) == SH_SRAM && !m->save && !strcmp(t, title))
			m->save = at;
		at += le16(sh);
	}
	m->end = at;
}

/* The uncompressed copy at save_start is this game's: Goomba's config names its checksum. */
static int owns_end(const uint8_t *sram, const goomba_map *m)
{
	if (!m->config || le16(sram + m->config) != SH_SIZE)
		return 0;
	uint32_t owner = le32(sram + m->config + 8);
	return owner && owner == le32(sram + m->save + 12);
}

static int lzo_ready(void)
{
	static int ok = -1;
	if (ok < 0)
		ok = lzo_init() == LZO_E_OK;
	return ok;
}

int goomba_read(const uint8_t *sram, const char *title, uint8_t *out, uint32_t size, const char **why)
{
	const char *dummy;
	if (!why)
		why = &dummy;
	if (!goomba_is_sram(sram)) {
		*why = "not Goomba Color's save memory";
		return -1;
	}
	goomba_map m;
	map(sram, title, &m);
	if (!m.save) {
		*why = "the Game Boy game has no save in it yet";
		return -1;
	}
	if (owns_end(sram, &m) && size <= 0x2000) {
		memcpy(out, sram + m.start, size);
		return 0;
	}
	if (!lzo_ready()) {
		*why = "LZO did not start";
		return -1;
	}
	uint8_t *buf = malloc(0x10000);
	if (!buf) {
		*why = "out of memory";
		return -1;
	}
	lzo_uint n = 0x10000;
	int r = lzo1x_decompress_safe(sram + m.save + SH_SIZE, le16(sram + m.save) - SH_SIZE, buf, &n, NULL);
	if (r != LZO_E_OK && r != LZO_E_INPUT_NOT_CONSUMED) { /* Goomba pads each save to 4 bytes */
		free(buf);
		*why = "the save would not decompress";
		return -1;
	}
	memset(out, 0xFF, size);
	memcpy(out, buf, n < size ? n : size);
	free(buf);
	return 0;
}

int goomba_write(uint8_t *sram, const char *title, const uint8_t *data, uint32_t size, const char **why)
{
	const char *dummy;
	if (!why)
		why = &dummy;
	if (!goomba_is_sram(sram)) {
		*why = "not Goomba Color's save memory";
		return -1;
	}
	goomba_map m;
	map(sram, title, &m);
	if (!m.save) {
		*why = "the Game Boy game has no save in it yet";
		return -1;
	}
	if (!lzo_ready()) {
		*why = "LZO did not start";
		return -1;
	}
	uint8_t *out = calloc(1, GOOMBA_SRAM_SIZE);
	uint8_t *packed = malloc(size + size / 16 + 64 + 3);
	void *work = malloc(LZO1X_1_MEM_COMPRESS);
	if (!out || !packed || !work) {
		free(out), free(packed), free(work);
		*why = "out of memory";
		return -1;
	}
	lzo_uint n = 0;
	lzo1x_1_compress(data, size, packed, &n, work);
	free(work);
	const uint32_t old_size = le16(sram + m.save);
	const uint32_t new_size = (SH_SIZE + (uint32_t)n + 3) & ~3u;
	/* the headers after this one, and the 48 bytes that end the list */
	const uint32_t rest = m.save + old_size;
	const uint32_t rest_len = (m.end + SH_SIZE <= m.start ? m.end + SH_SIZE : m.start) - rest;
	if (new_size > 0xFFFF || m.save + new_size + rest_len > m.start) {
		free(out), free(packed);
		*why = "no room left in Goomba's save memory";
		return -1;
	}
	memcpy(out, sram, m.save);
	memcpy(out + m.save, sram + m.save, SH_SIZE);
	put16(out + m.save, new_size);
	put32(out + m.save + 4, size);
	memcpy(out + m.save + SH_SIZE, packed, n);
	memcpy(out + m.save + new_size, sram + rest, rest_len);
	memcpy(out + m.start, sram + m.start, GOOMBA_SRAM_SIZE - m.start);
	if (owns_end(sram, &m) && size <= 0x2000)
		memcpy(out + m.start, data, size);
	memcpy(sram, out, GOOMBA_SRAM_SIZE);
	free(out), free(packed);
	return 0;
}

/* ---- RetroArch's settings and playlists ---- */

int ra_cfg_value(const char *cfg, const char *key, char *out, unsigned n)
{
	const size_t k = strlen(key);
	for (const char *line = cfg; line && *line; line = strchr(line, '\n'), line = line ? line + 1 : NULL) {
		while (*line == ' ' || *line == '\t')
			line++;
		if (strncmp(line, key, k) || (line[k] != ' ' && line[k] != '='))
			continue;
		const char *v = line + k;
		while (*v == ' ' || *v == '=')
			v++;
		if (*v == '"')
			v++;
		unsigned len = 0;
		while (v[len] && v[len] != '"' && v[len] != '\n' && v[len] != '\r')
			len++;
		if (len == 0 || len >= n || (len == 7 && !strncmp(v, "default", 7)))
			return 0;
		memcpy(out, v, len);
		out[len] = '\0';
		return 1;
	}
	return 0;
}

int ra_path(const char *in, const char *card, char *out, unsigned n)
{
	int len;
	if (!strncmp(in, "sd:/", 4))
		len = snprintf(out, n, "%s/%s", card, in + 4);
	else if (!strncmp(in, "fs:/vol/", 8))
		len = snprintf(out, n, "%s", in + 3);
	else if (in[0] == '/')
		len = snprintf(out, n, "%s", in);
	else
		return 0;
	return len > 0 && (unsigned)len < n;
}

const char *lpl_next_path(const char *text, char *out, unsigned n)
{
	for (const char *at = strstr(text, "\"path\""); at; at = strstr(at + 6, "\"path\"")) {
		const char *v = at + 6;
		while (*v == ' ' || *v == '\t')
			v++;
		if (*v++ != ':')
			continue;
		while (*v == ' ' || *v == '\t')
			v++;
		if (*v++ != '"')
			continue;
		unsigned len = 0;
		for (; *v && *v != '"'; v++) {
			char c = *v;
			if (c == '\\' && v[1])
				c = *++v; /* \" \\ \/ (playlist paths carry no other escapes) */
			if (len + 1 < n)
				out[len++] = c;
		}
		out[len] = '\0';
		return *v ? v + 1 : v;
	}
	return NULL;
}

/* ---- OurGB's game id ---- */

int oc_game_id(const uint8_t *rom, uint32_t rom_size, char *out, unsigned n)
{
	uint8_t x = 0;
	for (int i = 0x134; i <= 0x14C; i++)
		x = (uint8_t)(x - rom[i] - 1);
	if (x != rom[0x14D])
		return -1;
	char title[16];
	int k = 0, alnum = 0;
	for (int i = 0; i < 15 && rom[0x134 + i]; i++) {
		const uint8_t c = rom[0x134 + i];
		const int letter = (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
		alnum |= letter;
		title[k++] = letter || c == ' ' || c == '.' || c == '-' ? (char)c : '_';
	}
	while (k > 0 && title[k - 1] == ' ')
		k--;
	title[k] = '\0';
	if (!alnum)
		return -1;
	return snprintf(out, n, "%s-%04X-%lu", title, rom[0x14E] << 8 | rom[0x14F], (unsigned long)rom_size / 1024) <
	               (int)n ? 0 : -1;
}

/* ---- which Game Boy games each Transfer Pak game takes ---- */

/* Upper case without spaces, at most n characters. */
static void squeeze(const char *in, int n, char *out)
{
	int k = 0;
	for (int i = 0; i < n && in[i]; i++)
		if (in[i] != ' ')
			out[k++] = (char)toupper((unsigned char)in[i]);
	out[k] = '\0';
}

/* The Game Boy games each Transfer Pak game was made for (Wikipedia's
 * Transfer Pak list; the N64 games from mupen64plus's catalogue). A game is
 * known by its squeezed header name (a prefix; the Japanese ones' may be
 * half-width katakana, kept as bytes) or by its CRC1. A Game Boy game is
 * known by its squeezed header title (a prefix) or by its file name
 * (No-Intro's, lower case). Every title and name here was read from the
 * No-Intro sets' ROMs (2026-10-25). Only these go in the Pak; the N64 game
 * turns any other cartridge away, except the three that take any Game Boy
 * game (any: the named ones still come first). Red and Blue keep their
 * English titles in Europe; Yellow there is POKEMON YELAPSD (F, S, I); Gold,
 * Silver and Crystal end in a region code (POKEMON_GLDAAUE). Mario Artist
 * (64DD) is left out: it wants the Pocket Camera, a mapper Our64 does not
 * have. */
static const char *const g_gen1[] = { "POKEMONRED", "POKEMONGREEN", "POKEMONBLUE", "POKEMONYEL", NULL };
static const char *const g_gen12[] = { "POKEMON_GLD", "POKEMON_SLV", "PM_CRYSTAL", "POKEMONRED", "POKEMONGREEN",
	"POKEMONBLUE", "POKEMONYEL", NULL };
#define LIST(...) ((const char *const[]){ __VA_ARGS__, NULL })
#define CRCS(...) ((const uint32_t[]){ __VA_ARGS__, 0 })
static const tpak_pair g_pairs[] = {
	/* Japan's first two Stadiums by CRC1 only: the first is named POKEMON STADIUM, the second (the West's
	 * Stadium) POKEMON STADIUM 2, and those names start the Western games' entries below */
	{ NULL, CRCS(0x665E8259, 0x58FBD28F, 0x76AF28D8), g_gen1, NULL, "pocket monsters", 0 }, /* Pocket Monsters Stadium */
	{ NULL, CRCS(0x63775886, 0xFE667A12, 0x058381EF), g_gen1, NULL, "pocket monsters", 0 }, /* its Stadium 2 */
	{ "POKEMONSTADIUMG&S", CRCS(0xEE4FD7C2), g_gen12, NULL, "pocket monsters", 0 },        /* Pokemon Stadium Kin Gin */
	{ "\xC1\xAE\xDBQ642", CRCS(0x26CD0F54), LIST("CHOROQHCGB"), LIST("choro q - hyper customable gb"), "choro q", 0 },
	{ "PAWAFURUPUROYAKYU6", CRCS(0xB75E20B7, 0x3C084040, 0x438E6026), LIST("PPSUCSESS"), LIST("power pro kun pocket ("),
	  "power pro", 0 },                                                                        /* Pro Yakyuu 6 */
	{ "PAWAPURO2000", CRCS(0x0AC244D1, 0x4264DF23), LIST("PAWAPOKE2"), LIST("power pro kun pocket 2"), "power pro", 0 },
	/* Shiokaze ni Notte before Nushi Tsuri 64: its name starts the same */
	{ "\xC7\xBC\xC2\xDE\xD8" "64\xBC", CRCS(0x5B9B1618, 0x5B9A9618), LIST("KAWANUSHIG4"), LIST("kawa no nushi tsuri 4"),
	  "nushi tsuri", 0 },
	{ "\xC7\xBC\xC2\xDE\xD8" "64", CRCS(0xD83BB920, 0xC5F1DE79, 0x24081000, 0x9ADD5DE5, 0x49D83BB9), LIST("UMINUSHI"),
	  LIST("umi no nushi tsuri 2"), "nushi tsuri", 0 },                                        /* Nushi Tsuri 64 */
	{ "ULTRAMANBATTLE", CRCS(0xF468118C, 0x37B52113, 0x170D4973), NULL, LIST("ultraman"), "ultraman", 1 },
	{ "PUYOPUYO4", CRCS(0x4B4672B9), LIST("POCKETPUYOSUN"), LIST("pocket puyo puyo sun"), "puyo", 0 }, /* Puyo Puyo~n Party */
	{ "ROBOPON64", CRCS(0x272B690F, 0x7F564549), LIST("ROBOPON", "ROBOBOM", "ROBOMOON"), LIST("robot poncots", "robopon"),
	  "robo", 1 },                                                                             /* Robot Poncots 64 */
	{ "BATTLEPHOENIX64", CRCS(0xF4646B69), LIST("SUPERBDAMAN"), LIST("super b-daman - fighting phoenix"), "b-daman", 0 },
	{ "\xBD\xB0\xCA\xDF\xB0\xDB\xCE\xDE\xAF\xC4\xC0\xB2\xBE\xDD" "64", CRCS(0x1649D810, 0xAF981AB7), LIST("SROBOTLB"),
	  LIST("super robot taisen - link battler"), "robot taisen", 0 },                         /* Super Robot Taisen 64 */
	{ "BEASTWARSMETALS64", CRCS(0x91691C3D), LIST("BEASTWARS"), LIST("beast senshi saikyou ketteisen"), "beast wars", 1 },
	{ "POKEMONSTADIUM2", NULL, g_gen12, NULL, "pokemon", 0 }, /* before Stadium: its name starts the same */
	{ "POKEMONSTADIUM", NULL, g_gen1, NULL, "pokemon", 0 },
	{ "MARIOGOLF", NULL, LIST("MARIOGOLF"), NULL, "golf", 0 },
	{ "MARIOTENNIS", NULL, LIST("CGBTENNIS"), NULL, "tennis", 0 },
	{ "PERFECTDARK", NULL, LIST("PERFDARK"), NULL, "perfect dark", 0 },
	{ "MICKEYUSA", NULL, LIST("MIKSPEEDWAY"), NULL, "mickey", 0 },
};
#define PAIRS (sizeof(g_pairs) / sizeof(g_pairs[0]))
const tpak_pair *tpak_pair_of(const unsigned char *n64_name, uint32_t crc1)
{
	char n64[21];
	squeeze((const char *)n64_name, 20, n64);
	for (unsigned i = 0; i < PAIRS; i++) {
		const tpak_pair *p = &g_pairs[i];
		for (const uint32_t *c = p->crc1; c && *c; c++)
			if (*c == crc1)
				return p;
		if (p->n64 && !strncmp(n64, p->n64, strlen(p->n64)))
			return p;
	}
	return NULL;
}

int tpak_has_word(const char *name, const char *word)
{
	for (; *name; name++) {
		size_t i = 0;
		while (word[i] && tolower((unsigned char)name[i]) == word[i])
			i++;
		if (!word[i])
			return 1;
	}
	return 0;
}

int tpak_made_for(const tpak_pair *p, const char *title, const char *file)
{
	if (!p)
		return 0;
	char g[16];
	squeeze(title, 15, g);
	for (const char *const *t = p->titles; t && *t; t++)
		if (!strncmp(g, *t, strlen(*t)))
			return 2;
	for (const char *const *f = p->files; f && *f; f++)
		if (tpak_has_word(file, *f))
			return 2;
	return p->any;
}
