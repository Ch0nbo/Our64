#include "report.h"

#include <coreinit/mutex.h>
#include <coreinit/time.h>
#include <whb/log.h>
#include <whb/log_console.h>
#include <whb/sdcard.h>

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#ifndef REPORT_FILE
#define REPORT_FILE "bench-results.txt" /* the replay app sets its own */
#endif

static char g_buf[256 * 1024];
static volatile uint32_t g_send_max_us; /* the slowest WHBLogPrint (UDP) since report_send_max_us */

/* The slowest line sent (UDP, console) since the last call, in us. */
uint32_t report_send_max_us(void)
{
	const uint32_t m = g_send_max_us;
	g_send_max_us = 0;
	return m;
}
static size_t g_len;
static char g_root[200];
static char g_path[256];
static bool g_mounted, g_tried;
static OSMutex g_lock;
static bool g_lock_ready;
static bool g_append; /* after report_restart: the file keeps what was saved */
/* A line seen 5 times is not kept again, so a repeating warning cannot
 * fill the log (Puzzle League sends thousands). */
static struct { uint32_t hash; uint32_t count; } seen[128];

#ifdef OUR64_RELEASE
/* The public build keeps only what a player's description cannot give: the
 * version, which ROM, each game's summary, errors, crashes and hangs (Zach,
 * 2026-10-31). The indented lines under a kept line are kept with it. */
static const char *const g_keep_prefix[] = {
	"Our64 ", "OUR ROMS: play ", "Playing ", "GAME: ", "ERROR ", "CRASH", "FAILED", "SAVE FAILED",
	"RESUME FAILED", "SLOW SD", "WATCHDOG", "STALL", "SYSTEM:", "MEMORY:", "GLideN64:", "TPAK: SAVE NOT WRITTEN",
	"Closing after", "No sound", "No JIT area", "Graphics did not start", "Core variables",
};
static bool g_keep_indented;

static bool keep(const char *line)
{
	if (line[0] == ' ')
		return g_keep_indented;
	bool k = strstr(line, "NOT written") || strstr(line, "NOT deleted") || strstr(line, "OUR-0");
	for (size_t i = 0; !k && i < sizeof(g_keep_prefix) / sizeof(g_keep_prefix[0]); i++)
		k = !strncmp(line, g_keep_prefix[i], strlen(g_keep_prefix[i]));
	g_keep_indented = k;
	return k;
}
#endif

/* The watchdog thread saves too, so writers take turns. */
static void lock(void)
{
	if (!g_lock_ready) { OSInitMutex(&g_lock); g_lock_ready = true; }
	OSLockMutex(&g_lock);
}

void report_line(const char *fmt, ...)
{
	char line[512];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(line, sizeof(line), fmt, ap);
	va_end(ap);

	const OSTime send_t0 = OSGetSystemTime();
	WHBLogPrint(line);
	const uint32_t send_us = (uint32_t)OSTicksToMicroseconds(OSGetSystemTime() - send_t0);
	if (send_us > g_send_max_us)
		g_send_max_us = send_us;
#ifndef REPORT_NO_CONSOLE /* apps that draw with GX2 have no text console */
	WHBLogConsoleDraw();
#endif

	size_t n = strlen(line);
	lock();
#ifdef OUR64_RELEASE
	if (!keep(line)) {
		OSUnlockMutex(&g_lock);
		return;
	}
#endif
	uint32_t h = 2166136261u;
	for (size_t i = 0; i < n; i++)
		h = (h ^ (uint8_t)line[i]) * 16777619u;
	int k = 0;
	while (k < 128 && seen[k].count && seen[k].hash != h)
		k++;
	if (k < 128) {
		seen[k].hash = h;
		if (++seen[k].count > 5) {
			if (seen[k].count == 6 && g_len + 40 < sizeof(g_buf))
				g_len += snprintf(g_buf + g_len, sizeof(g_buf) - g_len, "(further copies of that line not logged)\n");
			OSUnlockMutex(&g_lock);
			return;
		}
	}
	if (g_len + n + 2 < sizeof(g_buf)) {
		memcpy(g_buf + g_len, line, n);
		g_len += n;
		g_buf[g_len++] = '\n';
		g_buf[g_len] = '\0';
	}
	OSUnlockMutex(&g_lock);
}

const char *report_sd_root(void)
{
	if (!g_tried) {
		g_tried = true;
		if (WHBMountSdCard()) {
			g_mounted = true;
			snprintf(g_root, sizeof(g_root), "%s/wiiu", WHBGetSdCardMountPath());
			mkdir(g_root, 0777);
			snprintf(g_root, sizeof(g_root), "%s/wiiu/our64", WHBGetSdCardMountPath());
			mkdir(g_root, 0777);
			snprintf(g_path, sizeof(g_path), "%s/" REPORT_FILE, g_root);
		}
	}
	return g_mounted ? g_root : NULL;
}

/* The log is copied under the lock and written after it, so a slow SD write
 * (15 s once, at a player start) holds up only the saves, not every thread
 * that logs a line. Saves after the first append only the lines since the
 * last one: rewriting the whole file took 0.3-1.5 s for 20-30 KB on a slow
 * card, and held up every other SD access meanwhile (2026-10-22). */
static size_t g_saved_len;
static char g_save_buf[sizeof(g_buf)];
static OSMutex g_save_lock;
static bool g_save_lock_ready;

bool report_save(void)
{
	if (!report_sd_root())
		return false;
	lock();
	if (!g_save_lock_ready) {
		OSInitMutex(&g_save_lock);
		g_save_lock_ready = true;
	}
	OSUnlockMutex(&g_lock);
	OSLockMutex(&g_save_lock);
	lock();
	size_t len = g_len;
	size_t from = g_saved_len && g_saved_len <= len ? g_saved_len : 0;
	memcpy(g_save_buf, g_buf + from, len - from);
	OSUnlockMutex(&g_lock);
	OSTime t0 = OSGetSystemTime();
	FILE *f = len == from ? NULL : fopen(g_path, from || g_append ? "a" : "w");
	if (f) {
		if (fwrite(g_save_buf, 1, len - from, f) == len - from)
			g_saved_len = len;
		else
			g_saved_len = 0; /* rewritten whole next time */
		if (fclose(f) != 0)
			g_saved_len = 0;
	}
	/* Past half the buffer, what is on the card leaves it and later saves
	 * append: the buffer held only the first 25 minutes of a game (1.6 KB of
	 * stats every 10 s), so a slowdown after an hour never reached the log
	 * (2026-11-10). Lines logged during the write stay. */
	lock();
	if (f && g_saved_len == len && len > sizeof(g_buf) / 2) {
		memmove(g_buf, g_buf + len, g_len - len + 1);
		g_len -= len;
		g_saved_len = 0;
		g_append = true;
	}
	OSUnlockMutex(&g_lock);
	OSUnlockMutex(&g_save_lock);
	double ms = OSTicksToMicroseconds(OSGetSystemTime() - t0) / 1e3;
	if (ms > 100) /* in the next save: a slow card holds up the thread that saves the log */
		report_line("LOG: %s %u KB of this log took %.0f ms", from ? "appending" : "writing", (unsigned)((len - from) / 1024), ms);
	return f != NULL || len == from;
}

/* One file across starts: what earlier starts wrote stays, the oldest starts
 * dropped whole (at a line beginning "Our64 ") once the file passes _keep
 * bytes; this start's lines are appended. */
void report_continue(size_t _keep)
{
	if (!report_sd_root())
		return;
	FILE *f = fopen(g_path, "rb");
	if (f) {
		fseek(f, 0, SEEK_END);
		long size = ftell(f);
		if (size > (long)_keep) {
			static char old[64 * 1024];
			size_t want = _keep < sizeof(old) - 1 ? _keep : sizeof(old) - 1;
			fseek(f, size - (long)want, SEEK_SET);
			size_t got = fread(old, 1, want, f);
			old[got] = '\0';
			fclose(f);
			f = NULL;
			/* from the first start that fits whole, else from the first whole line */
			char *start = strstr(old, "\nOur64 ");
			if (!start)
				start = strchr(old, '\n');
			FILE *w = fopen(g_path, "wb");
			if (w) {
				if (start)
					fwrite(start + 1, 1, strlen(start + 1), w);
				fclose(w);
			}
		}
		if (f)
			fclose(f);
	}
	lock();
	g_append = true;
	OSUnlockMutex(&g_lock);
}

void report_restart(void)
{
	report_save();
	lock();
	if (g_saved_len == g_len) { /* only once it is all on the card */
		g_len = g_saved_len = 0;
		g_buf[0] = '\0';
		memset(seen, 0, sizeof(seen));
		g_append = true;
	}
	OSUnlockMutex(&g_lock);
}

double report_now_us(void)
{
	return (double)OSTicksToMicroseconds(OSGetSystemTime());
}
