/* PC tests for player/source/update.c: version order, GitHub's release
 * answer, shaders.zip, and merging a release's shader packs and keys into
 * the card's. */
#include "update.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

static int g_failed, g_run;

#define CHECK(cond) do { if (!(cond)) { printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); g_failed++; return; } } while (0)

static void versions_order_by_number(void)
{
	CHECK(update_version_cmp("1.2.0", "1.2.1") < 0 && update_version_cmp("1.2.1", "1.2.0") > 0);
	CHECK(update_version_cmp("1.9.0", "1.10.0") < 0); /* not by letters */
	CHECK(update_version_cmp("v1.3.0", "1.3.0") == 0 && update_version_cmp("1.3", "1.3.0") == 0);
	CHECK(update_version_cmp("1.2.1-test", "1.2.1") == 0 && update_version_cmp("1.2.1-test", "1.2.0") > 0);
	CHECK(update_version_cmp("2.0.0", "1.99.99") > 0);
}

/* GitHub's shape, cut down: an uploader object inside each asset, a body with quotes and braces. */
static const char *const RELEASE =
	"{\"url\":\"https://api.github.com/repos/o/r/releases/1\",\"tag_name\":\"v1.3.0\",\"name\":\"Our64 1.3.0\","
	"\"author\":{\"login\":\"x\",\"id\":1,\"site_admin\":false},\"prerelease\":false,"
	"\"assets\":["
	"{\"url\":\"u\",\"id\":5,\"name\":\"Our64-1.3.0.zip\",\"label\":null,\"uploader\":{\"login\":\"x\",\"id\":1},"
	"\"size\":28571863,\"digest\":\"sha256:f6fbff6ea8b0d4b183ad45dfcbf8d68e645fc438636ed3312be7ce4ff10047d7\","
	"\"browser_download_url\":\"https://github.com/o/r/releases/download/v1.3.0/Our64-1.3.0.zip\"},"
	"{\"url\":\"u\",\"id\":6,\"name\":\"our64.wuhb\",\"label\":\"\",\"uploader\":{\"login\":\"x\",\"id\":1},"
	"\"size\":5035712,\"digest\":\"sha256:AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA\","
	"\"browser_download_url\":\"https://github.com/o/r/releases/download/v1.3.0/our64.wuhb\"},"
	"{\"name\":\"shaders.zip\",\"size\":12000000,\"digest\":null,"
	"\"browser_download_url\":\"https://github.com/o/r/releases/download/v1.3.0/shaders.zip\"}"
	"],\"body\":\"New\\r\\n\\\"quoted\\\" {not json] \\u00e9\"}";

static void release_gives_the_tag_and_our_files(void)
{
	update_release r;
	CHECK(update_parse_release(RELEASE, &r) == 0);
	CHECK(!strcmp(r.version, "1.3.0"));
	CHECK(!strcmp(r.app.url, "https://github.com/o/r/releases/download/v1.3.0/our64.wuhb") && r.app.size == 5035712);
	CHECK(!strcmp(r.app.sha256, "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa")); /* lower case */
	CHECK(r.ini.url[0] == 0); /* not in this release */
	CHECK(r.shaders.size == 12000000 && r.shaders.sha256[0] == 0 && strstr(r.shaders.url, "shaders.zip"));
}

static void release_refuses_what_is_not_one(void)
{
	update_release r;
	CHECK(update_parse_release("", &r) < 0);
	CHECK(update_parse_release("<html>rate limit</html>", &r) < 0);
	CHECK(update_parse_release("{\"message\":\"Not Found\"}", &r) < 0);
	CHECK(update_parse_release("{\"tag_name\":\"v1.3.0\",\"assets\":[{\"name\":\"our64.wuhb\"", &r) < 0); /* cut short */
	CHECK(update_parse_release("{\"tag_name\":\"nightly\",\"assets\":[]}", &r) < 0);
	CHECK(update_parse_release("{\"tag_name\":\"v2.0.0\",\"assets\":[]}", &r) == 0 && r.app.url[0] == 0);
}

/* ---- a zip made here: one file stored, one deflated ---- */

static void w16(FILE *f, unsigned v) { fputc(v & 255, f), fputc(v >> 8 & 255, f); }
static void w32(FILE *f, unsigned v) { w16(f, v & 0xFFFF), w16(f, v >> 16); }

typedef struct { const char *name; const unsigned char *data; unsigned size; int deflate; unsigned offset, packed, crc; } zfile;

static void zip_write(FILE *f, zfile *files, int n)
{
	for (int i = 0; i < n; i++) {
		zfile *z = &files[i];
		unsigned char packed[4096];
		const unsigned char *bytes = z->data;
		z->packed = z->size;
		z->crc = (unsigned)crc32(0, z->data, z->size);
		if (z->deflate) {
			z_stream s;
			memset(&s, 0, sizeof(s));
			deflateInit2(&s, 9, Z_DEFLATED, -15, 8, Z_DEFAULT_STRATEGY);
			s.next_in = (unsigned char *)z->data, s.avail_in = z->size;
			s.next_out = packed, s.avail_out = sizeof(packed);
			deflate(&s, Z_FINISH);
			z->packed = (unsigned)s.total_out;
			deflateEnd(&s);
			bytes = packed;
		}
		z->offset = (unsigned)ftell(f);
		w32(f, 0x04034b50), w16(f, 20), w16(f, 0), w16(f, z->deflate ? 8 : 0), w16(f, 0), w16(f, 0);
		w32(f, z->crc), w32(f, z->packed), w32(f, z->size), w16(f, (unsigned)strlen(z->name)), w16(f, 0);
		fputs(z->name, f);
		fwrite(bytes, 1, z->packed, f);
	}
	const unsigned dir = (unsigned)ftell(f);
	for (int i = 0; i < n; i++) {
		zfile *z = &files[i];
		w32(f, 0x02014b50), w16(f, 20), w16(f, 20), w16(f, 0), w16(f, z->deflate ? 8 : 0), w16(f, 0), w16(f, 0);
		w32(f, z->crc), w32(f, z->packed), w32(f, z->size), w16(f, (unsigned)strlen(z->name));
		w16(f, 0), w16(f, 0), w16(f, 0), w16(f, 0), w32(f, 0), w32(f, z->offset);
		fputs(z->name, f);
	}
	const unsigned end = (unsigned)ftell(f);
	w32(f, 0x06054b50), w16(f, 0), w16(f, 0), w16(f, (unsigned)n), w16(f, (unsigned)n), w32(f, end - dir), w32(f, dir), w16(f, 0);
}

static void zip_lists_and_unpacks(void)
{
	unsigned char big[3000];
	for (unsigned i = 0; i < sizeof(big); i++)
		big[i] = (unsigned char)(i % 7);
	zfile files[2] = { { "games/A.keys", (const unsigned char *)"0x01\n", 5, 0, 0, 0, 0 }, { "games/A.pack", big, sizeof(big), 1, 0, 0, 0 } };
	FILE *f = tmpfile();
	CHECK(f);
	zip_write(f, files, 2);
	update_zip z;
	update_zip_entry e;
	CHECK(update_zip_open(f, &z) == 0 && z.count == 2);
	CHECK(update_zip_next(&z, &e) && !strcmp(e.name, "games/A.keys") && e.size == 5);
	char *keys = update_zip_read(f, &e);
	CHECK(keys && !memcmp(keys, "0x01\n", 5));
	free(keys);
	CHECK(update_zip_next(&z, &e) && !strcmp(e.name, "games/A.pack") && e.size == sizeof(big) && e.packed < e.size);
	unsigned char *pack = update_zip_read(f, &e);
	CHECK(pack && !memcmp(pack, big, sizeof(big)));
	free(pack);
	e.crc ^= 1; /* a damaged file is refused */
	CHECK(update_zip_read(f, &e) == NULL);
	CHECK(!update_zip_next(&z, &e));
	update_zip_close(&z);
	fclose(f);
}

static void zip_refuses_other_files(void)
{
	FILE *f = tmpfile();
	CHECK(f);
	fputs("this is not a zip file, it is a page saying the download moved", f);
	update_zip z;
	CHECK(update_zip_open(f, &z) < 0);
	fclose(f);
}

/* ---- packs ---- */

/* A pack of pairs, each 20 bytes: 8 of header, its key, 4 of body (the tag). */
static unsigned pack_make(unsigned char *out, const unsigned long long *keys, int n, unsigned char tag)
{
	unsigned at = 12;
	memcpy(out, "O64K", 4);
	out[4] = out[5] = out[6] = 0, out[7] = 1;
	out[8] = out[9] = out[10] = 0, out[11] = (unsigned char)n;
	for (int i = 0; i < n; i++) {
		out[at] = out[at + 1] = out[at + 2] = 0, out[at + 3] = 20;
		memset(out + at + 4, 0x55, 8);
		for (int b = 0; b < 8; b++)
			out[at + 12 + b] = (unsigned char)(keys[i] >> (56 - 8 * b));
		memset(out + at + 20, tag, 4);
		at += 24;
	}
	return at;
}

static void packs_merge_keeps_the_cards_own_pairs(void)
{
	unsigned char a[256], b[256];
	const unsigned long long nk[2] = { 0x1111, 0x2222 }, ok[3] = { 0x2222, 0x3333, 0x4444 };
	const unsigned an = pack_make(a, nk, 2, 'N'), bn = pack_make(b, ok, 3, 'O');
	unsigned n = 0;
	unsigned char *m = update_pack_merge(a, an, b, bn, &n);
	CHECK(m && n == 12 + 4 * 24 && m[11] == 4);
	CHECK(!memcmp(m + 12, a + 12, an - 12)); /* the release's pairs first, as they were */
	CHECK(m[an + 4 + 15] == 0x33 && m[an + 20] == 'O'); /* then the card's 3333 */
	CHECK(m[an + 24 + 4 + 15] == 0x44);
	/* 2222 is the release's, not the card's */
	CHECK(m[12 + 24 + 4 + 15] == 0x22 && m[12 + 24 + 20] == 'N');
	free(m);
	/* no pack on the card, or a damaged one: the release's */
	m = update_pack_merge(a, an, NULL, 0, &n);
	CHECK(m && n == an && !memcmp(m, a, an));
	free(m);
	m = update_pack_merge(a, an, b, bn - 3, &n);
	CHECK(m && n == an);
	free(m);
	/* what the release sent is not a pack: nothing written */
	CHECK(update_pack_merge((const unsigned char *)"<html>", 6, b, bn, &n) == NULL);
	a[11] = 9; /* says 9 pairs, holds 2 */
	CHECK(update_pack_merge(a, an, b, bn, &n) == NULL);
}

static void keys_merge_is_every_key_once(void)
{
	unsigned n = 0;
	char *m = update_keys_merge("# Our64: combiner keys of X\n0x0000000000000002\n0x0000000000000005\n",
	                            "# old\n0x0000000000000005\n0x0000000000000001\n", &n);
	CHECK(m && !strcmp(m, "# Our64: combiner keys of X\n0x0000000000000001\n0x0000000000000002\n0x0000000000000005\n"));
	CHECK(n == strlen(m));
	free(m);
	m = update_keys_merge("0x000000000000000a\n", NULL, &n);
	CHECK(m && !strcmp(m, "0x000000000000000a\n"));
	free(m);
}

#define RUN(t) do { g_run++; t(); } while (0)

int main(void)
{
	RUN(versions_order_by_number);
	RUN(release_gives_the_tag_and_our_files);
	RUN(release_refuses_what_is_not_one);
	RUN(zip_lists_and_unpacks);
	RUN(zip_refuses_other_files);
	RUN(packs_merge_keeps_the_cards_own_pairs);
	RUN(keys_merge_is_every_key_once);
	printf("  %d tests, %d failed\n", g_run, g_failed);
	return g_failed != 0;
}
