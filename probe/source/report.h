#pragma once

// Results log: every line goes to the screen console (when one is up), to UDP
// (WHBLogUdp, port 4405) and into a buffer that Report_Save() writes to the SD card.

void Report_Line(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

// Writes the whole buffer to sd:/wiiu/our64/probe-results.txt. Safe to call
// after every section, so a crash later still leaves the earlier results.
bool Report_Save();

// Prints every buffered line again (used once the console is back after the GPU tests).
void Report_Replay();

const char *Report_Path();

// Microseconds since boot, from the system timer.
double Report_NowUs();
