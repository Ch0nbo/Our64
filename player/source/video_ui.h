// Our64 player: what video.cpp (GLideN64's frames) and ui_wiiu.cpp (Our64's
// own screens) share. Everything here runs on the graphics core, except
// video_call, which runs a function there from any thread and waits for it.
#pragma once
#include <gx2/surface.h>

void video_call(void (*fn)(void *), void * arg);

// video.cpp
GX2ColorBuffer * video_last_frame(); // the last game frame presented; nullptr before the first
bool video_game_started();           // GLideN64 is running: give it its GPU state back after drawing
void video_set_hold(bool hold);      // the UI owns the screens: game frames are not shown

// ui_wiiu.cpp
GX2ColorBuffer * ui_gfx_overlay(GX2ColorBuffer * frame); // the frame with the notices on, or frame
void ui_gfx_loading_progress(unsigned done, unsigned total); // GLideN64 builds saved shaders: the loading screen moves
void ui_gfx_scan(GX2ColorBuffer * frame);                // copies a game frame to the screens the mode uses
void ui_gfx_screens_changed();                           // new scan buffers: the GamePad-only card is copied again
void ui_gfx_set_picture_43(bool on);                     // Picture: 4:3 (black at the sides) or stretched
void ui_gfx_set_shader(int shader);                      // Picture's Filter (SHADER_, prompt.h): the filters are drawn here
void ui_gfx_game_stopped();                              // the game ended: the filters' buffers are freed
bool ui_gfx_copy_frame(GX2ColorBuffer * frame, GX2ColorBuffer * to); // a GPU copy into to, (re)allocated to frame's size

// tv_pass.cpp: frame drawn through a filter (SHADER_SCANLINES and on) into
// out, at out's size: the 4:3 middle (picture_43) or all of it. With clear,
// out is made black first. gamepad: out is the GamePad's picture, its lines
// locked to the screen's rows (tv_mesh.h). False if the shader could not be built.
bool tv_pass_draw(GX2ColorBuffer * frame, GX2ColorBuffer * out, int shader, bool picture_43, bool clear, bool gamepad);
void tv_pass_game_stopped(); // the next game's GPU time is logged again
// What tv_pass.cpp keeps on the heap for the session (its GPU state, the
// compiled shader pairs, CRT Curved's grid): not the TV's buffer, which is
// ui_wiiu.cpp's. A shader switch allocates none of it again.
unsigned tv_pass_held_bytes();
