/* PC tests for tpak.c: the Transfer Pak as libultra's osGbpak calls drive
 * it, a Game Boy cartridge's mapper, Goomba Color's save memory, and where
 * RetroArch, VBA GX and OurGB keep Game Boy saves. */
#include "tpak.h"
#include "minilzo.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <utime.h>

static int g_fail;
#define CHECK(c) do { if (!(c)) { printf("  FAIL line %d: %s\n", __LINE__, #c); g_fail = 1; } } while (0)

static void put16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *p, uint32_t v) { put16(p, v); put16(p + 2, v >> 16); }

static void block(uint8_t b[32], uint8_t v) { memset(b, v, 32); }

/* A 64 KB MBC5 cartridge with 8 KB of battery RAM, titled MARIO GOLF. */
static uint8_t *make_rom(void)
{
	uint8_t *rom = calloc(1, 0x10000);
	for (int i = 0; i < 0x10000; i++)
		rom[i] = (uint8_t)(i / 0x4000 * 0x11); /* each bank filled with its number */
	memcpy(rom + 0x134, "MARIO GOLF\0AWXE", 15);
	rom[0x147] = 0x1B; /* MBC5+RAM+BATTERY */
	rom[0x149] = 0x02; /* 8 KB */
	return rom;
}

static void test_tpak(void)
{
	uint8_t *rom = make_rom();
	uint8_t ram[0x2000];
	memset(ram, 0x5A, sizeof(ram));
	gb_cart cart;
	char title[16];
	gb_title(rom, title);
	CHECK(!strcmp(title, "MARIO GOLF"));
	CHECK(gb_ram_size(rom) == 0x2000);
	CHECK(gb_cart_init(&cart, rom, 0x10000, ram, sizeof(ram), NULL) == 0);
	tpak t;
	tpak_power_on(&t, &cart);
	uint8_t b[32];

	/* osGbpakInit: power off, read back (anything but 0xFE), power on, read 0x84 */
	block(b, 0xFE); tpak_write(&t, 0x8000, b);
	tpak_read(&t, 0x8000, b); CHECK(b[31] == 0x00);
	block(b, 0x84); tpak_write(&t, 0x8000, b);
	tpak_read(&t, 0x8000, b); CHECK(b[31] == 0x84);
	/* osGbpakPower on (0xB000), then the status says powered and reset done */
	block(b, 0x01); tpak_write(&t, 0xB000, b);
	tpak_read(&t, 0xB000, b); CHECK(b[0] & 0x80); CHECK(b[0] & 0x01);
	/* osGbpakReadId: bank 0, the header at 0x100 */
	block(b, 0); tpak_write(&t, 0xA000, b);
	tpak_read(&t, 0xC000 + 0x120, b);
	CHECK(!memcmp(b + 0x14, "MARIO GOLF", 10));
	/* switchable ROM bank: GB 0x2000 <- 3, read GB 0x4000 through Pak bank 1 */
	block(b, 3); tpak_write(&t, 0xC000 + 0x2000, b);
	block(b, 1); tpak_write(&t, 0xA000, b);
	tpak_read(&t, 0xC000, b); CHECK(b[0] == 0x33);

	/* RAM: disabled reads 0xFF; enabled (0x0A at GB 0x0000) reads the save */
	block(b, 2); tpak_write(&t, 0xA000, b); /* Pak bank 2: GB 0x8000-0xBFFF */
	tpak_read(&t, 0xE000, b); CHECK(b[0] == 0xFF);
	block(b, 0); tpak_write(&t, 0xA000, b);
	block(b, 0x0A); tpak_write(&t, 0xC000, b);
	block(b, 2); tpak_write(&t, 0xA000, b);
	tpak_read(&t, 0xE000, b); CHECK(b[0] == 0x5A);
	CHECK(!cart.ram_written);
	for (int i = 0; i < 32; i++) b[i] = (uint8_t)i;
	tpak_write(&t, 0xE020, b);
	CHECK(ram[0x20] == 0 && ram[0x3F] == 31);
	CHECK(cart.ram_written);
	memset(b, 0, 32);
	tpak_read(&t, 0xE020, b); CHECK(b[5] == 5);

	/* powered off: the cartridge reads as zeros, writes go nowhere */
	block(b, 0xFE); tpak_write(&t, 0x8000, b);
	tpak_read(&t, 0x8000, b); CHECK(b[0] == 0);
	tpak_read(&t, 0xE020, b); CHECK(b[5] == 0);
	free(rom);
}

/* Goomba's SRAM: the ID, a config header, another game's save, then MARIO GOLF's. */
static uint32_t add_save(uint8_t *sram, uint32_t at, const char *title, uint32_t checksum, const uint8_t *data, uint32_t size)
{
	static uint8_t work[LZO1X_1_MEM_COMPRESS];
	uint8_t packed[0x3000];
	lzo_uint n;
	lzo1x_1_compress(data, size, packed, &n, work);
	uint32_t total = (48 + (uint32_t)n + 3) & ~3u;
	put16(sram + at, total);
	put16(sram + at + 2, 1);
	put32(sram + at + 4, size);
	put32(sram + at + 12, checksum);
	strcpy((char *)sram + at + 16, title);
	memcpy(sram + at + 48, packed, n);
	return at + total;
}

static void test_goomba(void)
{
	CHECK(lzo_init() == LZO_E_OK);
	uint8_t *sram = calloc(1, GOOMBA_SRAM_SIZE);
	uint8_t other[0x2000], golf[0x2000], out[0x2000];
	for (int i = 0; i < 0x2000; i++) {
		other[i] = (uint8_t)(i * 7);
		golf[i] = (uint8_t)(i % 13 == 0 ? i : 0);
	}
	put32(sram, 0x57A731D8);
	put16(sram + 4, 48);
	put16(sram + 6, 2); /* config */
	strcpy((char *)sram + 4 + 16, "CFG");
	uint32_t at = add_save(sram, 52, "OTHER GAME", 0x1111, other, sizeof(other));
	add_save(sram, at, "MARIO GOLF", 0x2222, golf, sizeof(golf));
	CHECK(goomba_is_sram(sram));

	const char *why = NULL;
	CHECK(goomba_read(sram, "MARIO GOLF", out, sizeof(out), &why) == 0);
	CHECK(!memcmp(out, golf, sizeof(golf)));
	CHECK(goomba_read(sram, "POKEMON RED", out, sizeof(out), &why) == -1);

	/* Goomba ran MARIO GOLF last: its newest save is the uncompressed one at 0xE000 */
	put32(sram + 4 + 8, 0x2222);
	memset(sram + 0xE000, 0x77, 0x2000);
	CHECK(goomba_read(sram, "MARIO GOLF", out, sizeof(out), &why) == 0);
	CHECK(out[0] == 0x77 && out[0x1FFF] == 0x77);

	/* the N64 game changed it: both copies take it, the other game keeps its own */
	uint8_t changed[0x2000];
	for (int i = 0; i < 0x2000; i++)
		changed[i] = (uint8_t)(i * 3 + 1);
	CHECK(goomba_write(sram, "MARIO GOLF", changed, sizeof(changed), &why) == 0);
	CHECK(!memcmp(sram + 0xE000, changed, sizeof(changed)));
	put32(sram + 4 + 8, 0); /* Goomba's next start packs 0xE000 and frees it */
	CHECK(goomba_read(sram, "MARIO GOLF", out, sizeof(out), &why) == 0);
	CHECK(!memcmp(out, changed, sizeof(changed)));
	CHECK(goomba_read(sram, "OTHER GAME", out, sizeof(out), &why) == 0);
	CHECK(!memcmp(out, other, sizeof(other)));

	/* not the owner: only the compressed copy changes */
	memset(sram + 0xE000, 0x44, 0x2000);
	CHECK(goomba_write(sram, "MARIO GOLF", golf, sizeof(golf), &why) == 0);
	CHECK(sram[0xE000] == 0x44);
	CHECK(goomba_read(sram, "MARIO GOLF", out, sizeof(out), &why) == 0);
	CHECK(!memcmp(out, golf, sizeof(golf)));

	/* a save that cannot fit is refused and changes nothing */
	uint8_t *before = malloc(GOOMBA_SRAM_SIZE);
	memcpy(before, sram, GOOMBA_SRAM_SIZE);
	uint8_t *noise = malloc(0xE000);
	for (int i = 0; i < 0xE000; i++)
		noise[i] = (uint8_t)(rand() >> 3);
	CHECK(goomba_write(sram, "MARIO GOLF", noise, 0xE000, &why) == -1);
	CHECK(!memcmp(before, sram, GOOMBA_SRAM_SIZE));
	free(noise), free(before), free(sram);
}

/* RetroArch's settings and playlists, as the Wii U build writes them. */
static void test_retroarch(void)
{
	const char *cfg = "content_history_path = \"fs:/vol/external01/retroarch/content_history.lpl\"\n"
	                  "playlist_directory = \"sd:/retroarch/cores/playlists\"\n"
	                  "savefile_directory = \"default\"\n"
	                  "savefiles_in_content_dir = \"true\"\n"
	                  "sort_savefiles_enable = \"true\"\n";
	char v[256];
	CHECK(ra_cfg_value(cfg, "savefiles_in_content_dir", v, sizeof(v)) && !strcmp(v, "true"));
	CHECK(!ra_cfg_value(cfg, "savefile_directory", v, sizeof(v))); /* "default": RetroArch's own */
	CHECK(!ra_cfg_value(cfg, "sort_savefiles", v, sizeof(v)));     /* a key is the whole name */
	CHECK(!ra_cfg_value(cfg, "missing", v, sizeof(v)));
	CHECK(ra_cfg_value(cfg, "playlist_directory", v, sizeof(v)) && !strcmp(v, "sd:/retroarch/cores/playlists"));

	char out[256];
	CHECK(ra_path("sd:/retroarch/cores/savefiles", "/vol/external01", out, sizeof(out)) &&
	      !strcmp(out, "/vol/external01/retroarch/cores/savefiles"));
	CHECK(ra_path("fs:/vol/external01/roms/gbc/Pokemon.gbc", "/vol/external01", out, sizeof(out)) &&
	      !strcmp(out, "/vol/external01/roms/gbc/Pokemon.gbc"));
	CHECK(ra_path("/vol/external01/x", "/vol/external01", out, sizeof(out)) && !strcmp(out, "/vol/external01/x"));
	CHECK(!ra_path("usb:/x", "/vol/external01", out, sizeof(out)));

	const char *lpl = "{\n  \"items\": [\n    {\n"
	                  "      \"path\": \"fs:/vol/external01/retroarch/roms/GBC/Mario Tennis (USA).gbc\",\n"
	                  "      \"label\": \"Mario \\\"Tennis\\\"\",\n"
	                  "      \"core_path\": \"fs:/vol/external01/retroarch/cores/gambatte_libretro.rpx\"\n    },\n    {\n"
	                  "      \"path\": \"sd:/roms/a\\\\b\\/c.gb\",\n    }\n  ]\n}\n";
	const char *at = lpl_next_path(lpl, out, sizeof(out));
	CHECK(at && !strcmp(out, "fs:/vol/external01/retroarch/roms/GBC/Mario Tennis (USA).gbc"));
	at = at ? lpl_next_path(at, out, sizeof(out)) : NULL;
	CHECK(at && !strcmp(out, "sd:/roms/a\\b/c.gb")); /* JSON escapes undone; core_path is not a path entry */
	CHECK(at && !lpl_next_path(at, out, sizeof(out)));
}

/* A file of size bytes, each byte v; its last change set to when. */
static void put_file(const char *path, uint32_t size, uint8_t v, time_t when)
{
	FILE *f = fopen(path, "wb");
	for (uint32_t i = 0; f && i < size; i++)
		fputc(v, f);
	if (f)
		fclose(f);
	struct utimbuf t = { when, when };
	utime(path, &t);
}

/* VBA GX's settings.xml as mxml writes it, its folders, and its saves: found
 * by the ROM's name, and written back with the clock after the RAM kept. */
static void test_vbagx(void)
{
	const char *xml = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
	                  "<file app=\"Visual Boy Advance GX\" version=\"3.0.4\">\n\n"
	                  "<section name=\"File\" description=\"File Settings\">\n"
	                  "\t<setting name=\"saveMode\" value=\"0\" description=\"Save Mode\" />\n"
	                  "\t<setting name=\"saveDevice\" value=\"1\" description=\"Save Device\" />\n"
	                  "\t<setting name=\"loadFolder\" value=\"roms/Game Boy &amp; Color/\"\n"
	                  "\t\tdescription=\"Load Folder\" />\n"
	                  "\t<setting name=\"lastFileLoaded\" value=\"\" description=\"Last File Loaded\" />\n"
	                  "\t<setting name=\"saveFolder\" value=\"saves/GB\" description=\"Save Folder\" />\n"
	                  "</section>\n</file>\n";
	char v[256];
	CHECK(vba_setting(xml, "saveFolder", v, sizeof(v)) && !strcmp(v, "saves/GB"));
	CHECK(vba_setting(xml, "loadFolder", v, sizeof(v)) && !strcmp(v, "roms/Game Boy & Color/")); /* &amp; undone */
	CHECK(vba_setting(xml, "saveDevice", v, sizeof(v)) && !strcmp(v, "1"));
	CHECK(!vba_setting(xml, "lastFileLoaded", v, sizeof(v))); /* empty */
	CHECK(!vba_setting(xml, "Folder", v, sizeof(v)));         /* a name is the whole name */
	CHECK(!vba_setting(xml, "cheatFolder", v, sizeof(v)));
	CHECK(!vba_setting(xml, "saveFolder", v, 4));             /* too long for out */

	char out[512];
	CHECK(vba_path("vbagx/saves", "/vol/external01", out, sizeof(out)) && !strcmp(out, "/vol/external01/vbagx/saves"));
	CHECK(vba_path("roms/gb/", "/vol/external01", out, sizeof(out)) && !strcmp(out, "/vol/external01/roms/gb"));
	CHECK(vba_path("sd:/x/", "/vol/external01", out, sizeof(out)) && !strcmp(out, "/vol/external01/x"));
	CHECK(vba_path("/vol/external01/y", "/vol/external01", out, sizeof(out)) && !strcmp(out, "/vol/external01/y"));
	CHECK(!vba_path("usb1:/vbagx/saves", "/vol/external01", out, sizeof(out)));

	/* a card of its own: settings.xml moves the saves to saves/GB */
	char card[] = "/tmp/our64-tpak-XXXXXX";
	CHECK(mkdtemp(card) != NULL);
	char saves[256], plain[512], autoed[512];
	CHECK(vba_setting(xml, "saveFolder", v, sizeof(v)) && vba_path(v, card, saves, sizeof(saves)));
	snprintf(out, sizeof(out), "%s/saves", card);
	mkdir(out, 0777);
	mkdir(saves, 0777);
	const char *base = "Pokemon - Crystal Version (USA, Europe) (Rev 1)";
	snprintf(plain, sizeof(plain), "%s/%s.sav", saves, base);
	snprintf(autoed, sizeof(autoed), "%s/%s Auto.sav", saves, base);
	CHECK(!vba_save_path(saves, base, out, sizeof(out)));
	/* VBA GX 3.0.4 and older: <name> Auto.sav */
	put_file(autoed, 0x8030, 0x11, 1000000);
	CHECK(vba_save_path(saves, base, out, sizeof(out)) && !strcmp(out, autoed));
	CHECK(!vba_save_path(saves, "Pokemon - Gold Version (USA, Europe)", out, sizeof(out)));
	/* both: the newer, as VBA GX keeps it when it renames them */
	put_file(plain, 0x8030, 0x22, 900000);
	CHECK(vba_save_path(saves, base, out, sizeof(out)) && !strcmp(out, autoed));
	put_file(plain, 0x8030, 0x22, 1100000);
	CHECK(vba_save_path(saves, base, out, sizeof(out)) && !strcmp(out, plain));

	/* MBC3 with a clock (Gold, Silver, Crystal): 32 KB of RAM, then VBA GX's
	 * 48 bytes of clock; the N64 game's RAM goes over the first, the clock stays */
	FILE *f = fopen(plain, "r+b");
	for (int i = 0; f && i < 48; i++)
		fseek(f, 0x8000 + i, SEEK_SET), fputc(0xC0 + i, f);
	if (f)
		fclose(f);
	uint8_t *ram = malloc(0x8000);
	for (int i = 0; i < 0x8000; i++)
		ram[i] = (uint8_t)(i * 5);
	CHECK(gb_save_put(plain, ram, 0x8000) == 0);
	uint8_t *back = calloc(1, 0x8100);
	f = fopen(plain, "rb");
	const size_t got = f ? fread(back, 1, 0x8100, f) : 0;
	if (f)
		fclose(f);
	CHECK(got == 0x8030);
	CHECK(!memcmp(back, ram, 0x8000));
	int clock_kept = 1;
	for (int i = 0; i < 48; i++)
		clock_kept &= back[0x8000 + i] == 0xC0 + i;
	CHECK(clock_kept);
	snprintf(out, sizeof(out), "%s/missing.sav", saves);
	CHECK(gb_save_put(out, ram, 0x8000) == -1); /* only an existing save is written */
	CHECK(access(out, F_OK) != 0);
	free(ram), free(back);
	remove(plain), remove(autoed), rmdir(saves);
	snprintf(out, sizeof(out), "%s/saves", card);
	rmdir(out), rmdir(card);
}

/* OurGB's game id: its saves are saves/GB/<id>.srm. */
static void test_ourconsoles(void)
{
	uint8_t *rom = make_rom();
	memcpy(rom + 0x134, "PM_CRYSTAL\0BYTE", 15);
	rom[0x14E] = 0x12, rom[0x14F] = 0x9F;
	uint8_t x = 0;
	for (int i = 0x134; i <= 0x14C; i++)
		x = (uint8_t)(x - rom[i] - 1);
	rom[0x14D] = x;
	char id[40];
	CHECK(oc_game_id(rom, 0x200000, id, sizeof(id)) == 0 && !strcmp(id, "PM_CRYSTAL-129F-2048"));
	memcpy(rom + 0x134, "KIRBY'S DL  ", 12); /* the ' made _, the spaces at the end dropped */
	memset(rom + 0x140, 0, 3);
	x = 0;
	for (int i = 0x134; i <= 0x14C; i++)
		x = (uint8_t)(x - rom[i] - 1);
	rom[0x14D] = x;
	CHECK(oc_game_id(rom, 0x40000, id, sizeof(id)) == 0 && !strcmp(id, "KIRBY_S DL-129F-256"));
	rom[0x14D] ^= 1; /* a wrong header checksum: OurGB uses the file name */
	CHECK(oc_game_id(rom, 0x40000, id, sizeof(id)) == -1);
	CHECK(oc_game_id(rom, 0x40000, id, 8) == -1);
	free(rom);
}

/* Which Game Boy games each Transfer Pak game takes: real header titles and
 * No-Intro file names. */
static int takes(const char *n64, uint32_t crc1, const char *title, const char *file)
{
	return tpak_made_for(tpak_pair_of((const unsigned char *)n64, crc1), title, file);
}

static void test_pairs(void)
{
	CHECK(takes("POKEMON STADIUM 2   ", 0x03571182, "POKEMON_GLDAAUE", "Pokemon - Gold Version (USA, Europe)") == 2);
	CHECK(takes("POKEMON STADIUM 2   ", 0x03571182, "PM_CRYSTAL", "x") == 2);
	CHECK(takes("POKEMON STADIUM 2   ", 0x03571182, "POKEMON RED", "x") == 2);
	CHECK(takes("POKEMON STADIUM 2   ", 0x03571182, "POKEPINBALLVPHE", "Pokemon Pinball (USA)") == 0);
	CHECK(takes("POKEMON STADIUM     ", 0x9C8FB2FA, "POKEMON YELLOW", "x") == 2);
	CHECK(takes("POKEMON STADIUM     ", 0x9C8FB2FA, "POKEMON_SLVAAXE", "x") == 0);
	CHECK(takes("Perfect Dark        ", 0x41F2B98F, "PerfDark   VPDE", "x") == 2);
	CHECK(takes("MICKEY USA          ", 0xFA8C4571, "MIKSPEEDWAYBSNE", "x") == 2);
	CHECK(takes("MarioGolf64         ", 0x664BA3D4, "MARIO GOLF", "x") == 2);
	CHECK(takes("MarioTennis         ", 0x5001CF4F, "CGBTENNIS ", "x") == 2);
	CHECK(takes("MarioTennis         ", 0x5001CF4F, "MARIO GOLF", "Mario Golf (USA)") == 0);
	/* Japan: by CRC1, whatever the name; Japan's Stadium 2 is the West's Stadium */
	CHECK(takes("POKEMON STADIUM G&S ", 0xEE4FD7C2, "POKEMON_GLDAAUJ", "x") == 2);
	CHECK(takes("POKEMON STADIUM 2   ", 0x63775886, "POKEMON_GLDAAUJ", "x") == 0);
	CHECK(takes("\xbe\xdd", 0x0AC244D1, "", "Power Pro Kun Pocket 2 (Japan) (SGB Enhanced) (GB Compatible)") == 2);
	CHECK(takes("\xbe\xdd", 0x0AC244D1, "", "Power Pro Kun Pocket (Japan) (SGB Enhanced) (GB Compatible)") == 0);
	CHECK(takes("\xbe\xdd", 0xB75E20B7, "", "Power Pro Kun Pocket (Japan) (Rev 1) (SGB Enhanced) (GB Compatible)") == 2);
	CHECK(takes("\xbe\xdd", 0x5B9B1618, "", "Kawa no Nushi Tsuri 4 (Japan) (Rumble Version) (SGB Enhanced)") == 2);
	CHECK(takes("\xbe\xdd", 0x5B9B1618, "", "Kawa no Nushi Tsuri 3 (Japan) (SGB Enhanced)") == 0);
	/* the games that take any Game Boy game: their own still come first */
	CHECK(takes("\xbe\xdd", 0xF468118C, "TETRIS", "Tetris (World)") == 1);
	CHECK(takes("\xbe\xdd", 0xF468118C, "", "Ultraman Ball (Japan) (SGB Enhanced)") == 2);
	CHECK(takes("\xbe\xdd", 0x272B690F, "SUN", "Robopon - Sun Version (USA) (SGB Enhanced) (GB Compatible)") == 2);
	/* titles read from the No-Intro ROMs: Europe's Yellow, Japan's partners, by any file name */
	CHECK(takes("POKEMON STADIUM     ", 0x9C8FB2FA, "POKEMON YELAPSD", "Pokemon - Gelbe Edition (Germany)") == 2);
	CHECK(takes("PAWAFURU PUROYAKYU6 ", 0, "PP SUCSESS", "renamed.gbc") == 2);
	CHECK(takes("PAWAFURU PUROYAKYU6 ", 0, "PAWA POKE2", "renamed.gbc") == 0);
	CHECK(takes("PAWAPURO 2000       ", 0, "PAWA POKE2", "renamed.gbc") == 2);
	CHECK(takes("\xC7\xBC\xC2\xDE\xD8" "64\xBC\xB5\xB6\xBE\xDE\xC6\xC9\xAF\xC3    ", 0, "KAWANUSHIG4VUTJ", "x") == 2);
	CHECK(takes("\xC7\xBC\xC2\xDE\xD8" "64             ", 0, "UMINUSHI       ", "x") == 2);
	CHECK(takes("\xC7\xBC\xC2\xDE\xD8" "64             ", 0, "KAWANUSHIG4VUTJ", "x") == 0);
	CHECK(takes("\xC1\xAE\xDBQ64 2            ", 0, "CHOROQ HCGBACQJ", "x") == 2);
	CHECK(takes("POKEMON STADIUM G&S ", 0, "POKEMON_SLVAAXJ", "x") == 2);
	CHECK(takes("BattlePhoenix64     ", 0, "SUPER B DAMAN", "x") == 2);
	CHECK(takes("SUPER MARIO 64      ", 0x635A2BFF, "POKEMON RED", "x") == 0);
	CHECK(!tpak_pair_of((const unsigned char *)"SUPER MARIO 64      ", 0x635A2BFF));
}

int main(void)
{
	test_tpak();
	test_goomba();
	test_retroarch();
	test_vbagx();
	test_ourconsoles();
	test_pairs();
	puts(g_fail ? "  FAILED" : "  ok");
	return g_fail;
}
