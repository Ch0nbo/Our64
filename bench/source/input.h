/* Our64 bench: scripted controller input and capture points, per ROM, from
 * sd:/wiiu/our64/roms/<rom name without extension>.input. One command a line:
 *   mash FROM TO           press START, then A 30 VIs later, every 60 VIs (4 VIs each)
 *   press VI BUTTONS [LEN] hold BUTTONS (A,B,Z,START,L,R,CU,CD,CL,CR,DU,DD,DL,DR,
 *                          joined with '+') for LEN VIs (default 4)
 *   stick FROM TO X Y      hold the stick at X, Y (-80..80) from VI FROM to TO
 *   capture VI             save CAPTURE_TASKS graphics tasks from this VI on
 * Lines starting with '#' are comments. Without a script: no input, one
 * capture point at CAPTURE_AT_VI. */
#ifndef OUR64_INPUT_H
#define OUR64_INPUT_H
#include <stdint.h>

#define INPUT_MAX_CAPTURES 16

/* Loads the script for a ROM file path; returns the number of capture points
 * (at least one: the default when there is no script). */
int input_load(const char *sd_root, const char *rom_path, uint32_t default_capture_vi);
uint32_t input_capture_vi(int index);
/* The N64 controller state for VI `vi`: buttons in the high 16 bits, stick X
 * and Y in the low bytes (the BUTTONS.Value layout). */
uint32_t input_keys(uint32_t vi);
#endif
