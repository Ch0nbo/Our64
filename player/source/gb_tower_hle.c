/* Pokemon Stadium 2's GB Tower RSP program in C (see gb_tower_hle.h).
 *
 * The program is a Game Boy picture processor: the CPU hands it the Game
 * Boy's video memory, rearranged, and 8 bytes of registers a line; it draws
 * the background, the window and the sprites into 160 bytes a line, then
 * turns those into 16-bit framebuffer rows between rows of a border picture.
 * Three variants: 0xA10 and 0xB30 for Game Boy Color games (tile attributes,
 * 8 color palettes from tables in RDRAM; 0xB30 adds a color correction),
 * 0xA60 for Game Boy games (BGP, OBP0 and OBP1 shades, 4 palettes chosen by
 * an 18-by-20 attribute map, as the Super Game Boy's).
 *
 * This is a translation, not a reimplementation: DMEM is the RSP's own (the
 * caller's), every DMA is cxd4's (8-byte units, rows and skips, addresses
 * past 8 MB read as 0 and not written), and where odd register values make
 * the program's writes land on its own tables, the steps keep its order
 * (the slow paths below). The comments give the program's IMEM addresses in
 * the 0xA10 variant. cxd4 is the reference: vector results are its non-SSE
 * ones (VMUDN and VMUDL without a clamp, VMULF and VADD saturating), and the
 * vector unit's flags never carry from one task to the next (every VADD the
 * program uses follows one that clears the carry). */
#include "gb_tower_hle.h"

#include <string.h>

#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
#define BX 3u /* a byte's address within its word flips on a little-endian host */
#else
#define BX 0u
#endif
#define HX (BX & 2) /* and a halfword's */

#define RDRAM_LAST 0x7FFFFFu /* cxd4's su_max_address, as rsp_lle_init sets it */

enum { GBC, GBC_CC, GB }; /* the 0xA10, 0xB30 and 0xA60 variants */

/* The 0xC0 bytes after each variant's code, which it loads into vector
 * registers (DMEM 0x00-0xBF): masks for a tile's pixels, color masks and
 * weights, the screen's edges. The code below has their values built in. */
static const uint8_t tables[0xC0] = {
	0x01, 0x01, 0x02, 0x02, 0x04, 0x04, 0x08, 0x08, 0x10, 0x10, 0x20, 0x20, 0x40, 0x40, 0x80, 0x80,
	0x01, 0x00, 0x02, 0x00, 0x04, 0x00, 0x08, 0x00, 0x10, 0x00, 0x20, 0x00, 0x40, 0x00, 0x80, 0x00,
	0x00, 0x01, 0x00, 0x02, 0x00, 0x04, 0x00, 0x08, 0x00, 0x10, 0x00, 0x20, 0x00, 0x40, 0x00, 0x80,
	0x80, 0x80, 0x40, 0x40, 0x20, 0x20, 0x10, 0x10, 0x08, 0x08, 0x04, 0x04, 0x02, 0x02, 0x01, 0x01,
	0x80, 0x00, 0x40, 0x00, 0x20, 0x00, 0x10, 0x00, 0x08, 0x00, 0x04, 0x00, 0x02, 0x00, 0x01, 0x00,
	0x00, 0x80, 0x00, 0x40, 0x00, 0x20, 0x00, 0x10, 0x00, 0x08, 0x00, 0x04, 0x00, 0x02, 0x00, 0x01,
	0xE0, 0x03, 0x1F, 0x00, 0x00, 0x7C, 0xF8, 0x00, 0x07, 0xC0, 0x58, 0x00, 0xC0, 0x00, 0x06, 0x00,
	0xFF, 0xFF, 0xFF, 0xFE, 0xFF, 0xFD, 0xFF, 0xFC, 0xFF, 0xFB, 0xFF, 0xFA, 0xFF, 0xF9, 0xFF, 0xF8,
	0x00, 0x07, 0x00, 0x06, 0x00, 0x05, 0x00, 0x04, 0x00, 0x03, 0x00, 0x02, 0x00, 0x01, 0x00, 0x00,
	0x00, 0xA7, 0x00, 0xA6, 0x00, 0xA5, 0x00, 0xA4, 0x00, 0xA3, 0x00, 0xA2, 0x00, 0xA1, 0x00, 0xA0,
	0x00, 0x3E, 0x7C, 0x00, 0x03, 0xE0, 0x46, 0x00, 0x26, 0x46, 0x4B, 0x23, 0x0E, 0x98, 0x00, 0x00,
	0x71, 0x00, 0x0F, 0x00, 0x7F, 0xFF, 0x0A, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

typedef struct {
	unsigned char *ram, *m; /* RDRAM, DMEM */
	int kind;
	uint16_t s[6];              /* v16: scale and color correction factors */
	uint32_t r2, r12, r18, r19; /* rows' bytes written, ucode_data's next row, a row's offset and bytes */
} gbt;

/* DMEM as the program sees it: big-endian, addresses wrapping at 4 KB. */
static inline uint32_t rb(const unsigned char *m, uint32_t a) { return m[(a & 0xFFF) ^ BX]; }
static inline uint32_t rh(const unsigned char *m, uint32_t a) { return rb(m, a) << 8 | rb(m, a + 1); }
static inline uint32_t rw(const unsigned char *m, uint32_t a) { return rh(m, a) << 16 | rh(m, a + 2); }
static inline void wb(unsigned char *m, uint32_t a, uint32_t v) { m[(a & 0xFFF) ^ BX] = (unsigned char)v; }
static inline void wh(unsigned char *m, uint32_t a, uint32_t v) { wb(m, a, v >> 8); wb(m, a + 1, v); }
static inline void ww(unsigned char *m, uint32_t a, uint32_t v) { wh(m, a, v >> 16); wh(m, a + 2, v); }
/* An even address below 0x1000. */
static inline uint32_t rh2(const unsigned char *m, uint32_t a)
{
	uint16_t v;
	memcpy(&v, m + (a ^ HX), 2);
	return v;
}
static inline void wh2(unsigned char *m, uint32_t a, uint32_t v)
{
	const uint16_t h = (uint16_t)v;
	memcpy(m + (a ^ HX), &h, 2);
}
/* A word at a multiple of 4 below 0x1000: native, either way. */
static inline uint32_t rw4(const unsigned char *m, uint32_t a)
{
	uint32_t v;
	memcpy(&v, m + a, 4);
	return v;
}
static inline void ww4(unsigned char *m, uint32_t a, uint32_t v) { memcpy(m + a, &v, 4); }
/* 8 bytes at any address below 0xFF8, as two words. */
static inline void get8(const unsigned char *m, uint32_t a, uint32_t *w0, uint32_t *w1)
{
	const uint32_t s = (a & 3) * 8, b = a & ~3u, x = rw4(m, b), y = rw4(m, b + 4);
	if (!s) {
		*w0 = x, *w1 = y;
		return;
	}
	const uint32_t z = rw4(m, b + 8);
	*w0 = x << s | y >> (32 - s);
	*w1 = y << s | z >> (32 - s);
}
static inline void put8(unsigned char *m, uint32_t a, uint32_t w0, uint32_t w1)
{
	const uint32_t s = (a & 3) * 8, b = a & ~3u;
	if (!s) {
		ww4(m, b, w0);
		ww4(m, b + 4, w1);
		return;
	}
	ww4(m, b, (rw4(m, b) & ~(0xFFFFFFFFu >> s)) | w0 >> s);
	ww4(m, b + 4, w0 << (32 - s) | w1 >> s);
	ww4(m, b + 8, (rw4(m, b + 8) & (0xFFFFFFFFu >> s)) | w1 << (32 - s));
}

/* A nibble's bits one to a byte, bit 3 in the first (spread) or the last (spread_r). */
#define SP(n, a, b, c, d) ((uint32_t)((n) >> (a) & 1) << 24 | ((n) >> (b) & 1) << 16 | ((n) >> (c) & 1) << 8 | ((n) >> (d) & 1))
#define SP16(a, b, c, d) {SP(0, a, b, c, d), SP(1, a, b, c, d), SP(2, a, b, c, d), SP(3, a, b, c, d), SP(4, a, b, c, d), \
	SP(5, a, b, c, d), SP(6, a, b, c, d), SP(7, a, b, c, d), SP(8, a, b, c, d), SP(9, a, b, c, d), SP(10, a, b, c, d), \
	SP(11, a, b, c, d), SP(12, a, b, c, d), SP(13, a, b, c, d), SP(14, a, b, c, d), SP(15, a, b, c, d)}
static const uint32_t spread[16] = SP16(3, 2, 1, 0), spread_r[16] = SP16(0, 1, 2, 3);

/* cxd4's SP_DMA_READ and SP_DMA_WRITE; mem is always below 0x1000 here (DMEM). */
static void dma(gbt *g, uint32_t mem, uint32_t dram, uint32_t len, int write)
{
	len |= 7;
	const uint32_t length = (len & 0xFFF) + 1, skip = (len >> 20) + length;
	uint32_t count = ((len >> 12) & 0xFF) + 1;
	mem &= 0xFF8;
	dram &= 0xFFFFFFF8u;
	do {
		count--;
		const uint32_t c0 = (count * length + mem) & 0xFF8, d0 = (count * skip + dram) & 0xFFFFF8;
		if (c0 + length <= 0x1000 && d0 + length - 1 <= RDRAM_LAST) { /* neither wraps nor passes 8 MB */
			if (write)
				memcpy(g->ram + d0, g->m + c0, length);
			else
				memcpy(g->m + c0, g->ram + d0, length);
			continue;
		}
		for (uint32_t i = 0; i < length; i += 8) {
			unsigned char *c = g->m + ((count * length + mem + i) & 0xFF8);
			const uint32_t d = (count * skip + dram + i) & 0xFFFFF8;
			if (d > RDRAM_LAST) {
				if (!write)
					memset(c, 0, 8);
			} else if (write)
				memcpy(g->ram + d, c, 8);
			else
				memcpy(c, g->ram + d, 8);
		}
	} while (count);
}

static inline int16_t sat(int32_t v) { return v > 32767 ? 32767 : v < -32768 ? -32768 : (int16_t)v; }
/* VMULF: (2 s t + 0x8000) >> 16, saturated (its one overflow, -32768 squared, too). */
static inline int16_t mulf(int16_t s, int16_t t) { return sat(((int32_t)s * t + 0x4000) >> 15); }
/* VMUDL and VMUDN's results. */
static inline uint16_t mudl(uint16_t s, uint16_t t) { return (uint16_t)((uint32_t)s * t >> 16); }
static inline uint16_t mudn(uint16_t s, uint16_t t) { return (uint16_t)((uint32_t)s * t); }

/* A tile row's pixels 0-3 and 4-7, a byte each: its first byte's bit 7 - k
 * (bit k when flipped) as pixel k's color bit 0 in h, the second's as bit
 * 1 in l. */
static inline void tile_bits(uint32_t tile, int flip, uint32_t *h0, uint32_t *h1, uint32_t *l0, uint32_t *l1)
{
	const uint32_t b0 = tile >> 8, b1 = tile & 0xFF;
	if (flip)
		*h0 = spread_r[b0 & 15], *h1 = spread_r[b0 >> 4], *l0 = spread_r[b1 & 15], *l1 = spread_r[b1 >> 4];
	else
		*h0 = spread[b0 >> 4], *h1 = spread[b0 & 15], *l0 = spread[b1 >> 4], *l1 = spread[b1 & 15];
}

/* GB: pixels' shades (s0-s3 by color) << 1, a byte each. */
static inline uint32_t shades(uint32_t h, uint32_t l, uint32_t s0, uint32_t s1, uint32_t s2, uint32_t s3)
{
	const uint32_t nh = h ^ 0x01010101u, nl = l ^ 0x01010101u;
	return ((nh & nl) * s0 | (h & nl) * s1 | (nh & l) * s2 | (h & l) * s3) << 1;
}

/* Background or window pixels (0x16B0, 0x169C in GB): line r28 / 8's 160
 * bytes at 0xFE0 to DMEM 8, tiles from map row 0xF80 (entry r21 on) drawn at
 * DMEM r18 + 1 up to DMEM r19, and back. A pixel's byte: color << 1, the
 * map entry's attribute bits 3-5 and 7 (GBC), or shade << 1 and color << 3 (GB). */
static void bg_line(gbt *g, uint32_t r28, uint32_t r18, uint32_t r19, uint32_t r21, uint32_t r15)
{
	unsigned char *m = g->m;
	const uint32_t r5 = rw(m, 0xFE0) + r28 * 20;
	dma(g, 8, r5, 0x9F, 0);
	const int gb = g->kind == GB;
	const uint32_t bgp = gb ? rb(m, 0xA07 + r28) : 0, s0 = bgp & 3, s1 = bgp >> 2 & 3, s2 = bgp >> 4 & 3, s3 = bgp >> 6;
	/* 8 bytes a tile from DMEM r18 + 1 (below 0x110), as words: the first
	 * word's leading bytes kept, a word held over between tiles. */
	const uint32_t sh = ((r18 + 1) & 3) * 8;
	uint32_t at = (r18 + 1) & ~3u, held = sh ? rw4(m, at) & ~(0xFFFFFFFFu >> sh) : 0, w0 = 0, w1 = 0;
	int32_t left;
	while ((left = (int32_t)(r19 - r18)) > 0) { /* pixels before r19 (else the byte read is written back) */
		const uint32_t r20 = rh2(m, 0xF80 + r21), tile = rh2(m, ((r20 & 0x7FE) ^ r15) + 0x200);
		const uint32_t attr = (r20 >> 8 & 0xB8) * 0x01010101u;
		uint32_t h0, h1, l0, l1;
		tile_bits(tile, !gb && (r20 & 0x4000), &h0, &h1, &l0, &l1);
		if (gb) { /* color << 3, BGP's shade for it << 1 */
			w0 = h0 << 3 | l0 << 4 | shades(h0, l0, s0, s1, s2, s3) | attr;
			w1 = h1 << 3 | l1 << 4 | shades(h1, l1, s0, s1, s2, s3) | attr;
		} else { /* color << 1 */
			w0 = h0 << 1 | l0 << 2 | attr;
			w1 = h1 << 1 | l1 << 2 | attr;
		}
		if (left < 8)
			break;
		if (sh) {
			ww4(m, at, held | w0 >> sh);
			ww4(m, at + 4, w0 << (32 - sh) | w1 >> sh);
			held = w1 << (32 - sh);
		} else {
			ww4(m, at, w0);
			ww4(m, at + 4, w1);
		}
		at += 8;
		r21 = (r21 + 2) & 0x3E;
		r18 += 8;
	}
	if (sh)
		ww4(m, at, held | (rw4(m, at) & (0xFFFFFFFFu >> sh)));
	for (int k = 0; k < left; k++) /* the last tile's first pixels */
		wb(m, r18 + 1 + k, (k < 4 ? w0 << 8 * k : w1 << 8 * (k - 4)) >> 24);
	dma(g, 8, r5, 0x9F, 1);
}

/* 4 bytes of a line with a sprite's pixels drawn: unless clear (h and l 0),
 * off the screen (on's bit 7 clear) or under a background pixel with
 * priority (its bit 7, or the sprite's) and a color (old & keep). */
static inline uint32_t obj_pixels(uint32_t old, uint32_t h, uint32_t l, uint32_t on, uint32_t prio, uint32_t keep,
                                  uint32_t ink)
{
	const uint32_t behind = ((old & 0x80808080u) | prio) & (((old & keep) + 0x7F7F7F7Fu) & 0x80808080u);
	const uint32_t draw = ((h | l) << 7) & ~behind & on, mask = (draw >> 7) * 0xFF;
	return (old & ~mask) | (ink & mask);
}

/* A sprite's lines r16 to r16 + r7, bottom first (0x1770): 16 bytes of each
 * line from 0xFE0 at x & 0xF8 - 8 to DMEM, the sprite's 8 pixels drawn where
 * the line's LCDC has sprites on, under background pixels with priority, and
 * back. *r29 is the tile row's offset at DMEM 0xF80, stepped by r20 a line. */
static void obj_lines(gbt *g, uint32_t r16, int32_t r7, uint32_t r18, uint32_t r28, uint32_t r3, uint32_t *r29,
                      uint32_t r20)
{
	unsigned char *m = g->m;
	const uint32_t r5 = rw(m, 0xFE0) + r16 * 160 + (r18 & 0xF8) - 8;
	uint32_t r8 = (uint32_t)r7 << 3, r24 = r8 << 1;
	const uint32_t r6 = 0x09000000u | r8 << 9 | 0xF;
	r28 += r8;
	dma(g, 0, r5, r6, 0);
	const int flip = (r3 & 0x20) != 0;
	/* Lanes on the screen (7 - k < x <= 0xA7 - k), a byte each, pixels 0-3 and 4-7. */
	const int k0 = r18 > 7 ? 0 : 8 - (int)r18, k1 = r18 <= 0xA0 ? 8 : 0xA8 - (int)r18;
	uint32_t on0 = 0, on1 = 0;
	for (int k = k0; k < k1; k++)
		*(k < 4 ? &on0 : &on1) |= 0x80u << (24 - 8 * (k & 3));
	uint32_t top = (0xC0 | (r3 & 7) << 3) * 0x01010101u, keep = 0x46 * 0x01010101u;
	const uint32_t prio = (r3 & 0x80) * 0x01010101u;
	do {
		const uint32_t lcdc = rb(m, 0xA00 + r28), at = (r18 & 7) + r24;
		r28 -= 8;
		r7--;
		if (lcdc & 2) {
			uint32_t h0, h1, l0, l1, o0, o1, s1 = 0, s2 = 0, s3 = 0;
			tile_bits(rh(m, 0xF80 + *r29), flip, &h0, &h1, &l0, &l1);
			if (g->kind == GB) { /* OBP1 or OBP0's shades */
				uint32_t obp = rh(m, 0xA0C + r28);
				if (!(r3 & 0x10))
					obp >>= 8;
				s1 = obp >> 2 & 3, s2 = obp >> 4 & 3, s3 = obp >> 6 & 3;
				keep = 0x58 * 0x01010101u;
				top = 0xC0 * 0x01010101u;
			}
			const uint32_t ink0 = g->kind == GB ? shades(h0, l0, 0, s1, s2, s3) : h0 << 1 | l0 << 2;
			const uint32_t ink1 = g->kind == GB ? shades(h1, l1, 0, s1, s2, s3) : h1 << 1 | l1 << 2;
			get8(m, at, &o0, &o1);
			put8(m, at, obj_pixels(o0, h0, l0, on0, prio, keep, ink0 | top), obj_pixels(o1, h1, l1, on1, prio, keep, ink1 | top));
		}
		*r29 += r20;
		r24 -= 16;
	} while (r7 >= 0);
	dma(g, 0, r5, r6, 1);
}

/* Game Boy Color colors to framebuffer ones in place, 16 bytes at a time
 * down from DMEM r8 to r4 (0x19A0 after its DMA). */
static void pal(gbt *g, uint32_t r4, uint32_t r8)
{
	unsigned char *m = g->m;
	do {
		r8 -= 16;
		for (int i = 0; i < 8; i++) {
			const uint32_t a = r8 + 2 * i;
			const uint16_t v0 = rh(m, a);
			const uint16_t r = mudn(mudl(v0 & 0x1F00, g->s[0]), 0x0800) & 0xF800;
			const uint16_t gr = mudl(mudn(v0, 0x4000) | mudl(v0 & 0xE003, 0x4000), g->s[1]) & 0x07C0;
			const uint16_t b = mudl(mudn(v0 & 0x007C, g->s[2]), 0x0040) | 1;
			wh(m, a, r | gr | b);
		}
	} while (r8 != r4);
}

/* The same with the color correction (0x1A34 in the 0xB30 variant): channels
 * mixed by weights from the tables, by 0xFDD's bit 7 and strength, then scaled. */
static void pal_cc(gbt *g, uint32_t r4, uint32_t r8)
{
	unsigned char *m = g->m;
	const int plain = (rb(m, 0xFDD) & 0x80) != 0;
	const int16_t k = (int16_t)g->s[5], w = (int16_t)g->s[4], s = (int16_t)g->s[3];
	do {
		r8 -= 16;
		for (int i = 0; i < 8; i++) {
			const uint32_t a = r8 + 2 * i;
			const uint16_t v0 = rh(m, a), v4 = v0 & 0xE003;
			const int16_t c3 = (int16_t)(mudn(v0 & 0x1F00, 0x0004) | 0x200);
			const int16_t c4 = (int16_t)(mudn(v4, 0x2000) | mudl(v4, 0x2000) | 0x200);
			const int16_t c5 = (int16_t)(mudn(v0 & 0x007C, 0x0100) | 0x200);
			const int16_t y = sat(sat((int16_t)(mulf(c3, 0x2646) | 0x200) + (int16_t)(mulf(c4, 0x4B23) | 0x200)) +
			                      (int16_t)(mulf(c5, 0x0E98) | 0x200));
			int16_t e7 = y, e9 = y;
			if (!plain) {
				e7 = sat(y + 0x0F00);
				e9 = y >= 0x0A00 ? (int16_t)(y - 0x0A00) : 0;
			}
			const int16_t o3 = mulf(sat(mulf(c3, k) + mulf(e7, w)), s);
			const int16_t o4 = mulf(sat(mulf(c4, k) + mulf(y, w)), s);
			const int16_t o5 = mulf(sat(mulf(c5, k) + mulf(e9, w)), s);
			wh(m, a, (mudn((uint16_t)o3, 0x0002) & 0xF800) | (mudl((uint16_t)o4, 0x1000) & 0x07C0) |
			         mudl((uint16_t)o5, 0x0080) | 1);
		}
	} while (r8 != r4);
}

/* A palette from RDRAM to DMEM r4 (r6 + 1 bytes), converted. */
static void load_pal(gbt *g, uint32_t r4, uint32_t r5, uint32_t r6)
{
	dma(g, r4, r5, r6, 0);
	if (g->kind == GBC_CC)
		pal_cc(g, r4, r4 + r6 + 1);
	else
		pal(g, r4, r4 + r6 + 1);
}

/* The row at DMEM 0x200 to output_buff (0x1984). */
static void put_row(gbt *g)
{
	dma(g, 0x200, rw(g->m, 0xFE8) + g->r2, g->r19 - 1, 1);
	g->r2 += g->r19;
}

/* Rows of 0 (0x187C): r8 + 1 of them. */
static void blank_rows(gbt *g, uint32_t r8)
{
	memset(g->m + 0x200, 0, g->r19);
	for (;;) {
		put_row(g);
		if (r8-- == 0)
			break;
	}
}

/* n pixels' bytes from DMEM src through the colors at 0x800 to halfwords at
 * dst, 4 at a time as the program reads and writes them (0x18D4). */
static void border_pixels(unsigned char *m, uint32_t src, uint32_t dst, uint32_t n)
{
	for (uint32_t j = 0; j < n; j += 4) {
		uint32_t c[4];
		for (int k = 0; k < 4; k++)
			c[k] = rh(m, 0x800 + 2 * rb(m, src + j + k));
		for (int k = 0; k < 4; k++)
			wh(m, dst + 2 * (j + k), c[k]);
	}
}

/* A color from a table of halfwords: the one at tab + i (even). */
static inline uint32_t color16(const unsigned char *tab, uint32_t i)
{
	uint16_t v;
	memcpy(&v, tab + (i ^ HX), 2);
	return v;
}

/* n halfwords, two to a word in w, to DMEM dst (even, below 0x1000 - 2 n). */
static void place(unsigned char *m, uint32_t dst, const uint32_t *w, uint32_t n)
{
	if (!(dst & 2)) {
		memcpy(m + dst, w, 2 * n);
		return;
	}
	for (uint32_t k = 0; k < n / 2; k++, dst += 4) {
		wh2(m, dst, w[k] >> 16);
		wh2(m, dst + 2, w[k]);
	}
}

/* The same when the halfwords written miss the bytes read: n bytes from
 * DMEM src (a multiple of 4) converted to w, then placed. */
static void border_pixels_fast(unsigned char *m, uint32_t src, uint32_t dst, uint32_t n)
{
	uint32_t w[128], *o = w;
	const unsigned char *tab = m + 0x800;
	for (uint32_t j = 0; j < n; j += 4) {
		const uint32_t p = rw4(m, src + j);
		*o++ = color16(tab, p >> 23 & 0x1FE) << 16 | color16(tab, p >> 15 & 0x1FE);
		*o++ = color16(tab, p >> 7 & 0x1FE) << 16 | color16(tab, p << 1 & 0x1FE);
	}
	place(m, dst, w, n);
}

/* Whether halfwords at [dst, dst + 2 n) stay below 0x1000 and off DMEM
 * 0x700-0x9FF (the border's bytes and colors). */
static inline int border_apart(uint32_t dst, uint32_t n)
{
	return !(dst & 1) && dst + 2 * n <= 0x1000 && (dst + 2 * n <= 0x700 || dst >= 0xA00);
}

/* A border row from 0xFF8's stream (0x18C0): its bytes (r6 & 0xFF) + 1, then
 * 48 more with an odd r16, through the colors at 0x800 to the row at
 * DMEM 0x200 + r18 - 96 (the 48 more at 0x200 + r18 + 320); with r16 2,
 * ucode_data's next row over it, its pixels that are not 0. */
static void border_row(gbt *g, uint32_t r6, int32_t r16)
{
	unsigned char *m = g->m;
	const uint32_t r5 = rw(m, 0xFF8);
	dma(g, 0x700, r5, r6, 0);
	const uint32_t n = ((r6 & 0xFF) >> 2) * 4 + 4, at = (0x200 + g->r18 - 96) & 0xFFF, at2 = (at + 2 * n + 320) & 0xFFF;
	const int odd = r16 & 1;
	if (border_apart(at, n) && (!odd || border_apart(at2, 48))) {
		border_pixels_fast(m, 0x700, at, n);
		if (odd)
			border_pixels_fast(m, 0x700 + n, at2, 48);
	} else {
		border_pixels(m, 0x700, at, n);
		if (odd)
			border_pixels(m, 0x700 + n, at2, 48);
	}
	r16 -= odd ? 2 : 1;
	ww(m, 0xFF8, r5 + 256);
	if (r16 > 0) {
		dma(g, 0x480, g->r12, g->r19 - 1, 0);
		g->r12 += g->r19;
		for (uint32_t a = 0; a < g->r19; a += 2) {
			const uint32_t v = rh2(m, 0x480 + a);
			if (v)
				wh2(m, 0x200 + a, v);
		}
	}
	put_row(g);
}

/* A Game Boy line's 160 bytes at DMEM 0 to the row at 0x200 + r18 (0x1584):
 * GBC through the colors at 0x100 + byte. */
static void gbc_pixels(gbt *g)
{
	unsigned char *m = g->m;
	const uint32_t r18 = g->r18;
	if (r18 >= 1 && !(r18 & 1)) { /* the row starts past the colors' last byte, 0x200 */
		uint32_t w[80], *o = w;
		const unsigned char *tab = m + 0x100;
		for (uint32_t j = 0; j < 160; j += 4) {
			const uint32_t p = rw4(m, j);
			if (p & 0x01010101u) { /* an odd byte: a halfword across two */
				*o++ = rh(m, 0x100 + (p >> 24)) << 16 | rh(m, 0x100 + (p >> 16 & 0xFF));
				*o++ = rh(m, 0x100 + (p >> 8 & 0xFF)) << 16 | rh(m, 0x100 + (p & 0xFF));
				continue;
			}
			*o++ = color16(tab, p >> 24) << 16 | color16(tab, p >> 16 & 0xFF);
			*o++ = color16(tab, p >> 8 & 0xFF) << 16 | color16(tab, p & 0xFF);
		}
		place(m, 0x200 + r18, w, 160);
		return;
	}
	for (uint32_t j = 0; j < 160; j += 4) {
		uint32_t c[4];
		for (int k = 0; k < 4; k++)
			c[k] = rh(m, 0x100 + rb(m, j + k));
		for (int k = 0; k < 4; k++)
			wh(m, 0x200 + r18 + 2 * (j + k), c[k]);
	}
}

/* GB: shade << 1 through palette (attribute byte >> 6 - 2k & 3) at 0xF20, a
 * palette 8 pixels at a time, an attribute byte 32 (0x1538 in its variant).
 * The row (at most 0x200-0xB3F) never meets what is read. */
static void gb_pixels(gbt *g, uint32_t r15)
{
	unsigned char *m = g->m;
	const uint32_t r18 = g->r18;
	uint32_t w[80], *o = w;
	for (uint32_t q = 0; q < 20; q++) {
		const unsigned char *tab = m + 0xF20 + (rb(m, r15 + (q >> 2)) >> (6 - 2 * (q & 3)) & 3) * 8;
		for (uint32_t j = 0; j < 8; j += 4) {
			const uint32_t p = rw4(m, 8 * q + j);
			*o++ = color16(tab, p >> 24 & 6) << 16 | color16(tab, p >> 16 & 6);
			*o++ = color16(tab, p >> 8 & 6) << 16 | color16(tab, p & 6);
		}
	}
	if (!(r18 & 1))
		place(m, 0x200 + r18, w, 160);
	else
		for (uint32_t k = 0; k < 80; k++) {
			wh(m, 0x200 + r18 + 4 * k, w[k] >> 16);
			wh(m, 0x202 + r18 + 4 * k, w[k]);
		}
}

int gb_tower_hle(unsigned char *rdram, unsigned char *dmem)
{
	gbt gs, *g = &gs;
	unsigned char *m = dmem;
	const uint32_t size = rw(m, 0xFCC);
	if (size == 0xA10)
		g->kind = GBC;
	else if (size == 0xB30)
		g->kind = GBC_CC;
	else if (size == 0xA60)
		g->kind = GB;
	else
		return 0;
	/* Its tables as built in here, still there when it reads them, after the copy. */
	const uint32_t boot = rw(m, 0xFC8), at = (boot + size) & 0xFFFFF8, copy = rw(m, 0xFEC) & 0xFFFFF8;
	if (at + sizeof(tables) - 1 > RDRAM_LAST || (copy < at + sizeof(tables) && at < copy + 0x6000))
		return 0;
	for (uint32_t i = 0; i < sizeof(tables); i++)
		if (rdram[(at + i) ^ BX] != tables[i])
			return 0;
	/* Its rows must reach data_size's bytes exactly, after the first 40 or so. */
	const uint32_t r3 = rw(m, 0xFF4) & 0xFFFFFF, top = rb(m, 0xFF4), r19 = (rh(m, 0xFFE) & 0xF800) >> 5;
	if (r19 == 0 || r3 % r19 || r3 / r19 < top)
		return 0;
	g->ram = rdram;
	g->m = dmem;

	/* 0x1000: 0x6000 bytes from dram_stack to 0xFEC's address through DMEM,
	 * the task kept aside meanwhile; the line registers and sprites (0x540
	 * bytes) from data_ptr to 0xA00, the tables after the program to 0. */
	unsigned char task[0x40];
	memcpy(task, m + 0xFC0, 0x40);
	uint32_t src = rw(m, 0xFE0), dst = rw(m, 0xFEC);
	const uint32_t data = rw(m, 0xFF0);
	for (int k = 0; k < 6; k++, src += 0x1000, dst += 0x1000) {
		dma(g, 0, src, 0xFFF, 0);
		dma(g, 0, dst, 0xFFF, 1);
	}
	dma(g, 0xA00, data, 0x53F, 0);
	dma(g, 0, boot + size, 0xBF, 0);
	/* (SIG3 set: the input is read.) */
	memcpy(m + 0xFC0, task, 0x40);
	ww(m, 0xFE0, dst + 8); /* the lines' 160 bytes, past the copy */

	/* 0x10D0: 8 passes, each with a quarter of the tile data (rows r and 7 - r
	 * of either tile data area, as LCDC bit 4 picks); the lines whose tile
	 * row is one of those get their background (0x1174), then their window
	 * (0x1210). */
	const uint32_t on = g->kind == GB ? 1 : 0; /* GB: LCDC bit 0 as well */
	for (uint32_t r7 = 19;;) {
		uint32_t r9 = rw(m, 0xFEC);
		const uint32_t r10 = (r7 & 3) * 0x600;
		if (r7 & 0x10) {
			r9 += 0x1000;
			dma(g, 0x200, r9 + r10, 0x100011FF, 0);
			dma(g, 0x600, r9 + 0x2A00 - r10, 0x100011FF, 0);
		} else {
			r9 += 0x1200;
			dma(g, 0x200, r9 + r10, 0xFF, 0);
			dma(g, 0x400, r9 + r10 + 0x300, 0xFF, 0);
			dma(g, 0x600, r9 + 0x2A00 - r10, 0xFF, 0);
			dma(g, 0x800, r9 + 0x2A00 - r10 + 0x300, 0xFF, 0);
		}
		const uint32_t r2 = r7 & 7, r3r = r2 ^ 7;
		uint32_t r29 = 0xFFFFFFFFu, r15;
		uint32_t r16 = rb(m, 0xE79), r17 = (r7 & 0x10) | 0x80 | on;
		for (uint32_t r28 = 0x480; r28 != 0;) {
			const uint32_t r8 = r16 & 7;
			r16 = rb(m, 0x9F1 + r28);
			r28 -= 8;
			if (r8 != r2 && r8 != r3r)
				continue;
			const uint32_t r9l = rb(m, 0xA00 + r28), r10l = rb(m, 0xA01 + r28);
			r15 = (r8 & 4) << 8;
			if ((r9l & (0x90 | on)) != r17)
				continue;
			const uint32_t r11 = (((r9l & 8) << 5 | r10l) >> 3) << 6;
			if (r11 != r29)
				dma(g, 0xF80, rw(m, 0xFEC) + r11, 0x3F, 0);
			r29 = r11;
			const uint32_t scx = rb(m, 0xA06 + r28);
			bg_line(g, r28, (scx & 7) ^ 7, rb(m, 0xA03 + r28), (scx >> 3) << 1, r15);
		}
		r16 = rb(m, 0xE7A);
		r17 = (r7 & 0x10) | 0xA0 | on;
		for (uint32_t r28 = 0x480; r28 != 0;) {
			const uint32_t r8 = r16 & 7;
			r16 = rb(m, 0x9F2 + r28);
			r28 -= 8;
			if (r8 != r2 && r8 != r3r)
				continue;
			const uint32_t r9l = rb(m, 0xA00 + r28), r10l = rb(m, 0xA02 + r28);
			r15 = (r8 & 4) << 8;
			if ((r9l & (0xB0 | on)) != r17)
				continue;
			const uint32_t r11 = ((((r9l & 0x40) << 2) | r10l) >> 3) << 6;
			if (r11 != r29)
				dma(g, 0xF80, rw(m, 0xFEC) + r11, 0x3F, 0);
			r29 = r11;
			const uint32_t r18 = rb(m, 0xA03 + r28);
			if ((int32_t)(r18 - 167) < 0)
				bg_line(g, r28, r18, 167, 0, r15);
		}
		if (r7 == 0)
			break;
		r7 = (r7 & 0x10) ? r7 - 16 : r7 + 15;
	}

	/* GB (0x12BC in its variant): lines with the background off get BGP's
	 * shade 0, from the last line's BGP. */
	if (g->kind == GB) {
		memset(m, (rb(m, 0xE7F) & 3) << 1, 160);
		uint32_t r5 = rw(m, 0xFE0), r8 = rb(m, 0xA00);
		for (uint32_t r28 = 0xFFFFFB80u; r28 != 0; r5 += 160) {
			const uint32_t off = !(r8 & 1);
			r8 = rb(m, 0xE88 + r28);
			r28 += 8;
			if (off)
				dma(g, 0, r5, 159, 1);
		}
	}

	/* 0x12BC: the 40 sprites, in OAM order; 8x16 when the first line's LCDC
	 * says so, drawn as its top tile and then its bottom one. */
	for (int32_t r2 = -156;; r2 += 4) {
		uint32_t r16 = rb(m, 0xF1C + r2);
		const uint32_t r18 = rb(m, 0xF1D + r2), r3 = rb(m, 0xF1F + r2), r19 = rb(m, 0xF1E + r2);
		const int shown = r16 - 1 < 159 && r18 - 1 < 167;
		r16 -= 16;
		if (shown) {
			int32_t r7 = 7;
			if ((int32_t)r16 < 0) {
				r7 += (int32_t)r16;
				r16 = 0;
			}
			const uint32_t lcdc = rb(m, 0xA00 + (r16 << 3));
			const uint32_t bank = g->kind == GB ? 0 : (r3 & 8) << 9;
			dma(g, 0xF80, rw(m, 0xFEC) + ((r19 & 0xFE) << 4) + bank + 0x4000, 0x1F, 0);
			const uint32_t r8 = ((r3 & 0x40) >> 5) - 1, r20 = r8 << 1;
			uint32_t r29 = r8 & 0xE;
			if (lcdc & 4) {
				r29 |= (r3 & 0x40) >> 2;
				const uint32_t r15 = r29 ^ 0x10;
				if (r7 >= 0)
					obj_lines(g, r16, r7, r18, r16 << 3, r3, &r29, r20);
				r16 = rb(m, 0xF1C + r2) - 8;
				r29 = r15;
				r7 = 7;
			} else
				r29 |= (r19 & 1) << 4;
			if (r7 >= 0)
				obj_lines(g, r16, r7, r18, r16 << 3, r3, &r29, r20);
		}
		if (r2 >= 0)
			break;
	}

	/* 0x1398: the border's colors from 0xFF8 to 0x800, by 0xFFC's brightness. */
	{
		const uint32_t r5 = rw(m, 0xFF8);
		ww(m, 0xFF8, r5 + 0x200);
		dma(g, 0x800, r5, 0x1FF, 0);
		const uint16_t br = (uint16_t)(rb(m, 0xFFC) << 9);
		for (uint32_t a = 0x800; a < 0xA00; a += 2) {
			const uint32_t v0 = rh2(m, a);
			const uint16_t v = (mudl(v0 & 0xF800, br) & 0x7C00) | mudl(v0 & 0x003E, br) | (mudl(v0 & 0x07C0, br) & 0x03E0);
			wh2(m, a, (uint16_t)(v + v) | 1);
		}
	}

	/* 0x1404: the rows. Rows of 0 past the first 40 of data_size's top
	 * byte, the rest of those 40 from the border; then each Game Boy line
	 * between 48 border pixels each side; then up to 40 border rows, the
	 * last 0xFDE of them (after 0xFDF) with ucode_data's rows over them;
	 * then rows of 0, until the rows' bytes are data_size's. */
	g->r2 = 0;
	g->r19 = r19;
	g->r18 = rh(m, 0xFFE) & 0x7FF;
	uint32_t r29 = rw(m, 0xFE0);
	if (top >= 41)
		blank_rows(g, top - 41);
	memset(m + 0x200, 0, r19);
	for (uint32_t r7 = top < 40 ? top : 40; r7; r7--)
		border_row(g, 0xFF, 0);

	const uint32_t scale = rb(m, 0xFFD);
	g->s[0] = (uint16_t)(scale << 2);
	g->s[1] = (uint16_t)(scale << 5);
	g->s[2] = (uint16_t)(scale << 3);
	if (g->kind == GBC_CC) {
		const uint32_t cc = rb(m, 0xFDD);
		g->s[3] = (uint16_t)((scale << 9) - (scale == 0x40));
		g->s[4] = (uint16_t)((cc & 0x7F) << 8);
		g->s[5] = (uint16_t)(0x7F00 - g->s[4]);
	}
	uint32_t r15 = 0xA0;
	if (g->kind == GB) { /* the 4 palettes at 0xF20, the attribute map at 0xA0 */
		pal(g, 0xF20, 0xF40);
		dma(g, 0xA0, rw(m, 0xFF0) + 0x540, 95, 0);
	}
	uint32_t r20 = 0xFFFFFFFFu; /* GBC: the palettes loaded, BGP's << 0 and OBP's << 8 */
	for (uint32_t r28 = 0xFFFFFB88u;; r28 += 8) {
		if (g->r2 == r3)
			goto done;
		dma(g, 0, r29, 159, 0);
		r29 += 160;
		if (g->kind == GB) {
			gb_pixels(g, r15);
			r15 += 5;
		} else {
			if (!(rb(m, 0xE78 + r28) & 0x80)) { /* the LCD off: palette 0's first color */
				if (r20 != 0) {
					load_pal(g, 0x100, rw(m, 0xFF0) + 0x5E0, 15);
					const uint32_t c = rh2(m, 0x100);
					for (uint32_t a = 0x100; a < 0x200; a += 2)
						wh2(m, a, c);
				}
				r20 = 0;
			} else {
				const uint32_t r9 = rh(m, 0xE7C + r28) ^ r20;
				if (r9 & 0xFF) {
					r20 ^= r9 & 0xFF;
					load_pal(g, 0x100, rw(m, 0xFF0) + ((r20 & 0xFF) << 6) + 0x5A0, 63);
					memcpy(m + 0x180, m + 0x100, 0x40);
				}
				if (r9 & 0xFF00) {
					r20 ^= r9 & 0xFF00;
					load_pal(g, 0x140, rw(m, 0xFF0) + ((r20 & 0xFF00) >> 2) + 0x29A0, 63);
					memcpy(m + 0x1C0, m + 0x140, 0x40);
				}
			}
			gbc_pixels(g);
		}
		border_row(g, 0x0A00102F, 1);
		if (g->kind == GB && (r28 & 0x38))
			r15 -= 5; /* an attribute row is 8 lines */
		if (r28 == 0)
			break;
	}
	{
		int32_t r7 = 40, r14 = rb(m, 0xFDE), r24 = rb(m, 0xFDF);
		g->r12 = rw(m, 0xFD8);
		while (g->r2 != r3) {
			int32_t r16 = 0;
			if (r24-- <= 0 && r14-- > 0)
				r16 = 2;
			if (r7-- > 0)
				border_row(g, 0xFF, r16);
			else
				blank_rows(g, 0);
		}
	}
done:
	ww(m, 0xFFC, 0); /* DPC_CLOCK's ticks */
	return 1;
}
