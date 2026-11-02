/* Our64 player: in-game saves on the SD, through Not64's own load and save
 * functions. Each save type (EEPROM, SRAM, Flash, Controller Pak) lives in
 * sd:/wiiu/our64/saves/<game id>.<eep|sra|fla|mpk>, raw, as Not64 and
 * mupen64plus store them.
 *
 * Not64 sets eepromWritten (and the others) whenever the game writes that
 * save. Once per VI the CPU core reads and clears those flags. A save the
 * game stopped changing 1 s ago (5 s at most) is due: it waits for a resume
 * point to be captured (states_wiiu.c), then is copied out through Not64's
 * save function and goes to the SD writer behind that resume point
 * (saves.c: temp file, then rename, old copy kept as .bak). The VI check is
 * four flag reads; the copy (128 KB at most) happens only when a save goes. */
#include <gctypes.h>
#include "main/rom.h"
#include "fileBrowser/fileBrowser.h"

#include "saves.h"
#include "writer.h"
#include "player.h"
#include "report.h"
#include "pause_wiiu.h"

#include <coreinit/time.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

extern BOOL eepromWritten, sramWritten, flashramWritten, mempakWritten;
int loadEeprom(fileBrowser_file *), saveEeprom(fileBrowser_file *);
int loadSram(fileBrowser_file *), saveSram(fileBrowser_file *);
int loadFlashram(fileBrowser_file *), saveFlashram(fileBrowser_file *);
int loadMempak(fileBrowser_file *), saveMempak(fileBrowser_file *);
extern int (*saveFile_readFile)(fileBrowser_file *, void *, unsigned int);
extern int (*saveFile_writeFile)(fileBrowser_file *, void *, unsigned int);

enum { EEP, SRA, FLA, MPK, TYPES };
static void write_eep(void), write_sra(void), write_fla(void), write_mpk(void);
static const struct {
	const char *ext, *what;
	unsigned size;
	BOOL *written;
	int (*load)(fileBrowser_file *), (*save)(fileBrowser_file *);
	int slot;
	void (*job)(void);
} g_type[TYPES] = {
	{ "eep", "EEPROM", 0x800, &eepromWritten, loadEeprom, saveEeprom, WRITER_EEP, write_eep },
	{ "sra", "SRAM", 0x8000, &sramWritten, loadSram, saveSram, WRITER_SRA, write_sra },
	{ "fla", "Flash", 0x20000, &flashramWritten, loadFlashram, saveFlashram, WRITER_FLA, write_fla },
	{ "mpk", "Controller Pak", 0x20000, &mempakWritten, loadMempak, saveMempak, WRITER_MPK, write_mpk },
};

static char g_path[TYPES][384];       /* sd:/.../saves/<game id>.<ext> */
static int g_on;                      /* saves_start succeeded */
static saves_timer g_timer[TYPES];
static int g_waiting[TYPES];          /* due, waiting for a resume point */
static double g_waiting_since = -1;
static fileBrowser_file g_dir;        /* what Not64 passes back to the hooks */

/* The CPU core leaves a copy in g_pending; the writer job moves it to
 * g_work (under the writer lock) and writes that. */
static uint8_t g_pending[TYPES][0x20000] __attribute__((aligned(64)));
static uint8_t g_work[0x20000] __attribute__((aligned(64)));

static int type_of(const char *name)
{
	const char *dot = strrchr(name, '.');
	for (int t = 0; dot && t < TYPES; t++)
		if (!strcmp(dot + 1, g_type[t].ext))
			return t;
	return -1;
}

static void write_type(int t)
{
	writer_lock();
	memcpy(g_work, g_pending[t], g_type[t].size);
	writer_unlock();
	OSTime t0 = OSGetSystemTime();
	int failed = savefile_write(g_path[t], g_work, g_type[t].size);
	writer_slow(strrchr(g_path[t], '/') + 1, OSTicksToMicroseconds(OSGetSystemTime() - t0) / 1e3);
	if (failed)
		report_line("SAVE FAILED: %s not written to %s (SD full, locked or removed?)", g_type[t].what, g_path[t]);
	states_write_done(WRITE_INGAME, !failed);
}
static void write_eep(void) { write_type(EEP); }
static void write_sra(void) { write_type(SRA); }
static void write_fla(void) { write_type(FLA); }
static void write_mpk(void) { write_type(MPK); }

/* Not64's save function hands over the whole buffer: copy it for the writer. */
static int hook_write(fileBrowser_file *file, void *buf, unsigned int len)
{
	int t = type_of(file->name);
	if (t < 0 || len != g_type[t].size)
		return -1;
	writer_lock();
	memcpy(g_pending[t], buf, len);
	writer_unlock();
	writer_post(g_type[t].slot, g_type[t].job);
	return (int)len;
}

/* Not64 first reads 4 bytes to see whether the file exists, then the whole
 * save from offset 0. The first call reads the newest complete copy. */
static int g_read_from[TYPES];
static int hook_read(fileBrowser_file *file, void *buf, unsigned int len)
{
	static uint8_t whole[0x20000];
	int t = type_of(file->name);
	if (t < 0)
		return -1;
	if (len == 4 && file->offset == 0 && len != g_type[t].size) {
		g_read_from[t] = savefile_read(g_path[t], whole, g_type[t].size);
		if (g_read_from[t] == SAVEFILE_NONE)
			return 0;
		memcpy(buf, whole, 4);
		return 4;
	}
	if (len != g_type[t].size || g_read_from[t] == SAVEFILE_NONE)
		return -1;
	memcpy(buf, whole, len);
	return (int)len;
}

int saves_start(const char *root)
{
	char dir[256], id[64];
	snprintf(dir, sizeof(dir), "%s/saves", root);
	mkdir(dir, 0777);
	saves_game_id(id, sizeof(id), ROM_HEADER.Name, ROM_HEADER.CRC1);
	for (int t = 0; t < TYPES; t++)
		snprintf(g_path[t], sizeof(g_path[t]), "%s/%s.%s", dir, id, g_type[t].ext);

	saveFile_readFile = hook_read;
	saveFile_writeFile = hook_write;
	/* Not64 sprintf()s "<dir>/<goodname><region>.<ext>" into a 128-byte name
	 * and the hooks only look at the extension, so the folder it is given
	 * is one letter: a long game name cannot overrun the buffer. */
	snprintf(g_dir.name, sizeof(g_dir.name), "s");
	static const char *const from[] = { "new", "", " (from the .tmp a power cut left)", " (from the .bak: the file was damaged)" };
	for (int t = 0; t < TYPES; t++) {
		int r = g_type[t].load(&g_dir);
		*g_type[t].written = FALSE; /* Not64 sets it on load; from here on it means "changed" */
		if (r < 0)
			report_line("SAVE: %s %s could not be read; starting blank", g_type[t].what, g_path[t]);
		else if (g_read_from[t] != SAVEFILE_NONE)
			report_line("SAVE: %s loaded%s", g_type[t].what, from[g_read_from[t]]);
	}
	report_line("SAVE: saves for %s go to %s", id, dir);
	g_on = 1;
	return 0;
}

static void save_now(int t)
{
	*g_type[t].written = TRUE; /* Not64's save function writes only when it is set */
	g_type[t].save(&g_dir);
	*g_type[t].written = FALSE;
}

void saves_poll(double now_us)
{
	if (!g_on)
		return;
	for (int t = 0; t < TYPES; t++) {
		int changed = *g_type[t].written;
		if (changed)
			*g_type[t].written = FALSE;
		if (saves_due(&g_timer[t], changed, now_us) && !g_waiting[t]) {
			g_waiting[t] = 1;
			if (g_waiting_since < 0)
				g_waiting_since = now_us;
		}
	}
}

double saves_waiting_since(void) { return g_waiting_since; }

int saves_pending(void)
{
	if (!g_on)
		return 0;
	for (int t = 0; t < TYPES; t++)
		if (*g_type[t].written || g_timer[t].dirty || g_waiting[t])
			return 1;
	return 0;
}

void saves_release(void)
{
	for (int t = 0; t < TYPES; t++)
		if (g_waiting[t]) {
			g_waiting[t] = 0;
			save_now(t);
		}
	g_waiting_since = -1;
}

void saves_stop(void)
{
	saves_flush();
	g_on = 0;
	memset(g_timer, 0, sizeof(g_timer));
	memset(g_waiting, 0, sizeof(g_waiting));
	g_waiting_since = -1;
}

void saves_flush(void)
{
	if (!g_on)
		return;
	for (int t = 0; t < TYPES; t++) {
		int changed = *g_type[t].written;
		*g_type[t].written = FALSE;
		if (saves_take(&g_timer[t]) || changed)
			g_waiting[t] = 1;
	}
	saves_release();
}
