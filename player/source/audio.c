/* Our64 player: the game's audio through AX. Each AI DMA (16-bit stereo,
 * big-endian, left first: the same layout AX wants for one channel) is split
 * into two ring buffers, one per looping AX voice. AX resamples the N64 rate
 * to its 48 kHz output.
 *
 * The emulation thread writes; the AX frame callback (every 3 ms) follows
 * the voices' read position and stops them when the ring runs dry, so a slow
 * stretch is silence instead of the ring replaying old samples. While the
 * game sends audio, the emulation waits for room in the ring: the audio
 * clock paces the game.
 *
 * Smoothing (2026-10-26): Battle for Naboo at 55-59 VIs a second clicked.
 * The voices stopped under 6 ms left and started again only at 48 ms, a gap
 * of 40 ms or more with a hard cut at each end. Now the callback plays a
 * little slower as the ring drains (down to 97% near empty, up to 100.5%
 * when it is full), ramps the volume down over one AX frame before it stops
 * and up over one after a start, and a stopped voice starts at 24 ms. */
#include <coreinit/cache.h>
#include <coreinit/thread.h>
#include <coreinit/time.h>
#include <sndcore2/core.h>
#include <sndcore2/voice.h>

#include <string.h>

#include "player.h"

#define RING 16384          /* samples per channel, a power of two */
#define AHEAD_MS 96         /* the emulation waits while the ring holds more than this */
#define START_MS 24         /* a stopped voice starts once the ring holds this much */
#define FULL_MS 64          /* from here up the voices play at the game's rate (or a little over) */
#define VOLUME 0x8000
#define FRAME_SAMPLES 144   /* one AX frame (3 ms) at the 48 kHz output: a volume ramp's length */

static int16_t g_ring[2][RING] __attribute__((aligned(64)));
static AXVoice *g_voice[2];
static volatile uint32_t g_wr;  /* samples written, emulation thread */
static volatile uint32_t g_rd;  /* samples played, frame callback */
static uint32_t g_last_off;     /* the voices' read offset at the last callback */
static volatile int g_playing;
static unsigned g_rate = 32000;
static unsigned g_ahead, g_start, g_low, g_fade, g_full;
static float g_base_ratio;      /* the N64 rate over AX's */
static int g_adjust_pm;         /* the playback rate now, per mille off the game's */
static int g_lowest_pm, g_highest_pm;  /* since audio_smoothing last read them */
static unsigned g_dips;         /* volume ramped down and up again without a stop */
static int g_quiet;             /* the volume is ramped down (or the voices just started) */
static int g_starting;          /* the voices just started silent: their ramp up is no dip */
static int g_ramping;           /* a volume ramp was set last frame: its delta is cleared next */
static int32_t g_fill_avg;      /* the ring's fill, averaged, in 1/16 samples */
static OSTime g_last_push;
static unsigned g_last_count;   /* frames in the last push */
static int g_ready;
static volatile unsigned g_underruns;
static unsigned g_pushed, g_dropped;  /* stereo frames given to audio_push, and those thrown away */
static OSTime g_wait_ticks;
static int g_unpaced;           /* the TV clock paces the game (main.c): pushes never wait */

static void set_rate(unsigned rate)
{
	g_rate = rate;
	g_ahead = rate * AHEAD_MS / 1000;
	g_start = rate * START_MS / 1000;
	g_low = rate * 6 / 1000; /* two AX frames */
	g_fade = rate * 9 / 1000; /* one AX frame more: the ramp down runs in it */
	g_full = rate * FULL_MS / 1000;
	g_base_ratio = (float)rate / (float)AXGetInputSamplesPerSec();
	g_adjust_pm = 0;
	for (int c = 0; c < 2; c++) {
		AXVoiceBegin(g_voice[c]);
		AXSetVoiceSrcRatio(g_voice[c], g_base_ratio);
		AXVoiceEnd(g_voice[c]);
	}
}

/* The playback rate for this much in the ring, per mille off the game's:
 * -30 near empty, 0 at FULL_MS, +5 at AHEAD_MS (the emulation stops writing
 * there, so the ring sits near it while the game keeps up: a little over
 * drains the latency the waits built up). */
static int rate_for(uint32_t fill)
{
	if (fill >= g_ahead)
		return 5;
	if (fill >= g_full)
		return (int)((fill - g_full) * 5 / (g_ahead - g_full));
	if (fill <= g_low)
		return -30;
	return -30 + (int)((fill - g_low) * 30 / (g_full - g_low));
}

/* Both voices in one AX frame, so the channels stay together. The volume
 * ramps from one value to the other over the frame. */
static void set_ratio_and_volume(int adjust_pm, int volume_from, int volume_to)
{
	const float ratio = g_base_ratio * (1.0f + adjust_pm / 1000.0f);
	AXVoiceVeData ve = {.volume = (uint16_t)volume_from, .delta = (int16_t)((volume_to - volume_from) / FRAME_SAMPLES)};
	AXVoiceBegin(g_voice[0]);
	AXVoiceBegin(g_voice[1]);
	for (int c = 0; c < 2; c++) {
		AXSetVoiceSrcRatio(g_voice[c], ratio);
		AXSetVoiceVe(g_voice[c], &ve);
	}
	AXVoiceEnd(g_voice[1]);
	AXVoiceEnd(g_voice[0]);
}

static void frame_callback(void)
{
	/* Only this callback stops the voices and only the emulation thread
	 * starts them, each guarded by g_playing, so no lock is needed. */
	if (!g_playing)
		return;
	AXVoiceOffsets o;
	AXGetVoiceOffsets(g_voice[0], &o);
	g_rd += (o.currentOffset - g_last_off) & (RING - 1);
	g_last_off = o.currentOffset;
	const int32_t fill = (int32_t)(g_wr - g_rd);
	if (fill < (int32_t)g_low) {
		for (int c = 0; c < 2; c++)
			AXSetVoiceState(g_voice[c], AX_VOICE_STATE_STOPPED);
		g_rd = g_wr;
		g_playing = 0;
		g_underruns++;
		return;
	}
	/* Under g_fade the volume ramps to 0 over this frame, before the stop
	 * above; when the ring fills again it ramps back up (a dip, no gap).
	 * AX keeps adding a delta every frame, so the frame after a ramp sets its
	 * end value with none. */
	const int quiet = fill < (int32_t)g_fade;
	/* The rate follows the fill averaged over about 50 ms: the game writes in
	 * chunks of a frame's audio, and following each step would wobble the pitch. */
	g_fill_avg += (fill * 16 - g_fill_avg) / 16;
	const int adjust = rate_for((uint32_t)(g_fill_avg / 16));
	if (g_ramping && quiet == g_quiet) {
		set_ratio_and_volume(adjust, quiet ? 0 : VOLUME, quiet ? 0 : VOLUME);
		g_adjust_pm = adjust;
		g_ramping = 0;
	} else if (quiet != g_quiet) {
		g_ramping = 1;
		if (g_quiet && !g_starting)
			g_dips++;
		g_starting = 0;
		set_ratio_and_volume(adjust, g_quiet ? 0 : VOLUME, quiet ? 0 : VOLUME);
		g_quiet = quiet;
		g_adjust_pm = adjust;
	} else if (adjust != g_adjust_pm) {
		set_ratio_and_volume(adjust, quiet ? 0 : VOLUME, quiet ? 0 : VOLUME);
		g_adjust_pm = adjust;
	}
	if (adjust < g_lowest_pm)
		g_lowest_pm = adjust;
	if (adjust > g_highest_pm)
		g_highest_pm = adjust;
}

int audio_start(void)
{
	AXInitParams init = {.renderer = AX_INIT_RENDERER_48KHZ, .pipeline = AX_INIT_PIPELINE_SINGLE};
	AXInitWithParams(&init);
	if (!AXIsInit())
		return -1;
	memset(g_ring, 0, sizeof(g_ring));
	DCFlushRange(g_ring, sizeof(g_ring));

	for (int c = 0; c < 2; c++) {
		AXVoice *v = g_voice[c] = AXAcquireVoice(31, NULL, NULL);
		if (!v)
			return -1;
		AXVoiceBegin(v);
		AXSetVoiceType(v, 0);
		AXVoiceVeData ve = {.volume = VOLUME};
		AXSetVoiceVe(v, &ve);
		/* One device channel each: TV has 6 (L R ...), the GamePad 4 (L R ...). */
		AXVoiceDeviceMixData tv[6], drc[4];
		memset(tv, 0, sizeof(tv));
		memset(drc, 0, sizeof(drc));
		tv[c].bus[0].volume = 0x8000;
		drc[c].bus[0].volume = 0x8000;
		AXSetVoiceDeviceMix(v, AX_DEVICE_TYPE_TV, 0, tv);
		AXSetVoiceDeviceMix(v, AX_DEVICE_TYPE_DRC, 0, drc);
		AXVoiceOffsets o = {
			.dataType = AX_VOICE_FORMAT_LPCM16,
			.loopingEnabled = AX_VOICE_LOOP_ENABLED,
			.loopOffset = 0,
			.endOffset = RING - 1,
			.currentOffset = 0,
			.data = g_ring[c],
		};
		AXSetVoiceOffsets(v, &o);
		AXSetVoiceSrcType(v, AX_VOICE_SRC_TYPE_LINEAR);
		AXSetVoiceState(v, AX_VOICE_STATE_STOPPED);
		AXVoiceEnd(v);
	}
	set_rate(g_rate);
	AXRegisterAppFrameCallback(frame_callback);
	g_ready = 1;
	return 0;
}

/* Which speakers play: the TV's, the GamePad's, or both (the screen mode). */
void audio_route(int tv, int drc)
{
	if (!g_ready)
		return;
	for (int c = 0; c < 2; c++) {
		AXVoiceDeviceMixData mix_tv[6], mix_drc[4];
		memset(mix_tv, 0, sizeof(mix_tv));
		memset(mix_drc, 0, sizeof(mix_drc));
		mix_tv[c].bus[0].volume = tv ? 0x8000 : 0;
		mix_drc[c].bus[0].volume = drc ? 0x8000 : 0;
		AXVoiceBegin(g_voice[c]);
		AXSetVoiceDeviceMix(g_voice[c], AX_DEVICE_TYPE_TV, 0, mix_tv);
		AXSetVoiceDeviceMix(g_voice[c], AX_DEVICE_TYPE_DRC, 0, mix_drc);
		AXVoiceEnd(g_voice[c]);
	}
}

void audio_stop(void)
{
	if (!g_ready)
		return;
	g_ready = 0;
	AXDeregisterAppFrameCallback(frame_callback);
	for (int c = 0; c < 2; c++) {
		AXSetVoiceState(g_voice[c], AX_VOICE_STATE_STOPPED);
		AXFreeVoice(g_voice[c]);
	}
	AXQuit();
}

void audio_set_rate(unsigned rate)
{
	if (g_ready && rate && rate != g_rate)
		set_rate(rate);
}

void audio_push(const uint32_t *frames, unsigned count)
{
	if (!g_ready || !count)
		return;
	g_last_push = OSGetSystemTime();
	g_last_count = count;

	/* Wait for room: this is what paces the game. A game whose buffers are
	 * longer than AHEAD_MS (Stunt Racer: 4096 frames, 186 ms) could never fit
	 * one under it, so each push waited the whole 200 ms and the game ran at
	 * 45-48 VI/s in bursts (2026-10-28). Such a buffer goes in once the ring
	 * is down to FULL_MS, as the N64's two-buffer AI queue would take it. */
	const uint32_t limit = count + g_full > g_ahead ? count + g_ahead : g_ahead;
	OSTime give_up = g_last_push + OSMillisecondsToTicks(200), now = g_last_push;
	while (!g_unpaced && g_playing && g_wr - g_rd + count > limit && now < give_up) {
		OSSleepTicks(OSMicrosecondsToTicks(500));
		now = OSGetSystemTime();
	}
	g_wait_ticks += now - g_last_push;
	if (now != g_last_push)
		timeline_note('W', (unsigned long)OSTicksToMicroseconds(now - g_last_push), count, g_wr - g_rd, g_ahead);
	g_pushed += count;
	if (count > RING - g_ahead || g_wr - g_rd + count > RING) {
		g_dropped += count;
		return; /* would overwrite samples not played yet */
	}

	uint32_t at = g_wr & (RING - 1);
	for (unsigned i = 0; i < count; i++) {
		uint32_t f = frames[i];
		g_ring[0][(at + i) & (RING - 1)] = (int16_t)(f >> 16);
		g_ring[1][(at + i) & (RING - 1)] = (int16_t)f;
	}
	unsigned first = count < RING - at ? count : RING - at;
	for (int c = 0; c < 2; c++) {
		DCStoreRangeNoSync(&g_ring[c][at], first * 2);
		if (first < count)
			DCStoreRangeNoSync(&g_ring[c][0], (count - first) * 2);
	}
	__asm__ volatile("sync" ::: "memory");

	g_wr += count;
	/* A game that sends a list's audio every second VI (Perfect Dark, Banjo-
	 * Tooie: 33 ms at a time) ran dry again before its next push when the
	 * voices restarted at 24 ms (2026-10-27): they wait for a push and a half. */
	if (!g_playing && g_wr - g_rd >= g_start && g_wr - g_rd >= count + count / 2) {
		/* Both voices change in the same AX frame, so the channels stay
		 * together. They start silent; the next frame callback ramps them up. */
		g_last_off = g_rd & (RING - 1);
		AXVoiceVeData ve = {.volume = 0, .delta = 0};
		AXVoiceBegin(g_voice[0]);
		AXVoiceBegin(g_voice[1]);
		for (int c = 0; c < 2; c++) {
			AXSetVoiceVe(g_voice[c], &ve);
			AXSetVoiceCurrentOffset(g_voice[c], g_last_off);
			AXSetVoiceState(g_voice[c], AX_VOICE_STATE_PLAYING);
		}
		AXVoiceEnd(g_voice[1]);
		AXVoiceEnd(g_voice[0]);
		g_quiet = 1;
		g_starting = 1;
		g_ramping = 0;
		g_fill_avg = (int32_t)(g_wr - g_rd) * 16;
		g_playing = 1;
	}
}

unsigned audio_underruns(void) { return g_underruns; }

/* The playback rate's range since the last call (per mille off the game's),
 * and volume dips that ended without a stop. */
void audio_smoothing(int *lowest_pm, int *highest_pm, unsigned *dips)
{
	*lowest_pm = g_lowest_pm;
	*highest_pm = g_highest_pm;
	*dips = g_dips;
	g_lowest_pm = g_highest_pm = g_adjust_pm;
}

void audio_debug(unsigned *pushed, unsigned *dropped, unsigned *rate, int *playing)
{
	*pushed = g_pushed;
	*dropped = g_dropped;
	*rate = g_rate;
	*playing = g_playing;
}
double audio_wait_us(void) { return (double)OSTicksToMicroseconds(g_wait_ticks); }

/* Audio sets the pace while the game sends it in pieces of a few VIs. Longer
 * buffers (Stunt Racer: 186 ms) leave the VI clock to pace: waiting on each
 * made the game run 10 VIs fast, then stop for 50-200 ms (timeline,
 * 2026-10-28), and the ring holds several such buffers. */
/* The ring holds FULL_MS or more: the VI clock then keeps real time (main.c new_vi). */
int audio_ring_full(void)
{
	return g_playing && g_wr - g_rd >= g_full;
}

/* The TV clock paces the game (sd:/wiiu/our64/tv-clock): audio follows, the
 * playback rate keeping the ring's fill (rate_for). */
void audio_set_unpaced(int on) { g_unpaced = on; }

int audio_paces(void)
{
	return !g_unpaced && g_ready && OSGetSystemTime() - g_last_push < OSMillisecondsToTicks(100) && g_last_count <= g_rate / 20;
}
