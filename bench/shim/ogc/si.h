/* Our64: no GameCube serial bus on Wii U. SI_Transfer reports "not started". */
#ifndef OUR64_SI_H
#define OUR64_SI_H
#include <gctypes.h>
#define SI_ERROR_UNDER_RUN   0x0001
#define SI_ERROR_OVER_RUN    0x0002
#define SI_ERROR_COLLISION   0x0004
#define SI_ERROR_NO_RESPONSE 0x0008
static inline u32 SI_Busy(void) { return 0; }
static inline u32 SI_Sync(void) { return SI_ERROR_NO_RESPONSE; }
static inline u32 SI_Transfer(s32 chan, void *out, u32 outlen, void *in, u32 inlen, void *cb, u32 delay)
{
	(void)chan; (void)out; (void)outlen; (void)in; (void)inlen; (void)cb; (void)delay;
	return 0;
}
#endif
