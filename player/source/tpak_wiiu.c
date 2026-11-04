/* Our64 player: the Game Boy cartridge for the Transfer Pak (tpak_wiiu.h).
 *
 * A Game Boy game played as a Wii U Virtual Console inject (UWUVCI: Goomba
 * Color on a GBA base) keeps its save in the inject's save data:
 * usr/save/00050002/<title>/user/<account>/data_008_0000.bin, the GBA VC's
 * settings and then the GBA cartridge's SRAM, which holds Goomba's saves
 * (tpak.c). Other titles' saves are closed to an app, so the file system
 * client is unlocked through Mocha (part of Aroma, as FTPiiU uses it).
 *
 * A Game Boy game played in RetroArch keeps its cartridge RAM as
 * <savefile_directory>[/<core>]/<ROM name>.srm (or beside the ROM), the RAM
 * first and, for a cartridge with a clock, the clock after it. VBA GX
 * (Visual Boy Advance GX, the Wii U app) keeps the same in <save
 * folder>/<ROM name>.sav (<ROM name> Auto.sav in 3.0.4 and older), the save
 * folder vbagx/saves unless its settings.xml names another; a .sav that is a
 * Goomba save (VBA GX reads and writes those as they are) is read as one.
 * OurGB (OurConsoles) keeps the same in <card>/wiiu/ourconsoles/saves/GB/<game
 * id>.srm, the id from the ROM's header (oc_game_id).
 *
 * The ROM comes from <sd root>/gb (*.gb, *.gbc), RetroArch's playlists (the
 * games it played) and folders, VBA GX's ROM folders, or OurConsoles' roms/GB
 * and roms/GBC (only files whose name has the made-for game's word,
 * "pokemon", so as not to open every ROM): for a VC save, the ROM whose
 * header title names a Goomba save; for a RetroArch or VBA GX save, the ROM
 * of the same name; for an OurGB save, the ROM of its id.
 *
 * Saves are looked for in RetroArch first, then VBA GX (the other standalone
 * emulator), then the Virtual Console saves, then OurConsoles (Zach,
 * 2026-10-25: most players come from RetroArch), and the looking stops at the
 * first place with the player's save of a Game Boy game the N64 game was made
 * for. Only a made-for game's save goes in the Pak (g_pairs; any game's in
 * the three that take any); of those, the player's own account wins (another
 * account's VC save comes last), then the place looked in first, then the
 * newest. */
#include "tpak_wiiu.h"
#include "tpak.h"
#include "mocha_wiiu.h"
#include "report.h"

#include <coreinit/filesystem_fsa.h>
#include <coreinit/time.h>
#include <ctype.h>
#include <dirent.h>
#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/* nn::act, for the account playing: its saves come first */
extern int32_t act_initialize(void) __asm__("Initialize__Q2_2nn3actFv");
extern uint32_t act_persistent_id(void) __asm__("GetPersistentId__Q2_2nn3actFv");
extern int32_t act_finalize(void) __asm__("Finalize__Q2_2nn3actFv");

#define VC_SRAM_AT 0x4080 /* where the GBA VC's save file has the cartridge SRAM */
#define VC_FILE_MAX 0x80000
#define FSTIME_1980 315532800LL /* FSTime counts microseconds from 1980, stat's times seconds from 1970 */

enum { FROM_RETROARCH, FROM_VBAGX, FROM_VC, FROM_OURCONSOLES }; /* in the order they are looked in */

static FSAClientHandle g_fsa = -1;
static gb_cart g_cart;
static uint8_t *g_rom, *g_ram;
static uint8_t *g_file;      /* the save file as read */
static uint32_t g_file_size, g_sram_at;
static int g_goomba;          /* a VBA GX .sav that is a Goomba save */
static int g_from;           /* FROM_ */
static char g_title[16];     /* the Game Boy game's header title */
static char g_path[256];     /* the save file */
static char g_quota[128];     /* its title's save directory (flushed after a write) */
static char g_tid[20];       /* "00050002XXXXXXXX"; for RetroArch, VBA GX or OurGB, the save's file name */
static char g_root[256];     /* the SD's our64 folder */
static char g_card[64];      /* the SD card's root */

/* A whole file through FSA, into a 64-byte aligned buffer; NULL on failure. */
static uint8_t *fsa_read(const char *path, uint32_t *size)
{
	FSAStat st;
	if (FSAGetStat(g_fsa, path, &st) != FS_ERROR_OK || st.size == 0 || st.size > VC_FILE_MAX)
		return NULL;
	FSAFileHandle fh;
	if (FSAOpenFileEx(g_fsa, path, "r", 0, FS_OPEN_FLAG_NONE, 0, &fh) != FS_ERROR_OK)
		return NULL;
	uint8_t *buf = memalign(0x40, (st.size + 0x3F) & ~0x3Fu);
	int got = buf ? FSAReadFile(g_fsa, buf, 1, st.size, fh, 0) : -1;
	FSACloseFile(g_fsa, fh);
	if (got != (int)st.size) {
		free(buf);
		return NULL;
	}
	*size = st.size;
	return buf;
}

/* Goomba's SRAM in a VC save file: at VC_SRAM_AT, else wherever its ID is. */
static int find_sram(const uint8_t *file, uint32_t size)
{
	if (size >= VC_SRAM_AT + GOOMBA_SRAM_SIZE && goomba_is_sram(file + VC_SRAM_AT))
		return VC_SRAM_AT;
	for (uint32_t at = 0; at + GOOMBA_SRAM_SIZE <= size; at += 4)
		if (goomba_is_sram(file + at))
			return (int)at;
	return -1;
}

/* ---- the ROMs: <sd root>/gb, RetroArch's playlists and folders, OurConsoles' roms ---- */

typedef struct {
	char path[256];
	char base[128];   /* the file name without its extension */
	char title[16];
	char id[40];      /* OurGB's game id; "" when the header has none */
} gb_rom;
#define ROMS_MAX 128

static int is_gb_file(const char *name)
{
	const char *dot = strrchr(name, '.');
	return dot && (!strcasecmp(dot, ".gb") || !strcasecmp(dot, ".gbc"));
}

/* A whole small file from the SD, NUL-terminated; NULL when missing or larger than max. */
static char *read_text(const char *path, long max)
{
	FILE *f = fopen(path, "rb");
	if (!f)
		return NULL;
	long size = 0;
	if (!fseek(f, 0, SEEK_END))
		size = ftell(f), fseek(f, 0, SEEK_SET);
	char *text = size > 0 && size <= max ? malloc(size + 1) : NULL;
	if (text && fread(text, 1, size, f) == (size_t)size)
		text[size] = '\0';
	else
		free(text), text = NULL;
	fclose(f);
	return text;
}

/* The ROM's index in the list (added unless it is there already), or -1. */
static int add_rom(gb_rom *roms, int *n, const char *path)
{
	const char *name = strrchr(path, '/');
	name = name ? name + 1 : path;
	if (*n >= ROMS_MAX || !is_gb_file(name) || strlen(path) >= sizeof(roms[0].path))
		return -1;
	for (int i = 0; i < *n; i++)
		if (!strcmp(roms[i].path, path))
			return i;
	uint8_t head[0x150];
	FILE *f = fopen(path, "rb");
	int ok = f && fread(head, 1, sizeof(head), f) == sizeof(head);
	long size = ok && !fseek(f, 0, SEEK_END) ? ftell(f) : -1;
	if (f)
		fclose(f);
	if (!ok)
		return -1;
	gb_rom *r = &roms[*n];
	snprintf(r->path, sizeof(r->path), "%s", path);
	snprintf(r->base, sizeof(r->base), "%.*s", (int)(strrchr(name, '.') - name), name);
	gb_title(head, r->title);
	if (size < 0 || oc_game_id(head, (uint32_t)size, r->id, sizeof(r->id)))
		r->id[0] = '\0';
	return (*n)++;
}

/* The Game Boy games a RetroArch playlist names. */
static void add_playlist_roms(gb_rom *roms, int *n, const char *lpl)
{
	char *text = read_text(lpl, 4 << 20);
	if (!text)
		return;
	char p[512], path[256];
	for (const char *at = lpl_next_path(text, p, sizeof(p)); at; at = lpl_next_path(at, p, sizeof(p)))
		if (is_gb_file(p) && ra_path(p, g_card, path, sizeof(path)))
			add_rom(roms, n, path);
	free(text);
}

/* The Game Boy ROMs in dir and in its folders (one per system: roms/GAMEBOY). */
static void add_folder_roms(gb_rom *roms, int *n, const char *dir)
{
	DIR *d = opendir(dir);
	struct dirent *e;
	while (d && (e = readdir(d))) {
		if (e->d_name[0] == '.')
			continue;
		char sub[512];
		snprintf(sub, sizeof(sub), "%s/%s", dir, e->d_name);
		DIR *s = is_gb_file(e->d_name) ? NULL : opendir(sub);
		struct dirent *f;
		while (s && (f = readdir(s))) {
			char path[768];
			snprintf(path, sizeof(path), "%s/%s", sub, f->d_name);
			add_rom(roms, n, path);
		}
		if (s)
			closedir(s);
		else
			add_rom(roms, n, sub);
	}
	if (d)
		closedir(d);
}

/* RetroArch's settings as the Wii U build keeps them. */
typedef struct {
	char saves[256];       /* savefile_directory */
	int beside_rom;        /* savefiles_in_content_dir */
} ra_config;

static void list_roms(gb_rom *roms, int *n, ra_config *ra)
{
	char dir[300];
	snprintf(dir, sizeof(dir), "%s/gb", g_root);
	DIR *d = opendir(dir);
	struct dirent *e;
	while (d && (e = readdir(d))) {
		char path[512];
		snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
		add_rom(roms, n, path);
	}
	if (d)
		closedir(d);

	char cfg_path[128], v[256], path[256];
	snprintf(cfg_path, sizeof(cfg_path), "%s/retroarch/retroarch.cfg", g_card);
	char *cfg = read_text(cfg_path, 1 << 20);
	const char *empty = "";
	const char *c = cfg ? cfg : empty;
	if (!ra_cfg_value(c, "savefile_directory", v, sizeof(v)) || !ra_path(v, g_card, ra->saves, sizeof(ra->saves)))
		snprintf(ra->saves, sizeof(ra->saves), "%s/retroarch/cores/savefiles", g_card);
	ra->beside_rom = ra_cfg_value(c, "savefiles_in_content_dir", v, sizeof(v)) && !strcmp(v, "true");
	if (ra_cfg_value(c, "content_history_path", v, sizeof(v)) && ra_path(v, g_card, path, sizeof(path)))
		add_playlist_roms(roms, n, path);
	else {
		snprintf(path, sizeof(path), "%s/retroarch/content_history.lpl", g_card);
		add_playlist_roms(roms, n, path);
	}
	/* RetroArch's ROM folder and its folders, so a ROM there counts before
	 * RetroArch has played it */
	char browse[256];
	if (!ra_cfg_value(c, "rgui_browser_directory", v, sizeof(v)) || !ra_path(v, g_card, browse, sizeof(browse)))
		snprintf(browse, sizeof(browse), "%s/retroarch/roms", g_card);
	add_folder_roms(roms, n, browse);
	char lists[256];
	if (!ra_cfg_value(c, "playlist_directory", v, sizeof(v)) || !ra_path(v, g_card, lists, sizeof(lists)))
		snprintf(lists, sizeof(lists), "%s/retroarch/cores/playlists", g_card);
	free(cfg);
	if ((d = opendir(lists))) {
		while ((e = readdir(d))) {
			const char *dot = strrchr(e->d_name, '.');
			if (!dot || strcasecmp(dot, ".lpl"))
				continue;
			char lpl[512];
			snprintf(lpl, sizeof(lpl), "%s/%s", lists, e->d_name);
			add_playlist_roms(roms, n, lpl);
		}
		closedir(d);
	}
}

/* VBA GX's settings as its Wii U build keeps them in settings.xml. */
typedef struct {
	char saves[256];       /* saveFolder on the SD; "" when the saves are on a device the player does not open */
} vba_config;

/* VBA GX's settings.xml, looked for where VBA GX looks on the SD
 * (wiiu/apps/vbagx, then vbagx), and its ROM folders and their folders: the
 * one it showed last (loadFolder) and vbagx/roms. Its folders are kept from
 * their device's root; saveDevice and loadDevice say which device (0: the
 * first with a vbagx/saves, the SD on a Wii U without USB saves; 1: the SD;
 * more: USB drives and network shares, which the player does not open). */
static void list_vbagx_roms(gb_rom *roms, int *n, vba_config *vba)
{
	static const char *const homes[] = { "wiiu/apps/vbagx", "vbagx" };
	char *xml = NULL;
	for (unsigned i = 0; !xml && i < 2; i++) {
		char path[128];
		snprintf(path, sizeof(path), "%s/%s/settings.xml", g_card, homes[i]);
		xml = read_text(path, 1 << 20);
	}
	const char *x = xml ? xml : "";
	char v[256], dir[256];
	const int save_sd = !vba_setting(x, "saveDevice", v, sizeof(v)) || atoi(v) <= 1;
	const int load_sd = !vba_setting(x, "loadDevice", v, sizeof(v)) || atoi(v) <= 1;
	if (!save_sd)
		vba->saves[0] = '\0';
	else if (vba_setting(x, "saveFolder", v, sizeof(v))) {
		if (!vba_path(v, g_card, vba->saves, sizeof(vba->saves)))
			vba->saves[0] = '\0';
	} else
		snprintf(vba->saves, sizeof(vba->saves), "%s/vbagx/saves", g_card);
	if (!vba->saves[0])
		report_line("TPAK: VBA GX keeps its saves on a USB drive or a network share: they are not looked at");
	if (load_sd && vba_setting(x, "loadFolder", v, sizeof(v)) && vba_path(v, g_card, dir, sizeof(dir)))
		add_folder_roms(roms, n, dir);
	snprintf(dir, sizeof(dir), "%s/vbagx/roms", g_card);
	add_folder_roms(roms, n, dir);
	free(xml);
}

static const tpak_pair *g_pair; /* this N64 game's; NULL: not a game Our64 knows the Game Boy games of */

static int made_for(const gb_rom *r)
{
	return tpak_made_for(g_pair, r->title, r->base);
}

/* OurConsoles' Game Boy and Color ROMs of the games the N64 game was made
 * for: only the files with the word are opened (the card can have a thousand). */
static void add_ourconsoles_roms(gb_rom *roms, int *n)
{
	static const char *const dirs[] = { "GB", "GBC" };
	for (unsigned k = 0; g_pair && k < 2; k++) {
		char dir[128];
		snprintf(dir, sizeof(dir), "%s/wiiu/ourconsoles/roms/%s", g_card, dirs[k]);
		DIR *d = opendir(dir);
		struct dirent *e;
		while (d && (e = readdir(d))) {
			if (!is_gb_file(e->d_name) || !tpak_has_word(e->d_name, g_pair->scan))
				continue;
			char path[512];
			snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
			const int was = *n, r = add_rom(roms, n, path);
			if (r >= was && made_for(&roms[r]) < 2)
				(*n)--; /* opened for nothing: not one of the made-for games */
		}
		if (d)
			closedir(d);
	}
}

/* ---- finding the VC save ---- */

typedef struct {
	int rom;          /* index in the ROM list */
	int from;         /* FROM_ */
	int mine;         /* the account playing (a RetroArch save on the SD counts as the player's) */
	int made_for;     /* made_for(): 2 a game the N64 game was made for, 1 any game it takes */
	int64_t when;     /* the save file's last change, seconds from 1970 */
	char path[256], quota[128], tid[20];
} candidate;

static int better(const candidate *a, const candidate *b)
{
	if (a->made_for != b->made_for)
		return a->made_for > b->made_for;
	if (a->mine != b->mine)
		return a->mine > b->mine;
	if (a->from != b->from)
		return a->from < b->from;
	return a->when > b->when;
}

static void consider(const candidate *c, candidate *best, int *found)
{
	if (!*found || better(c, best))
		*best = *c;
	(*found)++;
}

/* The looking stops at the first place with the player's save of a made-for game. */
static int go_on(const candidate *best, int found)
{
	return !found || best->made_for < 2 || !best->mine;
}

static void scan_root(const char *root, uint32_t me, const gb_rom *roms, int nroms, const unsigned char *n64_name,
                      candidate *best, int *found, int *files)
{
	char dir[96];
	snprintf(dir, sizeof(dir), "%s/usr/save/00050002", root);
	FSADirectoryHandle dh;
	if (FSAOpenDir(g_fsa, dir, &dh) != FS_ERROR_OK)
		return;
	static FSADirectoryEntry title __attribute__((aligned(0x40)));
	while (FSAReadDir(g_fsa, dh, &title) == FS_ERROR_OK) {
		if (!(title.info.flags & FS_STAT_DIRECTORY) || strlen(title.name) != 8)
			continue;
		char users[192];
		snprintf(users, sizeof(users), "%s/%s/user", dir, title.name);
		FSADirectoryHandle uh;
		if (FSAOpenDir(g_fsa, users, &uh) != FS_ERROR_OK)
			continue;
		static FSADirectoryEntry user __attribute__((aligned(0x40)));
		while (FSAReadDir(g_fsa, uh, &user) == FS_ERROR_OK) {
			if (!(user.info.flags & FS_STAT_DIRECTORY) || strlen(user.name) != 8)
				continue;
			char path[256];
			snprintf(path, sizeof(path), "%s/%s/data_008_0000.bin", users, user.name);
			FSAStat st;
			if (FSAGetStat(g_fsa, path, &st) != FS_ERROR_OK || st.size < GOOMBA_SRAM_SIZE || st.size > VC_FILE_MAX)
				continue;
			uint32_t size;
			uint8_t *file = fsa_read(path, &size);
			if (!file)
				continue;
			(*files)++;
			int at = find_sram(file, size);
			for (int r = 0; at >= 0 && r < nroms; r++) {
				uint8_t probe[0x20];
				if (goomba_read(file + at, roms[r].title, probe, sizeof(probe), NULL))
					continue;
				candidate c = { .rom = r, .from = FROM_VC, .mine = strtoul(user.name, NULL, 16) == me,
				                .made_for = made_for(&roms[r]),
				                .when = (int64_t)(st.modified / 1000000) + FSTIME_1980 };
				snprintf(c.path, sizeof(c.path), "%s", path);
				snprintf(c.quota, sizeof(c.quota), "%s/%s", dir, title.name);
				snprintf(c.tid, sizeof(c.tid), "00050002%s", title.name);
				report_line("TPAK: %s's save in %s (account %s)", roms[r].title, c.tid, user.name);
				consider(&c, best, found);
			}
			free(file);
		}
		FSACloseDir(g_fsa, uh);
	}
	FSACloseDir(g_fsa, dh);
}

/* A RetroArch save of a ROM in the list: <dir>/<ROM name>.srm. */
static void consider_srm(const char *dir, const char *name, const gb_rom *roms, int nroms, const unsigned char *n64_name,
                         candidate *best, int *found, int *files)
{
	const char *dot = strrchr(name, '.');
	if (!dot || strcasecmp(dot, ".srm"))
		return;
	char path[512];
	snprintf(path, sizeof(path), "%s/%s", dir, name);
	struct stat st;
	if (stat(path, &st) || st.st_size <= 0 || strlen(path) >= sizeof(best->path))
		return;
	(*files)++;
	for (int r = 0; r < nroms; r++) {
		if (strncasecmp(roms[r].base, name, dot - name) || roms[r].base[dot - name])
			continue;
		candidate c = { .rom = r, .from = FROM_RETROARCH, .mine = 1, .made_for = made_for(&roms[r]),
		                .when = st.st_mtime };
		snprintf(c.path, sizeof(c.path), "%s", path);
		snprintf(c.tid, sizeof(c.tid), "%.19s", name);
		report_line("TPAK: %s's save in RetroArch's %s", roms[r].title, path);
		consider(&c, best, found);
		return;
	}
}

/* RetroArch's save directory and its folders (one per core when saves are
 * sorted by core), and each ROM's own folder when saves go beside the ROM. */
static void scan_retroarch(const ra_config *ra, const gb_rom *roms, int nroms, const unsigned char *n64_name,
                           candidate *best, int *found, int *files)
{
	DIR *d = opendir(ra->saves);
	struct dirent *e;
	while (d && (e = readdir(d))) {
		if (e->d_name[0] == '.')
			continue;
		char sub[512];
		snprintf(sub, sizeof(sub), "%s/%s", ra->saves, e->d_name);
		const char *dot = strrchr(e->d_name, '.');
		DIR *s = dot && !strcasecmp(dot, ".srm") ? NULL : opendir(sub); /* a folder of one core's saves */
		struct dirent *f;
		while (s && (f = readdir(s)))
			consider_srm(sub, f->d_name, roms, nroms, n64_name, best, found, files);
		if (s)
			closedir(s);
		else
			consider_srm(ra->saves, e->d_name, roms, nroms, n64_name, best, found, files);
	}
	if (d)
		closedir(d);
	for (int r = 0; ra->beside_rom && r < nroms; r++) {
		char dir[256], name[160];
		snprintf(dir, sizeof(dir), "%s", roms[r].path);
		*strrchr(dir, '/') = '\0';
		snprintf(name, sizeof(name), "%s.srm", roms[r].base);
		consider_srm(dir, name, roms, nroms, n64_name, best, found, files);
	}
}

/* VBA GX's saves of the ROMs in the list: <save folder>/<ROM name>.sav. */
static void scan_vbagx(const vba_config *vba, const gb_rom *roms, int nroms, candidate *best, int *found, int *files)
{
	for (int r = 0; vba->saves[0] && r < nroms; r++) {
		int seen = 0; /* a ROM of the same name in another folder: the same save */
		for (int k = 0; k < r && !seen; k++)
			seen = !strcasecmp(roms[k].base, roms[r].base);
		candidate c = { .rom = r, .from = FROM_VBAGX, .mine = 1, .made_for = made_for(&roms[r]) };
		struct stat st;
		if (seen || !vba_save_path(vba->saves, roms[r].base, c.path, sizeof(c.path)) || stat(c.path, &st))
			continue;
		(*files)++;
		c.when = st.st_mtime;
		snprintf(c.tid, sizeof(c.tid), "%.19s", strrchr(c.path, '/') + 1);
		report_line("TPAK: %s's save in VBA GX's %s", roms[r].title, c.path);
		consider(&c, best, found);
	}
}

/* OurGB's saves of the ROMs in the list: saves/GB/<game id>.srm. */
static void scan_ourconsoles(const gb_rom *roms, int nroms, const unsigned char *n64_name, candidate *best, int *found,
                             int *files)
{
	for (int r = 0; r < nroms; r++) {
		if (!roms[r].id[0])
			continue;
		candidate c = { .rom = r, .from = FROM_OURCONSOLES, .mine = 1, .made_for = made_for(&roms[r]) };
		snprintf(c.path, sizeof(c.path), "%s/wiiu/ourconsoles/saves/GB/%s.srm", g_card, roms[r].id);
		struct stat st;
		if (stat(c.path, &st) || st.st_size <= 0)
			continue;
		(*files)++;
		c.when = st.st_mtime;
		snprintf(c.tid, sizeof(c.tid), "%.19s", roms[r].id);
		report_line("TPAK: %s's save in OurConsoles' %s", roms[r].title, c.path);
		consider(&c, best, found);
	}
}

static void release(void)
{
	free(g_rom), free(g_ram), free(g_file);
	g_rom = g_ram = g_file = NULL;
	g_goomba = 0;
	if (g_fsa >= 0)
		FSADelClient(g_fsa);
	g_fsa = -1;
}

int tpak_wiiu_start(const char *sd_root, const unsigned char *n64_name, uint32_t crc1)
{
	release();
	g_pair = tpak_pair_of(n64_name, crc1);
	if (!sd_root)
		return 0;
	snprintf(g_root, sizeof(g_root), "%s", sd_root);
	snprintf(g_card, sizeof(g_card), "%s", sd_root); /* <card>/wiiu/our64 */
	char *wiiu = strstr(g_card, "/wiiu/");
	if (wiiu)
		*wiiu = '\0';
	OSTime t0 = OSGetSystemTime();
	static gb_rom roms[ROMS_MAX];
	int nroms = 0;
	ra_config ra;
	list_roms(roms, &nroms, &ra);
	vba_config vba;
	list_vbagx_roms(roms, &nroms, &vba);
	candidate best;
	int found = 0, vc_files = 0, ra_files = 0, vba_files = 0, oc_files = 0;
	scan_retroarch(&ra, roms, nroms, n64_name, &best, &found, &ra_files);
	if (go_on(&best, found))
		scan_vbagx(&vba, roms, nroms, &best, &found, &vba_files);
	const int ra_roms = nroms;
	if (go_on(&best, found))
		add_ourconsoles_roms(roms, &nroms); /* a Virtual Console or OurGB save may be of one of these */
	if (!nroms) {
		report_line("TPAK: no Game Boy ROMs in %s/gb, RetroArch's playlists and folders, VBA GX's roms or "
		            "OurConsoles' roms: the Transfer Pak stays empty", g_root);
		return 0;
	}
	if (go_on(&best, found)) {
		g_fsa = mocha_wiiu_fsa_client(); /* opens any title's files */
		if (g_fsa >= 0) {
			uint32_t me = 0;
			if (act_initialize() >= 0) { /* nn::Result: negative is a failure */
				me = act_persistent_id();
				act_finalize();
			}
			scan_root("/vol/storage_mlc01", me, roms, nroms, n64_name, &best, &found, &vc_files);
			scan_root("/vol/storage_usb01", me, roms, nroms, n64_name, &best, &found, &vc_files);
		} else
			report_line("TPAK: Mocha did not open the other titles' saves: Virtual Console saves are not looked at");
	}
	if (go_on(&best, found))
		scan_ourconsoles(roms, nroms, n64_name, &best, &found, &oc_files);
	report_line("TPAK: %d Game Boy ROMs (%d from OurConsoles), %d saves of them in %d RetroArch saves (%s), "
	            "%d VBA GX saves (%s), %d Virtual Console save files and %d OurConsoles saves, %.0f ms", nroms,
	            nroms - ra_roms, found, ra_files, ra.saves, vba_files, vba.saves[0] ? vba.saves : "not on the SD",
	            vc_files, oc_files, OSTicksToMicroseconds(OSGetSystemTime() - t0) / 1e3);
	if (!g_pair)
		report_line("TPAK: %.20s (CRC1 %08X) is not in the list of Transfer Pak games and their Game Boy games",
		            (const char *)n64_name, (unsigned)crc1);
	else if (found && !best.made_for)
		report_line("TPAK: none of them is of a Game Boy game %.20s was made for: the Transfer Pak stays empty",
		            (const char *)n64_name);
	if (!found || !best.made_for) {
		release();
		return 0;
	}
	const gb_rom *rom = &roms[best.rom];
	FILE *f = fopen(rom->path, "rb");
	long size = 0;
	if (f && !fseek(f, 0, SEEK_END))
		size = ftell(f), fseek(f, 0, SEEK_SET);
	g_rom = size >= 0x150 && size <= 0x800000 ? malloc(size) : NULL;
	int ok = g_rom && fread(g_rom, 1, size, f) == (size_t)size;
	if (f)
		fclose(f);
	const uint32_t ram_size = ok ? gb_ram_size(g_rom) : 0;
	g_ram = ram_size ? malloc(ram_size) : NULL;
	const char *why = "the ROM or the save could not be read";
	int at = -1;
	if (!ok || !g_ram)
		;
	else if (best.from == FROM_VC) {
		g_file = fsa_read(best.path, &g_file_size);
		at = g_file ? find_sram(g_file, g_file_size) : -1;
		if (at >= 0 && goomba_read(g_file + at, rom->title, g_ram, ram_size, &why))
			at = -1;
	} else {
		long n = 0;
		g_file = (uint8_t *)read_text(best.path, VC_FILE_MAX);
		if (g_file) {
			struct stat st;
			n = stat(best.path, &st) ? 0 : st.st_size;
		}
		if (g_file && best.from == FROM_VBAGX && n >= GOOMBA_SRAM_SIZE && goomba_is_sram(g_file)) {
			g_file_size = (uint32_t)n; /* a Goomba save VBA GX was given: the game's save is inside */
			g_goomba = 1;
			at = goomba_read(g_file, rom->title, g_ram, ram_size, &why) ? -1 : 0;
		} else if (g_file && n >= (long)ram_size) {
			g_file_size = (uint32_t)n;
			memcpy(g_ram, g_file, ram_size);
			at = 0;
		} else if (g_file)
			why = "the save is smaller than the cartridge's RAM";
	}
	if (at < 0 || gb_cart_init(&g_cart, g_rom, (uint32_t)size, g_ram, ram_size, &why)) {
		report_line("TPAK: %s not put in the Transfer Pak: %s", rom->path, why);
		release();
		return 0;
	}
	g_from = best.from;
	g_sram_at = (uint32_t)at;
	snprintf(g_title, sizeof(g_title), "%s", rom->title);
	snprintf(g_path, sizeof(g_path), "%s", best.path);
	snprintf(g_quota, sizeof(g_quota), "%s", best.quota);
	snprintf(g_tid, sizeof(g_tid), "%s", best.tid);
	player_tpak_insert(&g_cart);
	static const char *const from[] = { "RetroArch's ", "VBA GX's ", "", "OurConsoles' " };
	report_line("TPAK: %s (%s, %u KB save from %s%s) is in port 1's Transfer Pak", rom->path, g_title, ram_size / 1024,
	            from[g_from], g_from == FROM_VC ? g_tid : g_path);
	/* VBA GX's default Save Mode, Snapshot, opens the game from its Auto
	 * Snapshot and writes only that: the save the N64 game changes is then
	 * not what VBA GX plays */
	if (g_from == FROM_VBAGX) {
		char sgm[320];
		struct stat a, b;
		snprintf(sgm, sizeof(sgm), "%.*s/%s Auto.sgm", (int)(strrchr(g_path, '/') - g_path), g_path, rom->base);
		if (!stat(sgm, &a) && !stat(g_path, &b) && a.st_mtime > b.st_mtime)
			report_line("TPAK: VBA GX's Auto Snapshot of %s is newer than this save: in VBA GX's Snapshot Save Mode "
			            "the game opens from the Snapshot, not from this save", rom->base);
	}
	return 1;
}

/* The save file as it was, to <sd root>/gb/backup/<title id or save name>-<date>.<bin, sav or srm>. */
static int backup(void)
{
	char path[400];
	snprintf(path, sizeof(path), "%s/gb/backup", g_root);
	mkdir(path, 0777);
	OSCalendarTime t;
	OSTicksToCalendarTime(OSGetTime(), &t);
	char name[20];
	snprintf(name, sizeof(name), "%s", g_tid);
	char *dot = g_from == FROM_RETROARCH || g_from == FROM_VBAGX ? strrchr(name, '.') : NULL;
	if (dot)
		*dot = '\0';
	snprintf(path, sizeof(path), "%s/gb/backup/%s-%04d%02d%02d-%02d%02d%02d.%s", g_root, name, t.tm_year,
	         t.tm_mon + 1, t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec,
	         g_from == FROM_VC ? "bin" : g_from == FROM_VBAGX ? "sav" : "srm");
	FILE *f = fopen(path, "wb");
	int ok = f && fwrite(g_file, 1, g_file_size, f) == g_file_size;
	if (f)
		ok &= !fclose(f);
	if (ok)
		report_line("TPAK: the old save file is in %s", path);
	return ok;
}

/* RetroArch's or OurGB's .srm, or VBA GX's .sav: the cartridge RAM at its
 * start, whatever follows (a clock) kept. A VBA GX .sav that is a Goomba save
 * gets the game's save put back in it, and is written whole. */
static void write_back_srm(void)
{
	const char *why = NULL;
	if (g_goomba && goomba_write(g_file, g_title, g_ram, g_cart.ram_size, &why)) {
		report_line("TPAK: SAVE NOT WRITTEN BACK: %s", why);
		return;
	}
	if (!(g_goomba ? gb_save_put(g_path, g_file, g_file_size) : gb_save_put(g_path, g_ram, g_cart.ram_size)))
		report_line("TPAK: %s's save written back to %s", g_title, g_path);
	else
		report_line("TPAK: SAVE NOT WRITTEN BACK: %s could not be written", g_path);
}

static void write_back(void)
{
	const char *why = NULL;
	if (!backup()) {
		report_line("TPAK: SAVE NOT WRITTEN BACK: the backup to the SD failed");
		return;
	}
	if (g_from != FROM_VC) {
		write_back_srm();
		return;
	}
	if (goomba_write(g_file + g_sram_at, g_title, g_ram, g_cart.ram_size, &why)) {
		report_line("TPAK: SAVE NOT WRITTEN BACK: %s", why);
		return;
	}
	uint8_t *sram = memalign(0x40, GOOMBA_SRAM_SIZE);
	if (!sram) {
		report_line("TPAK: SAVE NOT WRITTEN BACK: out of memory");
		return;
	}
	memcpy(sram, g_file + g_sram_at, GOOMBA_SRAM_SIZE);
	FSAFileHandle fh;
	FSError e = FSAOpenFileEx(g_fsa, g_path, "r+", 0, FS_OPEN_FLAG_NONE, 0, &fh);
	int wrote = -1;
	if (e == FS_ERROR_OK) {
		if ((e = FSASetPosFile(g_fsa, fh, g_sram_at)) == FS_ERROR_OK)
			wrote = FSAWriteFile(g_fsa, sram, 1, GOOMBA_SRAM_SIZE, fh, 0);
		FSACloseFile(g_fsa, fh);
	}
	free(sram);
	FSError flushed = FSAFlushQuota(g_fsa, g_quota);
	char volume[32]; /* "/vol/storage_mlc01" */
	snprintf(volume, sizeof(volume), "%.18s", g_quota);
	if (FSAFlushVolume(g_fsa, volume) != FS_ERROR_OK)
		flushed = -1;
	if (wrote != GOOMBA_SRAM_SIZE)
		report_line("TPAK: SAVE NOT WRITTEN BACK: %s (%d)", FSAGetStatusStr(e), wrote);
	else
		report_line("TPAK: %s's save written back to %s%s", g_title, g_tid,
		            flushed == FS_ERROR_OK ? "" : " (the flush failed)");
}

void tpak_wiiu_stop(void)
{
	if (!g_rom)
		return;
	player_tpak_insert(NULL);
	if (g_cart.ram_written)
		write_back();
	else
		report_line("TPAK: the N64 game did not change %s's save", g_title);
	release();
}
