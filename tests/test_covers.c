/* PC tests for player/source/covertitles_find.c (the No-Intro names by
 * header CRCs) and covers.c (cover file names, URLs and HTTP replies). */
#include "covertitles.h"
#include "covers.h"
#include "version.h"

#include <stdio.h>
#include <string.h>

static int g_failed;
#define CHECK(c) do { if (!(c)) { printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); g_failed = 1; } } while (0)

static void test_title_by_header_crcs(void)
{
	const char *t = covertitles_find(0x635A2BFF, 0x8B022326); /* mupen64plus.ini: Super Mario 64 (U) [!] */
	CHECK(t && !strcmp(t, "Super Mario 64 (USA)"));
	CHECK(covertitles_find(0x12345678, 0x9ABCDEF0) == NULL);
	CHECK(covertitles_find(0, 0) == NULL);
	CHECK(covertitles_find(0xFFFFFFFF, 0xFFFFFFFF) == NULL);
}

static void test_table_is_sorted(void)
{
	CHECK(covertitles_count > 0);
	for (unsigned i = 1; i < covertitles_count; i++) {
		const covertitle *a = &covertitles[i - 1], *b = &covertitles[i];
		CHECK(a->crc1 < b->crc1 || (a->crc1 == b->crc1 && a->crc2 < b->crc2));
	}
	/* every row is found by its own pair: the first, the last and all between */
	for (unsigned i = 0; i < covertitles_count; i++)
		CHECK(covertitles_find(covertitles[i].crc1, covertitles[i].crc2) == covertitles[i].name);
}

static void test_thumb_name(void)
{
	char out[64];
	covers_thumb_name("Mario & Luigi: A/B?", out, sizeof out);
	CHECK(!strcmp(out, "Mario _ Luigi_ A_B_"));
	/* RetroArch's set (gfx_thumbnail.c): & * / : ` " < > ? \ | */
	covers_thumb_name("&*/:`\"<>?\\|", out, sizeof out);
	CHECK(!strcmp(out, "___________"));
	covers_thumb_name("Banjo-Kazooie (USA) (Rev 1)", out, sizeof out);
	CHECK(!strcmp(out, "Banjo-Kazooie (USA) (Rev 1)"));
	covers_thumb_name("Pok\xc3\xa9mon Snap (USA)", out, sizeof out); /* UTF-8 passes through */
	CHECK(!strcmp(out, "Pok\xc3\xa9mon Snap (USA)"));
}

static void test_path(void)
{
	char out[256];
	covers_path("Super Mario 64 (USA)", out, sizeof out);
	CHECK(!strcmp(out, "/Nintendo%20-%20Nintendo%2064/Named_Boxarts/Super%20Mario%2064%20%28USA%29.png"));
	covers_path("A-Z_a.z~09", out, sizeof out); /* the unreserved set stays */
	CHECK(!strcmp(out, "/Nintendo%20-%20Nintendo%2064/Named_Boxarts/A-Z_a.z~09.png"));
	covers_path("Pok\xc3\xa9mon, Mario's +!", out, sizeof out); /* UTF-8 bytes each as %XX */
	CHECK(!strcmp(out, "/Nintendo%20-%20Nintendo%2064/Named_Boxarts/Pok%C3%A9mon%2C%20Mario%27s%20%2B%21.png"));
}

static void test_request(void)
{
	char out[512];
	int n = covers_request("/Nintendo%20-%20Nintendo%2064/Named_Boxarts/X.png", out, sizeof out);
	CHECK(n == (int)strlen(out));
	CHECK(!strncmp(out, "GET /Nintendo%20-%20Nintendo%2064/Named_Boxarts/X.png HTTP/1.0\r\n", 64));
	CHECK(strstr(out, "\r\nHost: thumbnails.libretro.com\r\n") != NULL);
	CHECK(strstr(out, "\r\nUser-Agent: Our64/" OUR64_VERSION "\r\n") != NULL);
	CHECK(strstr(out, "\r\nConnection: close\r\n") != NULL);
	CHECK(n > 4 && !strcmp(out + n - 4, "\r\n\r\n"));
	CHECK(strstr(out, "\r\n\r\n") == out + n - 4); /* the only blank line is the last */
}

static int reply(const char *r, unsigned *body, unsigned *body_n)
{
	*body = *body_n = 12345;
	return covers_reply((const unsigned char *)r, (unsigned)strlen(r), body, body_n);
}

static void test_reply(void)
{
	unsigned body, body_n;
	const char *ok = "HTTP/1.1 200 OK\r\nContent-Type: image/png\r\n\r\n\x89PNG\r\n\x1a\n";
	CHECK(reply(ok, &body, &body_n) == 200);
	CHECK(body == (unsigned)(strstr(ok, "\x89PNG") - ok) && body_n == 8);
	CHECK(reply("HTTP/1.1 404 Not Found\r\nContent-Length: 9\r\n\r\nnot found", &body, &body_n) == 404);
	CHECK(reply("HTTP/1.0 503 Service Unavailable\r\n\r\n", &body, &body_n) == 503);
	CHECK(reply("garbage", &body, &body_n) == -1);
	CHECK(reply("", &body, &body_n) == -1);
	CHECK(reply("HTTP/1.1 OK\r\n\r\nx", &body, &body_n) == -1);
	/* Content-Length is the body, whatever follows it */
	CHECK(reply("HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\n123456789", &body, &body_n) == 200);
	CHECK(body_n == 5);
	/* a cut-off download: less than Content-Length arrived */
	CHECK(reply("HTTP/1.1 200 OK\r\nContent-Length: 50\r\n\r\n123456789", &body, &body_n) == -1);
	CHECK(reply("HTTP/1.1 200 OK\r\nContent-Length: 99999999999999999999\r\n\r\n123", &body, &body_n) == -1);
	CHECK(reply("HTTP/1.1 200 OK\r\nContent-Length: five\r\n\r\n12345", &body, &body_n) == -1);
	/* header names in any case */
	CHECK(reply("HTTP/1.1 200 OK\r\ncontent-LENGTH:3\r\n\r\n123456789", &body, &body_n) == 200);
	CHECK(body_n == 3);
	/* the headers never ended */
	CHECK(reply("HTTP/1.1 200 OK\r\nContent-Type: image/png\r\n", &body, &body_n) == -1);
	CHECK(reply("HTTP/1.1 200 OK\r\nContent-Type: image/png\r\n\r", &body, &body_n) == -1);
	/* only the n bytes given are read: a reply cut inside its headers */
	CHECK(covers_reply((const unsigned char *)ok, 20, &body, &body_n) == -1);
}

static void test_file_title(void)
{
	char out[64];
	covers_file_title("Banjo-Kazooie (USA).z64", out, sizeof out);
	CHECK(!strcmp(out, "Banjo-Kazooie (USA)"));
	covers_file_title("/vol/external01/wiiu/our64/roms/Super Mario 64 (USA).n64", out, sizeof out);
	CHECK(!strcmp(out, "Super Mario 64 (USA)"));
	covers_file_title("roms/Dr. Mario 64 (USA).v1.z64", out, sizeof out); /* only the last extension */
	CHECK(!strcmp(out, "Dr. Mario 64 (USA).v1"));
	covers_file_title("NoExtension", out, sizeof out);
	CHECK(!strcmp(out, "NoExtension"));
	covers_file_title("roms.old/NoExtension", out, sizeof out); /* a dot in a folder is not an extension */
	CHECK(!strcmp(out, "NoExtension"));
}

/* Every output is NUL-terminated inside size; what does not fit gives "". */
static void test_small_buffers(void)
{
	char out[16];
	unsigned body, body_n;
	(void)body; (void)body_n;
	for (unsigned size = 0; size <= 8; size++) {
		memset(out, 'X', sizeof out);
		covers_thumb_name("Super Mario 64 (USA)", out, size);
		CHECK(out[size] == 'X' && (size == 0 || out[0] == 0));
		memset(out, 'X', sizeof out);
		covers_path("Super Mario 64 (USA)", out, size);
		CHECK(out[size] == 'X' && (size == 0 || out[0] == 0));
		memset(out, 'X', sizeof out);
		CHECK(covers_request("/x.png", out, size) == -1);
		CHECK(out[size] == 'X' && (size == 0 || out[0] == 0));
		memset(out, 'X', sizeof out);
		covers_file_title("dir/Super Mario 64 (USA).z64", out, size);
		CHECK(out[size] == 'X' && (size == 0 || out[0] == 0));
	}
	/* exactly fitting */
	char fit[5];
	covers_thumb_name("a:b?", fit, sizeof fit);
	CHECK(!strcmp(fit, "a_b_"));
	covers_file_title("abcd.z64", fit, sizeof fit);
	CHECK(!strcmp(fit, "abcd"));
	covers_file_title("abcde.z64", fit, sizeof fit);
	CHECK(fit[0] == 0);
	char path[64];
	covers_path("A", path, sizeof "/Nintendo%20-%20Nintendo%2064/Named_Boxarts/A.png");
	CHECK(!strcmp(path, "/Nintendo%20-%20Nintendo%2064/Named_Boxarts/A.png"));
	covers_path("&", path, sizeof "/Nintendo%20-%20Nintendo%2064/Named_Boxarts/A.png"); /* %26 is 2 more */
	CHECK(path[0] == 0);
}

int main(void)
{
	test_title_by_header_crcs();
	test_table_is_sorted();
	test_thumb_name();
	test_path();
	test_request();
	test_reply();
	test_file_title();
	test_small_buffers();
	if (!g_failed)
		printf("  all passed\n");
	return g_failed;
}
