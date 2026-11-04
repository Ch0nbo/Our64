/* Our64 player: Aroma's Mocha, asked directly over IOS (mocha_wiiu.c). No
 * libmocha: its LGPL-3.0 does not mix with Our64's GPL-2.0-or-later. */
#ifndef OUR64_MOCHA_WIIU_H
#define OUR64_MOCHA_WIIU_H

#include <coreinit/filesystem_fsa.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 1 when Mocha answers its API version (1 or later) on /dev/mcp. */
int mocha_wiiu_present(void);
/* An FSA client that may open any title's files and the raw devices: Mocha
 * unlocks it. -1 when Mocha is not there or the client did not open; the
 * caller closes it with FSADelClient. */
FSAClientHandle mocha_wiiu_fsa_client(void);

#ifdef __cplusplus
}
#endif
#endif
