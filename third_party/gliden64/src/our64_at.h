/* Our64: where the graphics core is, for the player's watchdog. Each mark
 * stores the file and line it is on; a hang shows the last one passed. */
#pragma once
#ifdef __cplusplus
extern "C" {
#endif
extern volatile const char *our64_at_file;
extern volatile int our64_at_line;
extern volatile unsigned our64_vi_w, our64_vi_h; /* the VI size VI_UpdateSize last worked out */
/* The last 8 marks (file, line), newest at our64_at_n - 1 (mod 8), and two
 * values the newest marks set by hand (FrameBufferList::saveBuffer: address,
 * width and size), for the watchdog (issue 43, Rayman 2, 2026-11-15). */
extern volatile const char *our64_at_files[8];
extern volatile int our64_at_lines[8];
extern volatile unsigned our64_at_n;
extern volatile unsigned our64_at_arg[2];
/* Non-zero while the graphics core waits for the GPU (GX2DrawDone,
 * GX2WaitTimeStamp): that wait's line, also the last mark. */
extern volatile int our64_gpu_wait_line;
#ifdef __cplusplus
}
#endif
#define OUR64_GPU_WAIT(_call) do { OUR64_AT(); our64_gpu_wait_line = __LINE__; _call; our64_gpu_wait_line = 0; } while (0)
#define OUR64_AT() (our64_at_file = __FILE__, our64_at_line = __LINE__, \
	our64_at_files[our64_at_n & 7] = __FILE__, our64_at_lines[our64_at_n & 7] = __LINE__, our64_at_n = our64_at_n + 1)
