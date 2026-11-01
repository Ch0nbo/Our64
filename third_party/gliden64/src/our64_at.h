/* Our64: where the graphics core is, for the player's watchdog. Each mark
 * stores the file and line it is on; a hang shows the last one passed. */
#pragma once
#ifdef __cplusplus
extern "C" {
#endif
extern volatile const char *our64_at_file;
extern volatile int our64_at_line;
extern volatile unsigned our64_vi_w, our64_vi_h; /* the VI size VI_UpdateSize last worked out */
#ifdef __cplusplus
}
#endif
#define OUR64_AT() (our64_at_file = __FILE__, our64_at_line = __LINE__)
