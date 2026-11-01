/* Our64 bench: counters the emulator hooks feed while a game runs. */
#ifndef OUR64_BENCH_H
#define OUR64_BENCH_H
#include <stdint.h>

typedef struct {
	uint32_t vis;            /* vertical interrupts emulated */
	uint32_t gfx_tasks;      /* graphics tasks the game sent (one per rendered frame) */
	uint32_t audio_tasks;
	uint32_t origin_changes; /* VI origin changes: frames the game actually presented */
	uint32_t last_origin;
	uint32_t stop_after_vis; /* 0 = no limit */
	double   deadline_us;    /* 0 = no limit */
	double   window_start_us;
	uint32_t window_start_vis, window_start_gfx;
	double   window_from_us; /* steady-state window starts at this time */
	int      hash_every;     /* 0 = off; else hash RDRAM every N VIs */
	uint32_t hashes[64];
	int      nhashes;
	double   section_us[16];
	uint32_t section_calls[16];
	uint32_t capture_from_vi;  /* 0 = off; else capture graphics tasks from this VI on */
	uint32_t capture_left;     /* tasks still to capture */
	int      capture_point;    /* index of the current capture point (input.h) */
	uint32_t captured, capture_errors;
} bench_counters;

extern bench_counters g_bench;
uint32_t bench_hash_rdram(void);
void bench_on_gfx_task(void);
#endif
