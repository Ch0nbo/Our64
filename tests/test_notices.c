/* PC tests for player/source/notices.c: the controller notices over the game. */
#include "notices.h"

#include <stdio.h>
#include <string.h>

static int g_failed;
#define CHECK(c) do { if (!(c)) { printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); g_failed = 1; } } while (0)

#define S 1e6 /* microseconds */

static void test_shows_then_expires(void)
{
	notices q;
	const notice *v[NOTICES_SHOWN];
	notices_init(&q);
	CHECK(notices_visible(&q, 0, v) == 0);
	notices_add(&q, 1, NOTICE_INFO, "Player 2: Pro Controller", "", 3);
	CHECK(notices_visible(&q, 10 * S, v) == 1);
	CHECK(!strcmp(v[0]->line1, "Player 2: Pro Controller"));
	/* its 3 s start when it first shows, not when it was added */
	CHECK(notices_visible(&q, 12.9 * S, v) == 1);
	CHECK(notices_visible(&q, 13.1 * S, v) == 0);
}

static void test_oldest_first_and_waiting(void)
{
	notices q;
	const notice *v[NOTICES_SHOWN];
	notices_init(&q);
	notices_add(&q, 1, NOTICE_INFO, "one", "", 3);
	notices_add(&q, 2, NOTICE_INFO, "two", "", 3);
	notices_add(&q, 3, NOTICE_INFO, "three", "", 3);
	CHECK(notices_visible(&q, 0, v) == NOTICES_SHOWN);
	CHECK(!strcmp(v[0]->line1, "one") && !strcmp(v[1]->line1, "two"));
	/* "three" waits, and gets its full time once shown */
	CHECK(notices_visible(&q, 3.5 * S, v) == 1);
	CHECK(!strcmp(v[0]->line1, "three"));
	CHECK(notices_visible(&q, 6.4 * S, v) == 1);
	CHECK(notices_visible(&q, 6.6 * S, v) == 0);
}

static void test_same_key_replaces(void)
{
	notices q;
	const notice *v[NOTICES_SHOWN];
	notices_init(&q);
	notices_add(&q, 7, NOTICE_WARN, "Player 1's controller disconnected", "Turn it back on", -1);
	CHECK(notices_visible(&q, 0, v) == 1 && v[0]->kind == NOTICE_WARN);
	/* a standing notice stays */
	CHECK(notices_visible(&q, 100 * S, v) == 1);
	notices_add(&q, 7, NOTICE_INFO, "Player 1: Pro Controller is back", "", 3);
	CHECK(notices_visible(&q, 101 * S, v) == 1);
	CHECK(v[0]->kind == NOTICE_INFO && v[0]->line2[0] == 0);
	CHECK(notices_visible(&q, 104.1 * S, v) == 0); /* its time started again */
	notices_add(&q, 8, NOTICE_WARN, "standing", "", -1);
	notices_remove(&q, 8);
	CHECK(notices_visible(&q, 105 * S, v) == 0);
}

static void test_full_and_long_text(void)
{
	notices q;
	const notice *v[NOTICES_SHOWN];
	notices_init(&q);
	notices_add(&q, 100, NOTICE_WARN, "standing", "", -1);
	for (int i = 0; i < NOTICES_MAX + 3; i++)
		notices_add(&q, i, NOTICE_INFO, "timed", "", 3);
	CHECK(q.count == NOTICES_MAX);
	CHECK(q.n[0].key == 100); /* the standing one is never the one dropped */
	char long_text[300];
	memset(long_text, 'x', sizeof(long_text) - 1);
	long_text[sizeof(long_text) - 1] = 0;
	notices_init(&q);
	notices_add(&q, 1, NOTICE_INFO, long_text, long_text, 3);
	CHECK(notices_visible(&q, 0, v) == 1 && strlen(v[0]->line1) == NOTICE_TEXT - 1);
}

int main(void)
{
	test_shows_then_expires();
	test_oldest_first_and_waiting();
	test_same_key_replaces();
	test_full_and_long_text();
	if (g_failed)
		return 1;
	printf("  all passed\n");
	return 0;
}
