/* Our64 bench: results log. Every line goes to the screen console, to UDP
 * (WHBLogUdp, port 4405) and into a buffer that report_save() writes to
 * sd:/wiiu/our64/bench-results.txt (REPORT_FILE). */
#ifndef OUR64_REPORT_H
#define OUR64_REPORT_H
#include <stdbool.h>
#include <stddef.h>

void report_line(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
bool report_save(void);
void report_continue(size_t keep); /* one file across starts, the oldest dropped past keep bytes */
void report_restart(void); /* saves, then empties the buffer; later saves append to the file (the sweep: no 256 KB cap) */
const char *report_sd_root(void); /* "<sd mount>/wiiu/our64", or NULL when no SD */
double report_now_us(void);
#endif
