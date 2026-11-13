/* Game options > Fixes' games (fixes.c): A to Z, each code unique and well formed, and the
 * header fixes are found by: the ROM's own, or the picked game's. */
#include "fixes.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
int main(void){
	const int n = fixes_count();
	assert(n > 0 && n <= 31); /* Auto plus these fit a choice page (CHOICES_MAX 32) */
	for (int i = 0; i < n; i++) {
		const fixes_game *g = fixes_at(i);
		assert(g && g->title[0] && strlen(g->name) <= 20 && (g->crc1 || g->crc2));
		assert(strlen(g->code) == 4 && g->code[0] == 'N' && g->code[3] == 'E');
		if (i) assert(strcasecmp(fixes_at(i - 1)->title, g->title) < 0); /* A to Z */
		assert(fixes_find(g->code) == i);
	}
	assert(!fixes_at(-1) && !fixes_at(n) && fixes_find(NULL) == -1 && fixes_find("NXXE") == -1);

	const unsigned char own[20] = "REMIX EXTRA         ";
	fixes_start(own, 0x61751D6A, 0x771B9888, 'A' << 8 | 'L', 'E', NULL);
	const fixes_header *h = fixes_now();
	assert(h->picked == -1 && !memcmp(h->name, own, 20) && h->crc1 == 0x61751D6A && h->cart_id == ('A' << 8 | 'L'));
	fixes_start(own, 0x61751D6A, 0x771B9888, 'A' << 8 | 'L', 'E', "NQQE"); /* a code this Our64 doesn't know: its own */
	assert(h->picked == -1 && h->crc2 == 0x771B9888);
	fixes_start(own, 0x61751D6A, 0x771B9888, 'A' << 8 | 'L', 'E', "NZLE");
	assert(h->picked == fixes_find("NZLE"));
	assert(!memcmp(h->name, "THE LEGEND OF ZELDA ", 20) && strlen(h->name) == 20);
	assert(h->crc1 == 0x693BA2AE && h->crc2 == 0xB7F14E9F && h->cart_id == ('Z' << 8 | 'L') && h->country == 'E');
	puts("fixes ok"); return 0; }
