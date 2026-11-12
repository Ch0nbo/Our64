/* The speed display's number (player/source/speed.h). */
#include "speed.h"
#include <stdio.h>
#include <string.h>

static int g_failed, g_run;
#define CHECK(x) do { if (!(x)) { g_failed++; printf("  FAILED %s:%d: %s\n", __FILE__, __LINE__, #x); } } while (0)
#define RUN(fn) do { g_run++; int before = g_failed; fn(); if (g_failed == before) printf("  ok   %s\n", #fn); } while (0)

/* A game run VI by VI: the clock, and the totals the player hands speed_vi. */
typedef struct {
	speed_meter m;
	double now;
	unsigned vis, frames, wrong, skipped, holds;
	int readings, lowest, last;
} run;

static void run_start(run *r)
{
	memset(r, 0, sizeof(*r));
	speed_restart(&r->m);
	r->now = 5e6;
	r->lowest = 1000;
	r->last = -1;
}

/* seconds of the game at vi_rate VIs a second, 30 game frames a second, of which wrong_share held wrong */
static void play(run *r, double seconds, double vi_rate, double wrong_share)
{
	const int n = (int)(seconds * vi_rate + 0.5);
	for (int i = 0; i < n; i++) {
		r->now += 1e6 / vi_rate;
		r->vis++;
		if (r->vis % 2 == 0) {
			r->frames++;
			if (wrong_share > 0 && (r->frames % (unsigned)(1.0 / wrong_share + 0.5)) == 0)
				r->wrong++;
		}
		if (speed_vi(&r->m, r->now, r->vis, r->frames, r->wrong, r->skipped, r->holds, 59.94)) {
			r->readings++;
			r->last = r->m.shown;
			if (r->m.shown < r->lowest)
				r->lowest = r->m.shown;
		}
	}
}

static void pause_for(run *r, double seconds)
{
	r->holds++;
	r->now += seconds * 1e6;
}

/* Zach, 2026-11-12: "starting a new game the speed goes from 30% to 100% over
 * 30 seconds or so slowly even when game is running 100%". A game opened from
 * its autosave runs 4 VIs and pauses; the first reading spanned that pause. */
static void the_opening_pause_is_not_in_the_first_reading(void)
{
	run r;
	run_start(&r);
	play(&r, 4 / 60.0, 60, 0);
	pause_for(&r, 0.6);
	play(&r, 0.9, 60, 0);
	CHECK(r.readings == 0 && r.m.shown < 0); /* nothing until a full second of the game */
	play(&r, 0.2, 60, 0);
	CHECK(r.readings == 1 && r.last >= 99 && r.last <= 101);
	play(&r, 5, 60, 0);
	CHECK(r.lowest >= 99);
}

static void a_pause_shorter_than_two_seconds_starts_over_too(void)
{
	run r;
	run_start(&r);
	play(&r, 3, 60, 0);
	pause_for(&r, 1.5);
	play(&r, 3, 60, 0);
	CHECK(r.lowest >= 99);
}

static void a_bad_stretch_stays_up_a_few_seconds_and_then_is_gone(void)
{
	run r;
	run_start(&r);
	play(&r, 4.5, 60, 0);
	CHECK(r.last >= 99);
	play(&r, 2, 30, 0); /* half speed for two seconds */
	CHECK(r.last >= 45 && r.last <= 55); /* seen at once */
	play(&r, 1.5, 60, 0);
	CHECK(r.last <= 80); /* still up */
	play(&r, 3.5, 60, 0);
	CHECK(r.last >= 99); /* and gone: 4 points a second would have it at about 70 */
}

static void frames_held_wrong_lower_it(void)
{
	run r;
	run_start(&r);
	play(&r, 4, 60, 0.5);
	CHECK(r.last >= 45 && r.last <= 55);
}

/* A filter compiling froze the TV's picture 1.7 s while the game ran on at 60 VIs a second: 99 VIs skipped. */
static void a_frozen_picture_lowers_it(void)
{
	run r;
	run_start(&r);
	play(&r, 3.5, 60, 0);
	CHECK(r.last >= 99);
	r.skipped += 99;
	play(&r, 1, 60, 0);
	CHECK(r.last == 0); /* a whole second's VIs never seen */
	play(&r, 1, 60, 0);
	CHECK(r.m.raw > 25 && r.m.raw < 45); /* the other 39 */
	play(&r, 1, 60, 0);
	CHECK(r.m.raw >= 99 && r.last == 0);
	play(&r, 3, 60, 0);
	CHECK(r.last >= 99);
}

static void a_long_gap_without_a_pause_starts_over(void)
{
	run r;
	run_start(&r);
	play(&r, 3, 60, 0);
	r.now += 5e6; /* a state loading */
	play(&r, 3, 60, 0);
	CHECK(r.lowest >= 99);
}

int main(void)
{
	RUN(the_opening_pause_is_not_in_the_first_reading);
	RUN(a_pause_shorter_than_two_seconds_starts_over_too);
	RUN(a_bad_stretch_stays_up_a_few_seconds_and_then_is_gone);
	RUN(frames_held_wrong_lower_it);
	RUN(a_frozen_picture_lowers_it);
	RUN(a_long_gap_without_a_pause_starts_over);
	printf("%d of %d failed\n", g_failed, g_run);
	return g_failed != 0;
}
