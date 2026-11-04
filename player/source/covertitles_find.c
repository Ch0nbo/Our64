/* Our64 player: covertitles_find, a binary search over the generated table
 * (covertitles.c stays data only). Plain C. */
#include "covertitles.h"

#include <stddef.h>
#include <string.h>

const char *covertitles_find(uint32_t crc1, uint32_t crc2, char country)
{
	/* the pair's first row: the first not below (crc1, crc2) */
	unsigned lo = 0, hi = covertitles_count;
	while (lo < hi) {
		unsigned mid = lo + (hi - lo) / 2;
		const covertitle *t = &covertitles[mid];
		if (t->crc1 < crc1 || (t->crc1 == crc1 && t->crc2 < crc2))
			lo = mid + 1;
		else
			hi = mid;
	}
	if (lo == covertitles_count || covertitles[lo].crc1 != crc1 || covertitles[lo].crc2 != crc2)
		return NULL;
	/* a pair two names share: the row that lists the country */
	for (unsigned i = lo; country && i < covertitles_count && covertitles[i].crc1 == crc1 &&
			covertitles[i].crc2 == crc2; i++)
		if (covertitles[i].countries && strchr(covertitles[i].countries, country))
			return covertitles[i].name;
	return covertitles[lo].name;
}
