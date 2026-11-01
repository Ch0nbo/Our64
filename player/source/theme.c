/* Our64 player: its palette, and colors worked out from it (see theme.h).
 * The worked-out part is the shared UI's ui/theme.c, unchanged. */
#include "theme.h"
#include "prompt.h"

#include <math.h>

/* Standard (Zach, 2026-10-30): OurGB's two greys, slate screens and dark
 * slate bars, each fading top to bottom, with the Nintendo 64 logo's blue for
 * the selection in place of OurGB's magenta, and its green for progress bars
 * and the badge, with a pale tint of that green for highlights, the palest
 * that still reads at 4.5:1 on the slate (Zach, 2026-10-30). */
#define STANDARD { \
	.screen = { { 88, 90, 112 }, { 246, 246, 250 }, { 216, 218, 232 }, { 182, 223, 195 } }, \
	.bar = { { 40, 40, 54 }, { 246, 246, 250 }, { 196, 198, 214 }, { 182, 223, 195 } }, \
	.select = { { 0, 30, 168 }, { 255, 255, 255 }, { 214, 224, 255 }, { 214, 224, 255 } }, \
	.dim = { { 74, 76, 96 }, { 214, 216, 228 }, { 214, 216, 228 }, { 214, 216, 228 } }, \
	.panel = { { 246, 246, 250 }, { 20, 20, 30 }, { 56, 56, 72 }, { 0, 30, 168 } }, \
	.fail = { { 128, 20, 28 }, { 246, 246, 250 }, { 255, 200, 188 }, { 255, 200, 188 } }, \
	.lit = { 6, 146, 48 }, \
	.style = { .grad = 1, .screen_to = { 62, 64, 84 }, .bar_to = { 26, 26, 36 }, .select_to = { 0, 20, 124 } }, \
}
const our_theme OUR64_STANDARD = STANDARD;

/* Comrade: Our64's red and gold (2026-10-16), the colors its screens had before they
 * took the shared drawing: a red screen, darker red bars and buttons, the
 * selection red in a cream frame, cream panels. Muted words and gold made
 * lighter on 2026-10-29 so both read at 4.5:1 or more on the red (they were
 * 3.5:1 and 4.2:1). */
const our_theme OUR64_COMRADE = {
	.screen = { { 163, 28, 26 }, { 242, 230, 201 }, { 217, 198, 179 }, { 244, 194, 73 } },
	.bar = { { 104, 14, 12 }, { 242, 230, 201 }, { 217, 198, 179 }, { 244, 194, 73 } },
	.select = { { 163, 28, 26 }, { 242, 230, 201 }, { 217, 198, 179 }, { 244, 194, 73 } },
	.dim = { { 80, 30, 28 }, { 196, 170, 160 }, { 196, 170, 160 }, { 196, 170, 160 } },
	.panel = { { 242, 230, 201 }, { 34, 22, 18 }, { 92, 72, 68 }, { 163, 28, 26 } },
	.fail = { { 104, 14, 12 }, { 242, 230, 201 }, { 217, 198, 179 }, { 244, 194, 73 } },
	.lit = { 244, 194, 73 },
};

our_theme g_theme = STANDARD;
static int g_comrade;

void theme_use(int theme)
{
	g_comrade = theme == THEME_COMRADE;
	g_theme = g_comrade ? OUR64_COMRADE : OUR64_STANDARD;
}

int theme_comrade(void) { return g_comrade; }

static int g_list;
void theme_use_layout(int layout) { g_list = layout == LAYOUT_LIST; }
int theme_roms_list(void) { return g_list; }

our_rgb theme_mix(our_rgb a, our_rgb b, float t)
{
	our_rgb c = { (unsigned char)lroundf(a.r + (b.r - a.r) * t), (unsigned char)lroundf(a.g + (b.g - a.g) * t),
	              (unsigned char)lroundf(a.b + (b.b - a.b) * t) };
	return c;
}

static double lum(our_rgb c)
{
	double v[3] = { c.r / 255.0, c.g / 255.0, c.b / 255.0 };
	for (int i = 0; i < 3; i++)
		v[i] = v[i] <= 0.03928 ? v[i] / 12.92 : pow((v[i] + 0.055) / 1.055, 2.4);
	return 0.2126 * v[0] + 0.7152 * v[1] + 0.0722 * v[2];
}

double theme_contrast(our_rgb a, our_rgb b)
{
	double x = lum(a) + 0.05, y = lum(b) + 0.05;
	return x > y ? x / y : y / x;
}

our_surface theme_faded(const our_theme *t)
{
	our_surface f = t->bar;
	f.fill = theme_mix(t->bar.fill, t->screen.fill, 0.5f);
	f.ink = theme_off(&f);
	f.muted = f.accent = f.ink;
	return f;
}

our_rgb theme_off(const our_surface *s)
{
	/* the contrast falls as the ink nears the fill: halve toward the last
	 * point that still reads at THEME_OFF_CONTRAST */
	float lo = 0, hi = 1;
	for (int i = 0; i < 24; i++) {
		float t = (lo + hi) / 2;
		if (theme_contrast(theme_mix(s->ink, s->fill, t), s->fill) >= THEME_OFF_CONTRAST)
			lo = t;
		else
			hi = t;
	}
	return theme_mix(s->ink, s->fill, lo);
}
