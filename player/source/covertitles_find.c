/* Our64 player: covertitles_find, a binary search over the generated table
 * (covertitles.c stays data only). Plain C. */
#include "covertitles.h"

#include <stddef.h>

const char *covertitles_find(uint32_t crc1, uint32_t crc2)
{
	unsigned lo = 0, hi = covertitles_count;
	while (lo < hi) {
		unsigned mid = lo + (hi - lo) / 2;
		const covertitle *t = &covertitles[mid];
		if (t->crc1 == crc1 && t->crc2 == crc2)
			return t->name;
		if (t->crc1 < crc1 || (t->crc1 == crc1 && t->crc2 < crc2))
			lo = mid + 1;
		else
			hi = mid;
	}
	return NULL;
}
