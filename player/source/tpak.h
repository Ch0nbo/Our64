/* Our64 player: the Transfer Pak, the Game Boy cartridge in it, and Game Boy
 * saves kept by Goomba Color (the Game Boy emulator inside a Wii U Virtual
 * Console inject of a Game Boy game), RetroArch, VBA GX and OurGB. Plain C, so
 * tests/ runs it on the PC.
 *
 * The Transfer Pak follows mupen64plus (transferpak.c, gb_cart.c). */
#ifndef OUR64_TPAK_H
#define OUR64_TPAK_H
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- the Game Boy cartridge ---- */

enum { GB_NO_MBC, GB_MBC1, GB_MBC2, GB_MBC3, GB_MBC5 };

typedef struct {
	const uint8_t *rom;
	uint32_t rom_size;
	uint8_t *ram;      /* the cartridge's battery-backed RAM: the game's save */
	uint32_t ram_size;
	int mbc;           /* GB_ */
	unsigned rom_bank, ram_bank;
	int ram_enable, mbc1_mode;
	int ram_written;   /* the N64 game changed the save */
} gb_cart;

/* The header's title (0x134, 15 bytes at most, as Goomba Color names its saves). */
void gb_title(const uint8_t *rom, char out[16]);
/* The battery RAM the header asks for (0x149, MBC2's 512 bytes built in); 0 for none. */
uint32_t gb_ram_size(const uint8_t *rom);
/* 0, or -1 for a cartridge type Our64 has no mapper for (says which in *why). */
int gb_cart_init(gb_cart *c, const uint8_t *rom, uint32_t rom_size, uint8_t *ram, uint32_t ram_size, const char **why);
void gb_cart_read(gb_cart *c, uint16_t address, uint8_t *data, unsigned size);
void gb_cart_write(gb_cart *c, uint16_t address, const uint8_t *data, unsigned size);

/* ---- the Transfer Pak: 32-byte reads and writes at Pak addresses ---- */

typedef struct {
	int enabled;      /* 0x84 written at 0x8000 */
	unsigned bank;    /* which 16 KB of the cartridge 0xC000-0xFFFF shows */
	int cart_enabled; /* the access mode written at 0xB000 */
	int reset_state;
	gb_cart *cart;    /* NULL: no cartridge */
} tpak;

void tpak_power_on(tpak *t, gb_cart *cart);
void tpak_read(tpak *t, uint16_t address, uint8_t data[32]);
void tpak_write(tpak *t, uint16_t address, const uint8_t data[32]);

/* plugins.c: puts a cartridge in port 1's Transfer Pak, or takes it out (NULL). */
void player_tpak_insert(gb_cart *cart);

/* ---- Goomba Color's save memory: the GBA cartridge's 64 KB SRAM ---- */

#define GOOMBA_SRAM_SIZE 0x10000

/* The SRAM starts with Goomba's state ID. */
int goomba_is_sram(const uint8_t *sram);
/* Copies the save of the Game Boy game titled title (gb_title) into out
 * (size bytes: the cartridge's RAM). Goomba keeps a compressed copy of each
 * game's save, and the game it ran last uncompressed near the end of SRAM
 * (its newest data when the inject was closed without Goomba's menu).
 * 0, or -1 with the reason in *why. */
int goomba_read(const uint8_t *sram, const char *title, uint8_t *out, uint32_t size, const char **why);
/* Puts data in as that game's save: the compressed copy is replaced, and the
 * uncompressed one too when it is that game's. 0, or -1 (sram unchanged). */
int goomba_write(uint8_t *sram, const char *title, const uint8_t *data, uint32_t size, const char **why);

/* ---- RetroArch's files: a Game Boy game played in RetroArch keeps its
 * cartridge RAM in <savefile_directory>[/<core>]/<ROM name>.srm ---- */

/* The value of key in retroarch.cfg's text (key = "value"); 0 when missing,
 * empty or "default" (RetroArch's built-in directory). */
int ra_cfg_value(const char *cfg, const char *key, char *out, unsigned n);
/* RetroArch's "sd:/..." and "fs:/vol/..." paths as the player opens them
 * (card is the SD card's root, "/vol/external01"); 0 for any other device. */
int ra_path(const char *in, const char *card, char *out, unsigned n);
/* The next "path" entry of a playlist (.lpl, JSON) at or after text, JSON
 * escapes undone; returns where to look for the one after, NULL at the end. */
const char *lpl_next_path(const char *text, char *out, unsigned n);

/* ---- VBA GX's files: a Game Boy game played in VBA GX (Visual Boy Advance
 * GX, the Wii U app) keeps its cartridge RAM (and, for a cartridge with a
 * clock, the clock after it) in <save folder>/<ROM name>.sav; the save folder
 * is vbagx/saves unless settings.xml says otherwise ---- */

/* The value of the setting called name in VBA GX's settings.xml text
 * (<setting name="name" value="..." .../>), XML escapes undone; 0 when
 * missing or empty. */
int vba_setting(const char *xml, const char *name, char *out, unsigned n);
/* A folder as VBA GX keeps it (from its device's root: "vbagx/saves") as the
 * player opens it on the SD card (card: its root, "/vol/external01"), the
 * slash at the end dropped; also takes RetroArch's forms (ra_path). 0 for a
 * folder on another device (usb1:/, a network share). */
int vba_path(const char *folder, const char *card, char *out, unsigned n);
/* The Cartridge Save of the ROM named base (its file name without the
 * extension) in the save folder dir: <base>.sav, or <base> Auto.sav as VBA
 * GX 3.0.4 and older name it, the newer when both are there (VBA GX keeps
 * the newer when it renames them). 0 when neither is there. */
int vba_save_path(const char *dir, const char *base, char *out, unsigned n);

/* Writes data (size bytes) over the start of the existing save file at path,
 * what follows kept (the clock after the RAM in a RetroArch, OurGB or VBA GX
 * save). 0, or -1. */
int gb_save_put(const char *path, const uint8_t *data, uint32_t size);

/* ---- which Game Boy games each Transfer Pak game takes (tpak.c's g_pairs) ---- */

typedef struct {
	const char *n64;              /* the N64 header name's start, squeezed; NULL: by crc1 only */
	const uint32_t *crc1;         /* the Japan-only games' CRC1s, 0 at the end */
	const char *const *titles;    /* Game Boy header titles' starts, squeezed */
	const char *const *files;     /* Game Boy file names' words, lower case */
	const char *scan;             /* the word OurConsoles' file names are looked through for */
	int any;                      /* any Game Boy game works */
} tpak_pair;

/* The N64 game's entry (n64_name: the header's 20 bytes), or NULL for a
 * game whose Game Boy games Our64 does not know. */
const tpak_pair *tpak_pair_of(const unsigned char *n64_name, uint32_t crc1);
/* 2: the Game Boy game (header title, file name without its folder) is one
 * the N64 game was made for; 1: another, in a game that takes any; 0: one it
 * turns away. */
int tpak_made_for(const tpak_pair *p, const char *title, const char *file);
/* word (lower case) in name, any case */
int tpak_has_word(const char *name, const char *word);

/* ---- OurConsoles' files: a Game Boy game played in OurGB keeps its
 * cartridge RAM (and the clock after it) in
 * <card>/wiiu/ourconsoles/saves/GB/<game id>.srm ---- */

/* OurGB's game id (its game_id.c) from the ROM's first 0x150 bytes and its
 * size: "<TITLE>-<GLOBAL CHECKSUM>-<SIZE in KB>", the title's characters
 * other than letters, digits, spaces, dots and dashes made _, spaces at the
 * end dropped. 0, or -1 when the header checksum is wrong or the title has
 * no letter or digit (OurGB then names the save after the file). */
int oc_game_id(const uint8_t *rom, uint32_t rom_size, char *out, unsigned n);

#ifdef __cplusplus
}
#endif
#endif
