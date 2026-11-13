/* Our64 player: Game options > Fixes (fixes.h). */
#include "fixes.h"
#include <string.h>

/* Hack bases with fixes of their own in Our64, and games whose fixes no other
 * game has (Zach, 2026-11-13: the most common and the most unique). Headers
 * read from retail USA dumps; what each carries, as of 2026-11-13:
 *   Banjo-Kazooie          Rumble Pak
 *   Banjo-Tooie            counter factor 1, 16 kbit EEPROM, Rumble Pak
 *   Battle for Naboo       TLB links, discards in place, all highp, factor 1
 *   Blast Corps            GLideN64 hack_blastCorps
 *   Castlevania            whole-function code check, all highp, 16 kbit EEPROM
 *   Conker's Bad Fur Day   GLideN64 hacks and texrect, all highp, factor 1, TLB links, 16 kbit EEPROM
 *   Diddy Kong Racing      Rumble Pak, Controller Pak
 *   Donkey Kong 64         factor 1, 16 kbit EEPROM, Rumble Pak
 *   F-Zero X               Rumble Pak
 *   GoldenEye 007          GLideN64 hack_clearAloneDepthBuffer, TLB links
 *   Kirby 64               all highp, 16 kbit EEPROM
 *   Mario Kart 64          GLideN64 hack_MK64, Controller Pak
 *   Mario Tennis           GLideN64 hacks, 16 kbit EEPROM, Rumble and Transfer Pak
 *   Paper Mario            GLideN64 hack and texrect, factor 1
 *   Perfect Dark           GLideN64 hacks and texrect, TLB links, 16 kbit EEPROM, every Pak
 *   Pokemon Snap           GLideN64 hack_Snap, factor 1
 *   Pokemon Stadium 2      Transfer Pak
 *   Quake II               GLideN64 hack, factor 1
 *   Resident Evil 2        GLideN64 hacks, factor 1
 *   Star Fox 64            Rumble Pak
 *   Star Wars Ep. I Racer  factor 1, 16 kbit EEPROM
 *   Super Smash Bros.      all highp, factor 1
 *   Yoshi's Story          GLideN64 texrect, 16 kbit EEPROM
 *   Zelda: Majora's Mask   GLideN64 hacks
 *   Zelda: Ocarina of Time GLideN64 hacks, discards in place, Rumble Pak
 * Super Mario 64 has none, so it is not here. */
static const fixes_game g_games[] = {
	{ "Banjo-Kazooie", "NBKE", "Banjo-Kazooie", 0xA4BF9306, 0xBF0CDFD1 },
	{ "Banjo-Tooie", "NB7E", "BANJO TOOIE", 0xC2E9AA9A, 0x475D70AA },
	{ "Battle for Naboo", "NNAE", "Battle for Naboo", 0x3D02989B, 0xD4A381E2 },
	{ "Blast Corps", "NBCE", "Blast Corps", 0x7C647E65, 0x1948D305 },
	{ "Castlevania", "ND3E", "CASTLEVANIA", 0x4BCDFF47, 0xAAA3AF8F },
	{ "Conker's Bad Fur Day", "NFUE", "CONKER BFD", 0x30C7AC50, 0x7704072D },
	{ "Diddy Kong Racing", "NDYE", "Diddy Kong Racing", 0x53D440E7, 0x7519B011 },
	{ "Donkey Kong 64", "NDOE", "DONKEY KONG 64", 0xEC58EABF, 0xAD7C7169 },
	{ "F-Zero X", "NFZE", "F-ZERO X", 0xB30ED978, 0x3003C9F9 },
	{ "GoldenEye 007", "NGEE", "GOLDENEYE", 0xDCBC50D1, 0x09FD1AA3 },
	{ "Kirby 64", "NK4E", "Kirby64", 0x46039FB4, 0x0337822C },
	{ "Mario Kart 64", "NKTE", "MARIOKART64", 0x3E5055B6, 0x2E92DA52 },
	{ "Mario Tennis", "NM8E", "MarioTennis", 0x5001CF4F, 0xF30CB3BD },
	{ "Paper Mario", "NMQE", "PAPER MARIO", 0x65EEE53A, 0xED7D733C },
	{ "Perfect Dark", "NPDE", "Perfect Dark", 0x41F2B98F, 0xB458B466 },
	{ "Pokemon Snap", "NPFE", "POKEMON SNAP", 0xCA12B547, 0x71FA4EE4 },
	{ "Pokemon Stadium 2", "NP3E", "POKEMON STADIUM 2", 0x03571182, 0x892FD06D },
	{ "Quake II", "NQ2E", "QUAKE II", 0xBDA8F143, 0xB1AF2D62 },
	{ "Resident Evil 2", "NREE", "Resident Evil II", 0xAA18B1A5, 0x07DB6AEB },
	{ "Star Fox 64", "NFXE", "STARFOX64", 0xBA780BA0, 0x0F21DB34 },
	{ "Star Wars Episode I: Racer", "NEPE", "STAR WARS EP1 RACER", 0x72F70398, 0x6556A98B },
	{ "Super Smash Bros.", "NALE", "SMASH BROTHERS", 0x916B8B5B, 0x780B85A4 },
	{ "Yoshi's Story", "NYSE", "YOSHI STORY", 0x2337D8E8, 0x6B8E7CEC },
	{ "Zelda: Majora's Mask", "NZSE", "ZELDA MAJORA'S MASK", 0x5354631C, 0x03A2DEF0 },
	{ "Zelda: Ocarina of Time", "NZLE", "THE LEGEND OF ZELDA", 0x693BA2AE, 0xB7F14E9F },
};

static fixes_header g_now = { .picked = -1 };

int fixes_count(void) { return (int)(sizeof(g_games) / sizeof(g_games[0])); }

const fixes_game *fixes_at(int i) { return i >= 0 && i < fixes_count() ? &g_games[i] : NULL; }

int fixes_find(const char *code)
{
	for (int i = 0; code && i < fixes_count(); i++)
		if (!strcmp(g_games[i].code, code))
			return i;
	return -1;
}

void fixes_start(const unsigned char *name20, uint32_t crc1, uint32_t crc2, uint16_t cart_id, uint8_t country,
                 const char *code)
{
	const int i = fixes_find(code);
	memset(&g_now, 0, sizeof(g_now));
	g_now.picked = i;
	if (i < 0) {
		memcpy(g_now.name, name20, 20);
		g_now.crc1 = crc1;
		g_now.crc2 = crc2;
		g_now.cart_id = cart_id;
		g_now.country = country;
		return;
	}
	const fixes_game *g = &g_games[i];
	memset(g_now.name, ' ', 20);
	memcpy(g_now.name, g->name, strlen(g->name) < 20 ? strlen(g->name) : 20);
	g_now.crc1 = g->crc1;
	g_now.crc2 = g->crc2;
	g_now.cart_id = (uint16_t)((uint8_t)g->code[1] << 8 | (uint8_t)g->code[2]);
	g_now.country = (uint8_t)g->code[3];
}

const fixes_header *fixes_now(void) { return &g_now; }
