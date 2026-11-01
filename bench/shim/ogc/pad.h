/* Our64: no GameCube pads. The debug "press A to continue" loop in the
 * interpreter sees A pressed, then released, so it never blocks. */
#ifndef OUR64_PAD_H
#define OUR64_PAD_H
#include <gctypes.h>
#define PAD_BUTTON_A 0x0100
static inline u16 PAD_ButtonsHeld(int chan) { static int t; (void)chan; return (t ^= 1) ? PAD_BUTTON_A : 0; }
#endif
