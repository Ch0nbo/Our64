// Our64 replay app: a sampling profiler. A periodic alarm on the profiled
// thread's core records the PC and LR of the context it interrupted;
// profile_save writes them for tools/profile-report.py, which maps them to
// functions in the .elf. Cemu passes the alarm a dummy context, so profiles
// from Cemu are empty; suspending the thread from another core hung on the
// console (2026-10-12).
#pragma once
#include <coreinit/thread.h>
#include <cstdint>

// Samples the calling thread's core every period_us between start and
// stop. The alarm is set on the first call; later calls only resume sampling,
// so start/stop are cheap enough to wrap each ProcessDList call.
void profile_start(uint32_t period_us);
void profile_stop();
// Cancels the alarm; call before the app exits (a periodic alarm left queued
// is suspected of hanging later launches until a power cycle).
void profile_shutdown();
// Writes "O64P" format: magic, sample count, runtime address of ProcessDList
// (to check the load address), then (pc, lr) pairs as big-endian words.
bool profile_save(const char * path);
