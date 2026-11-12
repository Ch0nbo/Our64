/* Our64 player: the speed display's number (Settings > Picture; main.c's
 * speed_tick feeds it, tests/test_speed.c checks it). Our own code.
 *
 * Once a second, one percentage for how the game is running, three things
 * multiplied:
 * - speed: VIs emulated against the game's own 59.94 or 50 a second;
 * - frames held right: the share of the last two seconds' game frames that
 *   stayed on the TV as long as the game meant (speed alone read 98-101% in
 *   GoldenEye at 720p while a third of its frames were held wrong, Zach,
 *   2026-11-11);
 * - VIs shown: the share of this second's VIs the TV did not skip (a picture
 *   frozen 1.7 s while a filter compiled read 100%: the game ran on behind
 *   it, 2026-11-12). Skips are counted when the picture comes back, so what
 *   one second cannot take goes to the next.
 * The number shown is the lowest of the last SPEED_HOLD readings: a bad
 * second stays up long enough to be seen and is gone after (until 2026-11-12
 * it climbed back 4 points a second, so a game opened from its autosave,
 * whose first second held the opening pause, took 15-25 s to reach 100%).
 * A pause (holds) or a gap of over two seconds starts over, and the first
 * reading after it is of a full second of the game running. */
#pragma once

enum { SPEED_HOLD = 3 };

typedef struct {
	double t0; /* us: the reading under way began here; 0: start over at the next VI */
	unsigned vis0, frames0, wrong0, skipped0, holds0;
	unsigned frames_prev, wrong_prev, skip_carry;
	double recent[SPEED_HOLD];
	int n;     /* readings in recent */
	int shown; /* the number for the screen; below 0: none yet */
	/* the last reading's parts, for the log */
	double speed, raw;
	unsigned frames, wrong, skipped, vis;
} speed_meter;

static inline void speed_restart(speed_meter *m)
{
	m->t0 = 0;
	m->shown = -1;
}

/* One VI. now_us: the time; vis, frames, wrong, skipped: totals so far (VIs
 * emulated; game frames on the TV, those held wrong, VIs the TV skipped);
 * holds: pauses so far; full: the game's VIs a second. 1 when there is a new
 * reading (m->shown and the parts), else 0. */
static inline int speed_vi(speed_meter *m, double now_us, unsigned vis, unsigned frames, unsigned wrong,
                           unsigned skipped, unsigned holds, double full)
{
	if (m->t0 == 0 || holds != m->holds0 || now_us - m->t0 > 2e6) {
		m->t0 = now_us;
		m->vis0 = vis;
		m->frames0 = frames;
		m->wrong0 = wrong;
		m->skipped0 = skipped;
		m->holds0 = holds;
		m->frames_prev = m->wrong_prev = m->skip_carry = 0;
		m->n = 0;
		return 0;
	}
	if (now_us - m->t0 < 1e6)
		return 0;
	const unsigned n = vis - m->vis0, f = frames - m->frames0, w = wrong - m->wrong0;
	const unsigned f2 = f + m->frames_prev, w2 = w + m->wrong_prev; /* this second and the one before */
	unsigned sk = skipped - m->skipped0 + m->skip_carry;
	m->skip_carry = sk > n ? sk - n : 0;
	if (sk > n)
		sk = n;
	m->speed = n * 1e6 / (now_us - m->t0) * 100.0 / full;
	m->raw = m->speed;
	if (f2 > 0 && w2 <= f2)
		m->raw *= 1.0 - (double)w2 / (double)f2;
	if (n > 0)
		m->raw *= 1.0 - (double)sk / (double)n;
	m->frames = f;
	m->wrong = w;
	m->skipped = sk;
	m->vis = n;
	if (m->n < SPEED_HOLD)
		m->n++;
	for (int i = m->n - 1; i > 0; i--)
		m->recent[i] = m->recent[i - 1];
	m->recent[0] = m->raw;
	double low = m->recent[0];
	for (int i = 1; i < m->n; i++)
		if (m->recent[i] < low)
			low = m->recent[i];
	m->shown = (int)(low + 0.5);
	m->frames_prev = f;
	m->wrong_prev = w;
	m->t0 = now_us;
	m->vis0 = vis;
	m->frames0 = frames;
	m->wrong0 = wrong;
	m->skipped0 = skipped;
	return 1;
}
