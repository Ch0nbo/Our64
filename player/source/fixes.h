/* Our64 player: Game options > Fixes (2026-11-13). Our64 finds a game's fixes
 * by its ROM header: the name (GLideN64's per-game hacks, Our64's shader and
 * CPU choices), the CRCs (gamedb.c's Paks and counter factor, the boot
 * patches) and the game code (Not64's EEPROM size). A ROM hack has a header
 * of its own, so it often misses the fixes of the game it is made from. With
 * a game picked in Fixes, that game's header stands in for the ROM's own
 * wherever a fix is looked up. Saves, save states, shader files and the name
 * shown stay the ROM's own. Kept in game-options.txt as fixes=<game code>.
 * Plain C: tests/ runs it. */
#ifndef OUR64_FIXES_H
#define OUR64_FIXES_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
	const char *title; /* as the Fixes page lists it, A to Z */
	const char *code;  /* the game code at 0x3B-0x3E ("NZLE"): what game-options.txt keeps */
	const char *name;  /* the header's name at 0x20, as the fixes compare it */
	uint32_t crc1, crc2;
} fixes_game;

int fixes_count(void);
const fixes_game *fixes_at(int i);      /* NULL past the end */
int fixes_find(const char *code);       /* the game's index, -1 for none */

/* The header the fixes of the game being started are looked up by. */
typedef struct {
	char name[21];     /* 20 bytes as in a header, spaces after the name; 0 at the end */
	uint32_t crc1, crc2;
	uint16_t cart_id;  /* 0x3C-0x3D, big-endian as Not64's ROM_HEADER.Cartridge_ID */
	uint8_t country;   /* 0x3E */
	int picked;        /* the index of the game picked in Fixes, -1: the ROM's own */
} fixes_header;

/* The ROM's own header, or the one of the game `code` names (NULL or unknown: the ROM's own). */
void fixes_start(const unsigned char *name20, uint32_t crc1, uint32_t crc2, uint16_t cart_id, uint8_t country,
                 const char *code);
const fixes_header *fixes_now(void);

#ifdef __cplusplus
}
#endif
#endif
