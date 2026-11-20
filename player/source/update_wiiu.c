/* Our64 player: the updater on the console (see update_wiiu.h).
 *
 * Every wait is curl's, which asks on_progress about once a second and
 * whenever data moves: the stop flag ends a transfer there. The name lookup
 * has no such call (as in covers_wiiu.c): a stop during it waits for it. */
#include "update_wiiu.h"
#include "setup.h"
#include "version.h"
#include "report.h"

#include <coreinit/dynload.h>
#include <coreinit/thread.h>
#include <coreinit/time.h>
#include <curl/curl.h>
#include <mbedtls/sha256.h>
#include <nn/ac/ac_c.h>
#include <whb/sdcard.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include "cacert_pem.h" /* third_party/cacert/cacert.pem, built in (bin2s) */

/* the last 10 releases, newest first: the newest is the update, and the
 * Release notes page shows all of theirs from 1.3.0 on (one request, as before) */
#define API_URL "https://api.github.com/repos/Ch0nbo/Our64/releases?per_page=10"
#define SLICE_MS 100
#define AC_TIMEOUT_MS 30000 /* to bring the network up */
#define API_MAX (512 * 1024) /* the list's JSON: 40 KB for the 6 releases up to 1.2.0, their notes included */

/* nn::ac::GetConnectStatus: wut declares it only for C++ (nn/ac/ac_cpp.h). */
enum { AC_STATUS_FAILED = -1, AC_STATUS_OK = 0, AC_STATUS_PROCESSING = 1 };
extern NNResult updater_ac_status(int32_t *status) __asm__("GetConnectStatus__Q2_2nn2acFPQ3_2nn2ac6Status");

enum { JOB_NONE, JOB_CHECK, JOB_DOWNLOAD };

static OSThread g_thread __attribute__((aligned(16)));
static uint8_t g_stack[256 * 1024] __attribute__((aligned(16))); /* TLS: mbedTLS's big-number work */
static int g_job;                 /* the menu thread's: JOB_ started and not finished */
static volatile int g_stop, g_finished;
static volatile int g_step;
static volatile unsigned g_done, g_total;
static int g_result;              /* the thread's, read after it finished */
static int g_curl_ready;

static char g_root[256];          /* "<sd>/wiiu/our64" */
static char g_self[320];          /* the running .wuhb, a full path; "" unknown */
static char g_self_rel[256];      /* the same from the card's root, as Aroma's loader names it */
static int g_loader;              /* Aroma's loader module was found */

static update_release g_release;  /* the last good check's */
static int g_shaders_new;
static char g_notes[64 * 1024];   /* the releases' notes, as lines for the Release notes page */
#define NOTES_WIDTH 70            /* bytes a line: the page's width at its text size */
static update_release g_want;     /* a download's */
static int g_want_app, g_want_shaders;

/* devkitPro's curl is built with brotli. Our64 never asks a server for it
 * (no Accept-Encoding), and libgui already stands in for the one brotli
 * function FreeType names, to keep the 175 KB decoder out of the app: these
 * stand in for curl's the same way. A decoder that cannot be made is how
 * curl learns there is none. */
#include <stdint.h>
uint32_t BrotliDecoderVersion(void) { return 0; }
void *BrotliDecoderCreateInstance(void *alloc, void *release, void *opaque)
{
	(void)alloc, (void)release, (void)opaque;
	return NULL;
}
void BrotliDecoderDestroyInstance(void *state) { (void)state; }
int BrotliDecoderDecompressStream(void *state, size_t *in_n, const uint8_t **in, size_t *out_n, uint8_t **out, size_t *total)
{
	(void)state, (void)in_n, (void)in, (void)out_n, (void)out, (void)total;
	return 0; /* BROTLI_DECODER_RESULT_ERROR */
}
int BrotliDecoderGetErrorCode(const void *state)
{
	(void)state;
	return -31; /* BROTLI_DECODER_ERROR_UNREACHABLE */
}

/* ---- one transfer ---- */

typedef struct {
	FILE *f;              /* to the card, or NULL: into mem */
	char *mem;
	unsigned n, cap;
	int write_failed, too_big;
	mbedtls_sha256_context sha;
} sink;

static size_t on_data(char *p, size_t size, size_t count, void *ctx)
{
	sink *s = ctx;
	const size_t n = size * count;
	if (s->f) {
		if (fwrite(p, 1, n, s->f) != n) {
			s->write_failed = 1;
			return 0;
		}
		mbedtls_sha256_update_ret(&s->sha, (const unsigned char *)p, n);
	} else {
		if (s->n + n >= s->cap) {
			s->too_big = 1;
			return 0;
		}
		memcpy(s->mem + s->n, p, n);
	}
	s->n += (unsigned)n;
	return n;
}

static int on_progress(void *ctx, curl_off_t total, curl_off_t now, curl_off_t ut, curl_off_t un)
{
	(void)ut;
	(void)un;
	const unsigned known = *(const unsigned *)ctx; /* the size GitHub named; else what the server says */
	g_done = (unsigned)now;
	g_total = known ? known : (unsigned)total;
	return g_stop ? 1 : 0;
}

/* Window scaling and a large receive buffer: the Wii U's defaults are slow. */
static int on_socket(void *ctx, curl_socket_t fd, curlsocktype purpose)
{
	(void)ctx;
	(void)purpose;
	int on = 1, buf = 128 * 1024;
	setsockopt(fd, SOL_SOCKET, SO_WINSCALE, &on, sizeof(on));
	setsockopt(fd, SOL_SOCKET, SO_TCPSACK, &on, sizeof(on));
	setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &buf, sizeof(buf));
	return CURL_SOCKOPT_OK;
}

static int fetch(const char *what, const char *url, sink *s, unsigned known_size)
{
	char err[CURL_ERROR_SIZE] = "";
	struct curl_blob ca = { (void *)cacert_pem, cacert_pem_size, CURL_BLOB_NOCOPY };
	if (!g_curl_ready) {
		if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) {
			report_line("UPDATE: curl did not start");
			return UPDATE_NO_START;
		}
		g_curl_ready = 1;
	}
	CURL *c = curl_easy_init();
	if (!c) {
		report_line("UPDATE: no memory for a transfer");
		return UPDATE_NO_START;
	}
	struct curl_slist *headers = curl_slist_append(NULL, "Accept: application/vnd.github+json, application/octet-stream");
	curl_easy_setopt(c, CURLOPT_URL, url);
	curl_easy_setopt(c, CURLOPT_USERAGENT, "Our64/" OUR64_VERSION);
	curl_easy_setopt(c, CURLOPT_HTTPHEADER, headers);
	curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
	curl_easy_setopt(c, CURLOPT_MAXREDIRS, 5L);
#ifdef OUR64_RELEASE
	curl_easy_setopt(c, CURLOPT_PROTOCOLS_STR, "https");
	curl_easy_setopt(c, CURLOPT_REDIR_PROTOCOLS_STR, "https");
#else /* the developer's build may be pointed at a computer on the network (update-source.txt) */
	curl_easy_setopt(c, CURLOPT_PROTOCOLS_STR, "http,https");
	curl_easy_setopt(c, CURLOPT_REDIR_PROTOCOLS_STR, "http,https");
#endif
	curl_easy_setopt(c, CURLOPT_CAINFO_BLOB, &ca);
	curl_easy_setopt(c, CURLOPT_SSL_VERIFYPEER, 1L);
	curl_easy_setopt(c, CURLOPT_SSL_VERIFYHOST, 2L);
	curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 20L);
	curl_easy_setopt(c, CURLOPT_LOW_SPEED_LIMIT, 1L);
	curl_easy_setopt(c, CURLOPT_LOW_SPEED_TIME, 20L);
	curl_easy_setopt(c, CURLOPT_BUFFERSIZE, 128L * 1024);
	curl_easy_setopt(c, CURLOPT_ERRORBUFFER, err);
	curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, on_data);
	curl_easy_setopt(c, CURLOPT_WRITEDATA, s);
	curl_easy_setopt(c, CURLOPT_XFERINFOFUNCTION, on_progress);
	curl_easy_setopt(c, CURLOPT_XFERINFODATA, &known_size);
	curl_easy_setopt(c, CURLOPT_NOPROGRESS, 0L);
	curl_easy_setopt(c, CURLOPT_SOCKOPTFUNCTION, on_socket);
	const OSTime t0 = OSGetSystemTime();
	const CURLcode rc = curl_easy_perform(c);
	long http = 0;
	curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &http);
	curl_easy_cleanup(c);
	curl_slist_free_all(headers);
	report_line("UPDATE: %s: curl %d%s%s, http %ld, %u bytes in %.1f s", what, (int)rc, rc ? " " : "",
	            rc ? (err[0] ? err : curl_easy_strerror(rc)) : "", http, s->n,
	            OSTicksToMilliseconds(OSGetSystemTime() - t0) / 1000.0);
	if (g_stop)
		return UPDATE_STOPPED;
	if (s->write_failed)
		return UPDATE_CARD;
	switch (rc) {
	case CURLE_OK: return http == 200 ? UPDATE_OK : UPDATE_NO_SERVER;
	case CURLE_COULDNT_RESOLVE_HOST:
	case CURLE_COULDNT_RESOLVE_PROXY: return UPDATE_NO_NETWORK;
	case CURLE_PEER_FAILED_VERIFICATION:
	case CURLE_SSL_CERTPROBLEM:
	case CURLE_SSL_CACERT_BADFILE:
	case CURLE_SSL_ISSUER_ERROR: return UPDATE_CERTIFICATE;
	default: return UPDATE_NO_SERVER;
	}
}

/* ---- the network up (nn::ac), as Fetch covers does ---- */

static int network_up(int *ours)
{
	BOOL on = FALSE;
	*ours = 0;
	if (NNResult_IsSuccess(ACIsApplicationConnected(&on)) && on)
		return UPDATE_OK;
	if (NNResult_IsFailure(ACConnectAsync())) {
		report_line("UPDATE: ACConnectAsync failed");
		return UPDATE_NO_NETWORK;
	}
	*ours = 1;
	for (int waited = 0; waited < AC_TIMEOUT_MS && !g_stop; waited += SLICE_MS) {
		int32_t status = AC_STATUS_PROCESSING;
		if (NNResult_IsFailure(updater_ac_status(&status)) || status == AC_STATUS_FAILED) {
			report_line("UPDATE: not connected (status %d)", (int)status);
			return UPDATE_NO_NETWORK;
		}
		if (status == AC_STATUS_OK)
			return UPDATE_OK;
		OSSleepTicks(OSMillisecondsToTicks(SLICE_MS));
	}
	if (g_stop)
		return UPDATE_STOPPED;
	report_line("UPDATE: not connected after %d s", AC_TIMEOUT_MS / 1000);
	return UPDATE_NO_NETWORK;
}

/* ---- files ---- */

static void path_in(char *out, unsigned size, const char *folder, const char *name)
{
	snprintf(out, size, "%s/%s/%s", g_root, folder, name);
}

/* A whole file (a byte of zero after it), or NULL. */
static unsigned char *read_whole(const char *path, unsigned *size)
{
	FILE *f = fopen(path, "rb");
	if (!f)
		return NULL;
	unsigned char *data = NULL;
	if (!fseek(f, 0, SEEK_END)) {
		const long n = ftell(f);
		if (n >= 0 && n < 64 * 1024 * 1024 && !fseek(f, 0, SEEK_SET) && (data = malloc((size_t)n + 1)) != NULL) {
			if (fread(data, 1, (size_t)n, f) == (size_t)n) {
				data[n] = 0;
				*size = (unsigned)n;
			} else {
				free(data);
				data = NULL;
			}
		}
	}
	fclose(f);
	return data;
}

/* data to path. direct: written over what is there, for a shader pack: in a
 * full folder each create, delete and rename takes the card about a second,
 * and a pack cut off by a power loss is one the graphics core already treats
 * as absent and builds again. Otherwise through path.tmp, renamed into
 * place, so a cut-off write leaves the old file (a .keys cut mid-line would
 * read as a key the game never used). No .bak either way: every file more in
 * games/ slows each open there. */
static int write_whole(const char *path, const void *data, unsigned size, int direct)
{
	char tmp[340];
	snprintf(tmp, sizeof(tmp), "%s%s", path, direct ? "" : ".tmp");
	FILE *f = fopen(tmp, "wb");
	if (!f)
		return -1;
	int ok = fwrite(data, 1, size, f) == size;
	ok = fclose(f) == 0 && ok;
	if (ok && !direct) {
		remove(path);
		ok = rename(tmp, path) == 0;
	}
	if (!ok && !direct)
		remove(tmp);
	return ok ? 0 : -1;
}

/* One of the release's files to <root>/update/<name>, then its size and
 * SHA-256 against what GitHub named. */
static int download(const char *name, const update_asset *a, int step)
{
	static char buffer[256 * 1024]; /* stdio's: the card is slow at small writes */
	char path[320], hex[65];
	unsigned char digest[32];
	path_in(path, sizeof(path), "update", name);
	sink s;
	memset(&s, 0, sizeof(s));
	s.f = fopen(path, "wb");
	if (!s.f) {
		report_line("UPDATE: can't create %s (errno %d)", path, errno);
		return UPDATE_CARD;
	}
	setvbuf(s.f, buffer, _IOFBF, sizeof(buffer));
	mbedtls_sha256_init(&s.sha);
	mbedtls_sha256_starts_ret(&s.sha, 0);
	g_step = step;
	g_done = 0;
	g_total = a->size;
	int r = fetch(name, a->url, &s, a->size);
	if (fclose(s.f) && r == UPDATE_OK)
		r = UPDATE_CARD;
	mbedtls_sha256_finish_ret(&s.sha, digest);
	mbedtls_sha256_free(&s.sha);
	for (int i = 0; i < 32; i++)
		sprintf(hex + i * 2, "%02x", digest[i]);
	if (r == UPDATE_OK && ((a->size && s.n != a->size) || (a->sha256[0] && strcmp(hex, a->sha256)))) {
		report_line("UPDATE: %s is not what the release names: %u bytes (%u), sha256 %s (%s)", name, s.n, a->size, hex,
		            a->sha256[0] ? a->sha256 : "none named");
		r = UPDATE_DAMAGED;
	}
	if (r != UPDATE_OK)
		remove(path);
	return r;
}

/* ---- shaders.zip into the card's shaders/games ---- */

static int ends_with(const char *s, const char *end)
{
	const size_t n = strlen(s), m = strlen(end);
	return n > m && !strcmp(s + n - m, end);
}

/* The SHA-256 of the shaders.zip the card's shaders came from
 * (shaders/release.txt: written here, and by the full download's zip). */
static void shaders_have(char *out, unsigned size)
{
	char path[320];
	out[0] = 0;
	path_in(path, sizeof(path), "shaders", "release.txt");
	FILE *f = fopen(path, "r");
	if (!f)
		return;
	if (fgets(out, (int)size, f))
		out[strcspn(out, " \r\n")] = 0;
	else
		out[0] = 0;
	fclose(f);
}

static int shaders_unpack(const update_asset *a)
{
	char path[320];
	path_in(path, sizeof(path), "update", UPDATE_SHADERS_NAME);
	FILE *f = fopen(path, "rb");
	update_zip z;
	if (!f || update_zip_open(f, &z)) {
		if (f)
			fclose(f);
		report_line("UPDATE: shaders.zip can't be read");
		return UPDATE_DAMAGED;
	}
	g_step = USTEP_UNPACK;
	g_done = 0;
	g_total = z.count;
	snprintf(path, sizeof(path), "%s/shaders", g_root);
	mkdir(path, 0777);
	path_in(path, sizeof(path), "shaders", "games");
	mkdir(path, 0777);
	const OSTime t0 = OSGetSystemTime();
	int r = UPDATE_OK, written = 0, merged = 0, kept = 0;
	OSTime t_write = 0; /* in the writes alone: the rest is reading the zip and the card's files */
	update_zip_entry e;
	while (r == UPDATE_OK && update_zip_next(&z, &e)) {
		g_done = z.index;
		if (g_stop) {
			r = UPDATE_STOPPED;
			break;
		}
		const int pack = ends_with(e.name, ".pack"), keys = ends_with(e.name, ".keys");
		/* games/<name>.pack or .keys and nothing else: no other folder, no path out of it */
		if (strncmp(e.name, "games/", 6) || strchr(e.name + 6, '/') || strstr(e.name, "..") || !(pack || keys))
			continue;
		unsigned char *fresh = update_zip_read(f, &e);
		if (!fresh) {
			report_line("UPDATE: %s in shaders.zip is damaged", e.name);
			r = UPDATE_DAMAGED;
			break;
		}
		path_in(path, sizeof(path), "shaders", e.name);
		unsigned old_size = 0, n = 0, added = 0;
		unsigned char *old = read_whole(path, &old_size);
		void *out = NULL;
		if (pack) {
			out = update_pack_merge(fresh, e.size, old, old_size, &n, &added);
		} else {
			char *text = malloc(e.size + 1);
			if (text) {
				memcpy(text, fresh, e.size);
				text[e.size] = 0;
				out = update_keys_merge(text, (const char *)old, &n, &added);
				free(text);
			}
		}
		if (!out) {
			report_line("UPDATE: %s in shaders.zip is not a shader file (or no memory)", e.name);
			r = UPDATE_DAMAGED;
		} else if (old && !added) {
			kept++; /* the card's file has all of it: no write */
		} else if ((t_write -= OSGetSystemTime()), write_whole(path, out, n, pack)) {
			report_line("UPDATE: %s not written (errno %d)", path, errno);
			r = UPDATE_CARD;
		} else {
			t_write += OSGetSystemTime();
			written++;
			merged += old != NULL;
		}
		free(out);
		free(old);
		free(fresh);
	}
	update_zip_close(&z);
	fclose(f);
	if (r == UPDATE_OK) {
		char line[80];
		const int n = snprintf(line, sizeof(line), "%s\n", a->sha256[0] ? a->sha256 : "unknown");
		path_in(path, sizeof(path), "shaders", "release.txt");
		if (write_whole(path, line, (unsigned)n, 0))
			r = UPDATE_CARD;
	}
	report_line("UPDATE: shaders: %d files written (%d merged with the card's) in %.1f s, %d already there, %.1f s in all%s",
	            written, merged, r == UPDATE_CARD ? 0.0 : OSTicksToMilliseconds(t_write) / 1000.0, kept,
	            OSTicksToMilliseconds(OSGetSystemTime() - t0) / 1000.0, r == UPDATE_OK ? "" : ", not finished");
	path_in(path, sizeof(path), "update", UPDATE_SHADERS_NAME);
	remove(path);
	return r;
}

/* ---- the jobs ---- */

/* Where a check asks. The developer's build reads another address from
 * <root>/update-source.txt (a computer serving a release's JSON and files). */
static void api_url(char *out, unsigned size)
{
	snprintf(out, size, "%s", API_URL);
#ifndef OUR64_RELEASE
	char path[320];
	snprintf(path, sizeof(path), "%s/update-source.txt", g_root);
	FILE *f = fopen(path, "r");
	if (f) {
		char line[256];
		if (fgets(line, sizeof(line), f)) {
			line[strcspn(line, " \r\n")] = 0;
			if (line[0]) {
				snprintf(out, size, "%s", line);
				report_line("UPDATE: asking %s (update-source.txt)", out);
			}
		}
		fclose(f);
	}
#endif
}

static int check(void)
{
	char url[256];
	api_url(url, sizeof(url));
	sink s;
	memset(&s, 0, sizeof(s));
	s.cap = API_MAX;
	s.mem = malloc(s.cap);
	if (!s.mem)
		return UPDATE_NO_START;
	g_done = g_total = 0;
	int r = fetch("check", url, &s, 0);
	if (r == UPDATE_OK) {
		update_release rel;
		s.mem[s.n] = 0;
		if (update_parse_release(s.mem, &rel)) {
			report_line("UPDATE: the answer is not a release (%u bytes)", s.n);
			r = UPDATE_NO_SERVER;
		} else {
			char have[80];
			shaders_have(have, sizeof(have));
			g_release = rel;
			update_release_notes(s.mem, OUR64_VERSION, g_notes, sizeof(g_notes), NOTES_WIDTH);
			g_shaders_new = rel.shaders.url[0] && (!rel.shaders.sha256[0] || strcmp(have, rel.shaders.sha256));
			report_line("UPDATE: newest is %s (this is " OUR64_VERSION "): app %u bytes%s, shaders %u bytes%s", rel.version,
			            rel.app.size, rel.app.url[0] ? "" : " (not in the release)", rel.shaders.size,
			            !rel.shaders.url[0] ? " (not in the release)" : g_shaders_new ? ", new" : ", the card's");
		}
	}
	free(s.mem);
	return r;
}

static int get_files(void)
{
	char path[320];
	snprintf(path, sizeof(path), "%s/update", g_root);
	mkdir(path, 0777);
	int r = UPDATE_OK;
	if (g_want_app) {
		r = download(UPDATE_APP_NAME, &g_want.app, USTEP_APP);
		/* the ini goes with the app; a release without one keeps the card's */
		if (r == UPDATE_OK && g_want.ini.url[0])
			r = download(UPDATE_INI_NAME, &g_want.ini, USTEP_APP);
	}
	if (r == UPDATE_OK && g_want_shaders && g_want.shaders.url[0]) {
		r = download(UPDATE_SHADERS_NAME, &g_want.shaders, USTEP_SHADERS);
		if (r == UPDATE_OK)
			r = shaders_unpack(&g_want.shaders);
		if (r == UPDATE_OK)
			g_shaders_new = 0;
	}
	return r;
}

static int worker(int argc, const char **argv)
{
	(void)argv;
	int ours = 0, r = UPDATE_NO_NETWORK;
	const int ac = NNResult_IsSuccess(ACInitialize());
	if (!ac)
		report_line("UPDATE: ACInitialize failed");
	else
		r = network_up(&ours);
	if (r == UPDATE_OK && !g_stop)
		r = argc == JOB_CHECK ? check() : get_files();
	if (ours)
		ACClose();
	if (ac)
		ACFinalize();
	if (g_stop && r != UPDATE_OK)
		r = UPDATE_STOPPED;
	g_result = r;
	g_finished = 1;
	return 0;
}

static int start(int job)
{
	if (g_job != JOB_NONE || !g_root[0])
		return -1;
	g_stop = g_finished = 0;
	g_result = UPDATE_OK;
	g_step = USTEP_APP;
	g_done = g_total = 0;
	/* below the loader (26) and the SD writer (24), on core 2 with them */
	if (!OSCreateThread(&g_thread, worker, job, NULL, g_stack + sizeof(g_stack), sizeof(g_stack), 28,
	                    OS_THREAD_ATTRIB_AFFINITY_CPU2)) {
		report_line("UPDATE: no thread");
		return -1;
	}
	OSSetThreadName(&g_thread, "Our64 updater");
	g_job = job;
	OSResumeThread(&g_thread);
	return 0;
}

int updater_check_start(void) { return start(JOB_CHECK); }

int updater_download_start(const update_release *r, int app, int shaders)
{
	if (g_job != JOB_NONE)
		return -1;
	g_want = *r;
	g_want_app = app && r->app.url[0];
	g_want_shaders = shaders && r->shaders.url[0];
	if (!g_want_app && !g_want_shaders)
		return -1;
	report_line("UPDATE: downloading %s%s%s", g_want_app ? r->version : "", g_want_app && g_want_shaders ? " and " : "",
	            g_want_shaders ? "shaders" : "");
	return start(JOB_DOWNLOAD);
}

int updater_job(void) { return g_job; }
int updater_running(void) { return g_job != JOB_NONE && !g_finished; }

void updater_progress(int *step, unsigned *done, unsigned *total)
{
	*step = g_step;
	*done = g_done;
	*total = g_total;
}

void updater_stop(void) { g_stop = 1; }

int updater_finish(void)
{
	if (g_job == JOB_NONE)
		return UPDATE_STOPPED;
	int ret;
	OSJoinThread(&g_thread, &ret);
	g_job = JOB_NONE;
	return g_result;
}

const update_release *updater_release(void) { return &g_release; }
int updater_shaders_new(void) { return g_shaders_new; }
const char *updater_notes(void) { return g_notes; }

/* ---- the running app ----
 *
 * Aroma starts a .wuhb through its module homebrew_rpx_loader
 * (github.com/wiiu-env/RPXLoadingModule), which keeps the file open while
 * the app runs. The module exports what an updater needs; they are looked up
 * here by name, as its client library does. Each returns 0 when it worked. */
static int (*rl_running_path)(char *out, uint32_t size); /* from the card's root: "wiiu/apps/our64.wuhb" */
static int (*rl_unmount)(void);                          /* lets go of the running .wuhb */
static int (*rl_launch)(const char *path);               /* starts a .wuhb as this app closes */

static int loader_find(void)
{
	OSDynLoad_Module module;
	if (OSDynLoad_Acquire("homebrew_rpx_loader", &module) != OS_DYNLOAD_OK)
		return 0;
	return OSDynLoad_FindExport(module, OS_DYNLOAD_EXPORT_FUNC, "RL_GetPathOfRunningExecutable", (void **)&rl_running_path) == OS_DYNLOAD_OK &&
	       OSDynLoad_FindExport(module, OS_DYNLOAD_EXPORT_FUNC, "RL_UnmountCurrentRunningBundle", (void **)&rl_unmount) == OS_DYNLOAD_OK &&
	       OSDynLoad_FindExport(module, OS_DYNLOAD_EXPORT_FUNC, "RL_LaunchHomebrew", (void **)&rl_launch) == OS_DYNLOAD_OK;
}

void updater_boot(const char *root)
{
	char path[340];
	snprintf(g_root, sizeof(g_root), "%s", root);
	g_loader = loader_find();
	if (g_loader && rl_running_path(g_self_rel, sizeof(g_self_rel)) == 0 && ends_with(g_self_rel, ".wuhb")) {
		snprintf(g_self, sizeof(g_self), "%s/%s", WHBGetSdCardMountPath(), g_self_rel + (g_self_rel[0] == '/'));
	} else {
		g_self[0] = 0;
	}
	report_line("UPDATE: running from %s%s", g_self[0] ? g_self_rel : "no .wuhb Our64 can replace",
	            g_loader ? "" : " (Aroma's loader not found)");
	if (g_self[0]) { /* the app an update replaced, when it could not be deleted while it ran */
		snprintf(path, sizeof(path), "%s.old", g_self);
		if (!remove(path))
			report_line("UPDATE: removed the app the last update replaced");
	}
	static const char *const left[] = { UPDATE_APP_NAME, UPDATE_INI_NAME, UPDATE_SHADERS_NAME };
	for (unsigned i = 0; i < sizeof(left) / sizeof(left[0]); i++) {
		path_in(path, sizeof(path), "update", left[i]);
		remove(path);
	}
}

static int copy_file(const char *from, const char *to)
{
	unsigned size = 0;
	unsigned char *data = read_whole(from, &size);
	if (!data)
		return -1;
	FILE *f = fopen(to, "wb");
	int ok = f != NULL && fwrite(data, 1, size, f) == size;
	if (f)
		ok = fclose(f) == 0 && ok;
	free(data);
	return ok ? 0 : -1;
}

int updater_install(void)
{
	char staged[320], old[340], path[320];
	struct stat st;
	path_in(staged, sizeof(staged), "update", UPDATE_APP_NAME);
	if (!g_self[0] || stat(staged, &st)) {
		report_line("UPDATE: not placed: %s", g_self[0] ? "the download is gone" : "Our64 was not started from a .wuhb");
		return UPDATE_NOT_PLACED;
	}
	/* Aroma's loader holds the running .wuhb open: every write or rename is
	 * refused as busy until it lets go (seen on the console, 2026-11-15). */
	const int un = rl_unmount();
	snprintf(old, sizeof(old), "%s.old", g_self);
	remove(old);
	errno = 0;
	if (rename(g_self, old)) {
		report_line("UPDATE: not placed: the running app can't be moved (errno %d, unmount %d)", errno, un);
		return UPDATE_NOT_PLACED;
	}
	if (rename(staged, g_self) && copy_file(staged, g_self)) {
		report_line("UPDATE: not placed: the new app can't be written (errno %d): the old one is back", errno);
		remove(g_self);
		rename(old, g_self);
		return UPDATE_NOT_PLACED;
	}
	remove(staged);
	if (remove(old))
		report_line("UPDATE: the old app stays as .old until the next start (errno %d)", errno);
	/* GLideN64.custom.ini follows the app */
	path_in(staged, sizeof(staged), "update", UPDATE_INI_NAME);
	if (!stat(staged, &st)) {
		snprintf(path, sizeof(path), "%s/%s", g_root, UPDATE_INI_NAME);
		remove(path);
		if (rename(staged, path))
			report_line("UPDATE: %s not replaced (errno %d)", UPDATE_INI_NAME, errno);
	}
	report_line("UPDATE: %s is in place at %s", g_want.version, g_self_rel);
	return UPDATE_OK;
}

int updater_launch(void)
{
	const int st = g_self[0] ? rl_launch(g_self_rel) : -1;
	report_line("UPDATE: starting %s: %d", g_self_rel, st);
	return st == 0 ? 0 : -1;
}

void updater_summary(int result, char *out, unsigned size)
{
	static const char *const words[] = {
		"",
		"Stopped.",
		"No internet connection. OUR-0411",
		"Couldn't reach GitHub. Try again later. OUR-0412",
		"Check the console's date and time. OUR-0413",
		"The download was damaged. Try again. OUR-0414",
		"Couldn't save to the SD card: check its lock switch or free space. OUR-0206",
		"Couldn't start. Close Our64 and try again. OUR-0415",
		"Couldn't replace Our64. Update from a computer. OUR-0416",
	};
	snprintf(out, size, "%s", result >= 0 && result < (int)(sizeof(words) / sizeof(words[0])) ? words[result] : "");
	if (result > UPDATE_STOPPED)
		report_line("ERROR update: %s", out);
}
