#include "profile.h"

#include <coreinit/alarm.h>
#include <coreinit/context.h>
#include <coreinit/time.h>

#include <cstdio>

#define M64P_PLUGIN_PROTOTYPES 1
#include "m64p_types.h"
#include "m64p_plugin.h"

namespace {

constexpr uint32_t kMaxSamples = 256 * 1024;
uint32_t g_samples[kMaxSamples][2];
volatile uint32_t g_count;
volatile bool g_active;
bool g_alarm_set;
OSAlarm g_alarm;

// Runs in the decrementer interrupt on the core the alarm was set on.
void sample(OSAlarm *, OSContext * context)
{
	const uint32_t n = g_count;
	if (g_active && n < kMaxSamples && context) {
		g_samples[n][0] = context->srr0;
		g_samples[n][1] = context->lr;
		g_count = n + 1;
	}
}

} // namespace

// One periodic alarm for the whole run, set on first use, with g_active
// gating the samples. Re-creating it per game (OSCreateAlarm on a queued
// alarm) left samples arriving between passes, and the console hung on the
// next launches until it was power-cycled.
void profile_start(uint32_t period_us)
{
	if (!g_alarm_set) {
		g_alarm_set = true;
		OSCreateAlarm(&g_alarm);
		const OSTime period = OSMicrosecondsToTicks(period_us);
		OSSetPeriodicAlarm(&g_alarm, OSGetTime() + period, period, sample);
	}
	g_active = true;
}

void profile_stop()
{
	g_active = false;
}

void profile_shutdown()
{
	g_active = false;
	if (g_alarm_set) {
		OSCancelAlarm(&g_alarm);
		g_alarm_set = false;
	}
}

bool profile_save(const char * path)
{
	FILE * f = fopen(path, "wb");
	if (!f) return false;
	const uint32_t header[4] = {0x4F363450, g_count, (uint32_t)(uintptr_t)&ProcessDList, 0}; // "O64P"
	fwrite(header, sizeof(header), 1, f);
	fwrite(g_samples, sizeof(g_samples[0]), g_count, f);
	fclose(f);
	g_count = 0;
	return true;
}
