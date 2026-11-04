/* PC tests for player/source/covertitles_find.c (the No-Intro names by
 * header CRCs) and covers.c (cover file names, URLs and HTTP replies). */
#include "covertitles.h"

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

int main(void)
{
	test_title_by_header_crcs();
	test_table_is_sorted();
	if (!g_failed)
		printf("  all passed\n");
	return g_failed;
}
