/* Our64 player: which Paks each game uses (gamedb.c, generated from
 * mupen64plus's ROM catalogue by tools/make-gamedb.py). Plain C. */
#ifndef OUR64_GAMEDB_H
#define OUR64_GAMEDB_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

enum { GAMEDB_RUMBLE = 1, GAMEDB_MEMPAK = 2, GAMEDB_TRANSFER = 4 };
int gamedb_paks(uint32_t crc1, uint32_t crc2); /* GAMEDB_ bits; 0 for a game not listed */
/* The catalogue's CountPerOp (Project64's counter factor): N64 cycles per
 * instruction for the game's timing, 1-4; 0 when not listed (use 2). */
int gamedb_count_per_op(uint32_t crc1, uint32_t crc2);

/* The Pak a player gets in this game (Zach, 2026-10-16: no Pak choice, a
 * rumble switch): a Rumble Pak when the game rumbles (swapped for a
 * Controller Pak when the game saves to one, plugins.c), else a Controller
 * Pak. The rumble switch only stops the motor (Zach, 2026-10-25): a game
 * looks for its Rumble Pak only now and then, so one turned on after that
 * look must still find the Pak there (Mickey's Speedway USA never rumbled). */
enum { PAK_CONTROLLER, PAK_RUMBLE };
int gamedb_pak_for(int paks);

#ifdef __cplusplus
}
#endif
#endif
