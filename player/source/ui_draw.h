// Our64 player: what ui_draw.cpp (the screens as drawing) needs from the
// platform that shows them: ui_wiiu.cpp on the console, tests/ui_shots.cpp
// on a PC.
#ifndef OUR64_UI_DRAW_H
#define OUR64_UI_DRAW_H
#include "libgui/Gui.h"
#include "ui.h"

namespace our64ui {

// Colors come from the app's theme (system.h: SYSTEM.theme), one surface
// per background: ui_draw.cpp takes each text's color from the surface under
// it, so no screen can put dark text on the blue.

// Button glyphs in the system font (the ones the Aroma config menu uses).
#define FONT_A ""
#define FONT_DPAD_LR ""
#define FONT_HOME ""

// What tests/ui_shots.cpp checks each draw against; nullptr on the console.
struct draw_checks {
	virtual void box(float x, float y, float w, float h, PixelColor c, float stroke) = 0;
	virtual void text(float x, float y, const char * s, int px, int width, uint32_t justify, PixelColor c) = 0;
	// fit() at its smallest still too wide (a width), or wrap_fit() lines cut off (a count)
	virtual void too_long(const char * s, int px, int got, int room) = 0;
};
extern draw_checks * g_checks;

// Set by the platform before the first frame.
extern ImageRenderer * g_images;          // fontSystem (libgui's) draws the text
extern void * g_cat, * g_cat2, * g_pad;   // the portrait, the loading cat, the app's controller photo (nullptr: none)
extern int g_cat2_w, g_cat2_h, g_pad_w, g_pad_h; // their sizes in pixels
extern notices g_notices;

// The platform's side.
double now_us();
void lock_notices();
void unlock_notices();
void * frozen_texture();                  // the paused game's last frame, nullptr when none

void draw_screen(const ui_view & v);      // one screen, whole
void draw_tv_card(const char * title);    // GamePad only: the TV's card
void draw_notices(bool paused);

} // namespace our64ui
#endif
