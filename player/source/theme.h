/* Our64 player: its palette in surfaces, and the colors worked out from it,
 * the shared UI's (OurConsoles ui/system.h and ui/theme.h) so Our64's
 * drawing stays the shared ui_draw.cpp with only Our64's own words. */
#ifndef OUR64_THEME_H
#define OUR64_THEME_H

#ifdef __cplusplus
extern "C" {
#endif

typedef struct { unsigned char r, g, b; } our_rgb;
/* A background and the colors that go on it. Drawing code takes text
 * colors from the surface it draws on, never from the palette. */
typedef struct {
	our_rgb fill;
	our_rgb ink;     /* text */
	our_rgb muted;   /* second lines */
	our_rgb accent;  /* highlights (Our64's gold) */
	/* The words of something disabled for now are not a color of their own:
	 * theme_off works them out from ink and fill. */
} our_surface;
typedef struct {
	our_surface screen;  /* the main background, dialogs; and the light words over the dimmed game */
	our_surface bar;     /* bars, legend, buttons, cards */
	our_surface select;  /* the selected item */
	our_surface dim;     /* a cover still on its way from the SD card */
	our_surface panel;   /* light panels and frames */
	our_surface fail;    /* what failed */
	our_rgb lit;         /* lit screens, progress, the badge */
	/* grad: the screen, bar and selection fade top to bottom to their _to
	 * color; glow: raised things glow in glow_color instead of a shadow. */
	struct {
		int grad;
		our_rgb screen_to, bar_to, select_to;
		int glow;
		our_rgb glow_color;
	} style;
} our_theme;

/* Our64's two themes (Settings > Theme, Zach, 2026-10-30): Standard, the
 * default, and Comrade, the red and gold with the portrait and its words.
 * THEME_ ids are prompt.h's. theme_use copies one into g_theme, which every
 * screen draws from; the next frame shows it. */
extern const our_theme OUR64_STANDARD, OUR64_COMRADE;
extern our_theme g_theme;
void theme_use(int theme);
int theme_comrade(void); /* the portrait and "comrade" show only in Comrade */
/* Our ROMs' layout (LAYOUT_, prompt.h): the row of covers, or the list as it was. */
void theme_use_layout(int layout);
int theme_roms_list(void);

/* a moved toward b: t 0 is a, 1 is b */
our_rgb theme_mix(our_rgb a, our_rgb b, float t);
/* WCAG 2's contrast ratio, 1 to 21 */
double theme_contrast(our_rgb a, our_rgb b);
/* The words of something disabled for now on s (Zach, 2026-10-23: the
 * normal button with its words dimmed, from the theme's own colors): s's
 * ink moved toward s's fill as far as it goes while it still reads at
 * THEME_OFF_CONTRAST. */
#define THEME_OFF_CONTRAST 3.2 /* the ruling's 3:1, with room for rounding */
our_rgb theme_off(const our_surface *s);
/* A button that does nothing now, faded (Zach, 2026-10-23, after Nielsen's
 * "inactive buttons": a muted version of the button, not a new color): the
 * bar mixed half into the screen behind it, its words the bar's ink dimmed
 * on that (theme_off), still 3:1 or more. */
our_surface theme_faded(const our_theme *t);

#ifdef __cplusplus
}
#endif
#endif
