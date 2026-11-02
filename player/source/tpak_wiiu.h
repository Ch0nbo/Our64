/* Our64 player: the Game Boy cartridge for the Transfer Pak, from a Game Boy
 * game's RetroArch save, VBA GX save, Wii U Virtual Console save or OurGB save
 * (tpak_wiiu.c). */
#ifndef OUR64_TPAK_WIIU_H
#define OUR64_TPAK_WIIU_H
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* For a game that takes a Transfer Pak: finds a Game Boy ROM (in
 * <sd root>/gb, RetroArch's playlists and folders, VBA GX's ROM folders, or
 * OurConsoles' roms) whose save RetroArch, VBA GX, a Virtual Console inject
 * or OurGB keeps, and puts that cartridge in port 1's Transfer Pak.
 * n64_name is the ROM header's 20-byte name, crc1 its CRC1 (the Japan-only
 * games are known by it). 1 when a cartridge went in. */
int tpak_wiiu_start(const char *sd_root, const unsigned char *n64_name, uint32_t crc1);
/* The game ended: a save the N64 game changed goes back where it came from
 * (the old file copied to <sd root>/gb/backup first). */
void tpak_wiiu_stop(void);

#ifdef __cplusplus
}
#endif
#endif
