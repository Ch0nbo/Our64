// Every Our64 screen drawn on a PC, with the console's own drawing code
// (player/source/ui_draw.cpp), libgui and the console's system font, into
// PNGs; and every text checked against the box it sits in.
//
//   make -C tests shots   (needs results/font/system-font.ttf from the
//                          console's sd:/wiiu/our64/debug/system-font.ttf)
//
// Writes results/shots/<screen>.png and prints each text that does not fit:
// past the edge of its box (with PAD of room), past the screen, cut short
// by fit() or wrap_fit(). Exit status 1 when any did.
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

#include <png.h>
#include <turbojpeg.h>

#include "ui_draw.h"
#include "covers.h"
#include "theme.h"
extern "C" {
#include "states.h"
}

using namespace our64ui;

namespace {

int K = 2;              // PNG pixels per canvas pixel
float g_ui_scale = 2.25f; // the console's text scale for a 1080p TV (1.5 at 720p)
const float PAD = 8;    // room a text keeps from the edge of its box

// ---- a canvas in software ----

std::vector<float> g_px; // RGB, 0..1, UI_W*K x UI_H*K

void blend(int x, int y, float r, float g, float b, float a)
{
	if (x < 0 || y < 0 || x >= UI_W * K || y >= UI_H * K || a <= 0)
		return;
	float * p = &g_px[(size_t(y) * UI_W * K + x) * 3];
	a = a > 1 ? 1 : a;
	p[0] += (r - p[0]) * a;
	p[1] += (g - p[1]) * a;
	p[2] += (b - p[2]) * a;
}

struct Tex {
	int w, h, ch; // ch 4: RGBA; 1: coverage
	std::vector<uint8_t> px;
	float at(float u, float v, int c) const // bilinear, u v in texels
	{
		u -= 0.5f; v -= 0.5f;
		int x0 = int(floorf(u)), y0 = int(floorf(v));
		float fx = u - x0, fy = v - y0;
		auto get = [&](int x, int y) {
			x = x < 0 ? 0 : x >= w ? w - 1 : x;
			y = y < 0 ? 0 : y >= h ? h - 1 : y;
			return px[(size_t(y) * w + x) * ch + c] / 255.0f;
		};
		return (get(x0, y0) * (1 - fx) + get(x0 + 1, y0) * fx) * (1 - fy) +
		       (get(x0, y0 + 1) * (1 - fx) + get(x0 + 1, y0 + 1) * fx) * fy;
	}
};

void draw_tex(const Tex * t, float x, float y, float w, float h, float alpha, const PixelColor * tint)
{
	if (t == nullptr || w <= 0 || h <= 0)
		return;
	for (int py = int(floorf(y * K)); py < int(ceilf((y + h) * K)); py++)
		for (int px = int(floorf(x * K)); px < int(ceilf((x + w) * K)); px++) {
			float u = ((px + 0.5f) / K - x) / w * t->w, v = ((py + 0.5f) / K - y) / h * t->h;
			if (u < 0 || v < 0 || u > t->w || v > t->h)
				continue;
			if (t->ch == 1)
				blend(px, py, tint->r / 255.0f, tint->g / 255.0f, tint->b / 255.0f, t->at(u, v, 0) * tint->a / 255.0f);
			else
				blend(px, py, t->at(u, v, 0), t->at(u, v, 1), t->at(u, v, 2), t->at(u, v, 3) * alpha);
		}
}

class Images : public ImageRenderer
{
public:
	void * createTexture(int w, int h) override { return new Tex{ w, h, 4, std::vector<uint8_t>(size_t(w) * h * 4) }; }
	void loadTextureData(void * t, const uint8_t * rgba, int w, int h) override
	{
		Tex * x = static_cast<Tex *>(t);
		x->w = w; x->h = h;
		x->px.assign(rgba, rgba + size_t(w) * h * 4);
	}
	void destroyTexture(void * t) override { delete static_cast<Tex *>(t); }
	void drawTexture(void * t, float x, float y, uint16_t w, uint16_t h, float, float sx, float sy, uint8_t alpha) override
	{
		draw_tex(static_cast<Tex *>(t), x, y, w * sx, h * sy, alpha / 255.0f, nullptr);
	}
	void drawRectangle(float x, float y, float w, float h, PixelColor c) override
	{
		MeshVertex v[6] = { { x, y, c }, { x + w, y, c }, { x + w, y + h, c }, { x, y, c }, { x + w, y + h, c }, { x, y + h, c } };
		drawMesh(v, 6);
	}
	void drawMesh(const MeshVertex * v, size_t n) override
	{
		for (size_t i = 0; i + 2 < n; i += 3)
			triangle(v[i], v[i + 1], v[i + 2]);
	}

private:
	static void triangle(MeshVertex a, MeshVertex b, MeshVertex c)
	{
		const float ax = a.x * K, ay = a.y * K, bx = b.x * K, by = b.y * K, cx = c.x * K, cy = c.y * K;
		const float area = (bx - ax) * (cy - ay) - (cx - ax) * (by - ay);
		if (fabsf(area) < 1e-6f)
			return;
		int x0 = int(floorf(fminf(ax, fminf(bx, cx)))), x1 = int(ceilf(fmaxf(ax, fmaxf(bx, cx))));
		int y0 = int(floorf(fminf(ay, fminf(by, cy)))), y1 = int(ceilf(fmaxf(ay, fmaxf(by, cy))));
		for (int y = y0; y <= y1; y++)
			for (int x = x0; x <= x1; x++) {
				const float px = x + 0.5f, py = y + 0.5f;
				float w0 = ((bx - px) * (cy - py) - (cx - px) * (by - py)) / area;
				float w1 = ((cx - px) * (ay - py) - (ax - px) * (cy - py)) / area;
				float w2 = 1 - w0 - w1;
				if (w0 < -1e-5f || w1 < -1e-5f || w2 < -1e-5f)
					continue;
				auto mix = [&](uint8_t PixelColor::*ch) { return (w0 * (a.c.*ch) + w1 * (b.c.*ch) + w2 * (c.c.*ch)) / 255.0f; };
				blend(x, y, mix(&PixelColor::r), mix(&PixelColor::g), mix(&PixelColor::b), mix(&PixelColor::a));
			}
	}
};

class Glyphs : public GlyphRenderer
{
public:
	void * createTexture(uint16_t w, uint16_t h) override { return new Tex{ w, h, 1, std::vector<uint8_t>(size_t(w) * h) }; }
	void loadTextureData(void * t, FT_Bitmap * b) override
	{
		Tex * x = static_cast<Tex *>(t);
		const int pitch = b->pitch < 0 ? -b->pitch : b->pitch;
		// just the glyph's own pixels: drawQuad stretches them over the quad
		x->w = b->width > 0 ? int(b->width) : 1;
		x->h = b->rows > 0 ? int(b->rows) : 1;
		std::vector<uint8_t> tight(size_t(x->w) * x->h);
		for (int y = 0; y < x->h; y++)
			for (int i = 0; i < x->w; i++)
				tight[size_t(y) * x->w + i] = int(b->width) > i && int(b->rows) > y ? b->pixel_mode == FT_PIXEL_MODE_MONO
				                              ? ((b->buffer[y * pitch + i / 8] >> (7 - i % 8)) & 1) * 255 : b->buffer[y * pitch + i] : 0;
		x->px = tight;
	}
	void destroyTexture(void * t) override { delete static_cast<Tex *>(t); }
	void drawQuad(void * t, float x, float y, float w, float h, const PixelColor & c) override
	{
		draw_tex(static_cast<Tex *>(t), x, y, w, h, 1, &c);
	}
	void drawFeature(int16_t x, int16_t y, uint16_t w, uint16_t h, const PixelColor & c) override
	{
		MeshVertex v[6] = { { float(x), float(y), c }, { float(x + w), float(y), c }, { float(x + w), float(y + h), c },
		                    { float(x), float(y), c }, { float(x + w), float(y + h), c }, { float(x), float(y + h), c } };
		Images().drawMesh(v, 6);
	}
};

Images g_img_renderer;
Glyphs g_glyph_renderer;

bool write_png(const std::string & path)
{
	std::vector<uint8_t> rgb(g_px.size());
	for (size_t i = 0; i < g_px.size(); i++)
		rgb[i] = uint8_t(lroundf(fminf(fmaxf(g_px[i], 0), 1) * 255));
	png_image im;
	memset(&im, 0, sizeof(im));
	im.version = PNG_IMAGE_VERSION;
	im.width = UI_W * K;
	im.height = UI_H * K;
	im.format = PNG_FORMAT_RGB;
	return png_image_write_to_file(&im, path.c_str(), 0, rgb.data(), 0, nullptr) != 0;
}

std::string read_file(const std::string & path)
{
	std::string s;
	if (FILE * f = fopen(path.c_str(), "rb")) {
		char buf[65536];
		size_t n;
		while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
			s.append(buf, n);
		fclose(f);
	}
	return s;
}

Tex * load_image(const std::string & path)
{
	std::string data = read_file(path);
	if (data.empty())
		return nullptr;
	if (path.size() > 4 && path.compare(path.size() - 4, 4, ".png") == 0) {
		png_image im;
		memset(&im, 0, sizeof(im));
		im.version = PNG_IMAGE_VERSION;
		if (!png_image_begin_read_from_memory(&im, data.data(), data.size()))
			return nullptr;
		im.format = PNG_FORMAT_RGBA;
		Tex * t = new Tex{ int(im.width), int(im.height), 4, std::vector<uint8_t>(PNG_IMAGE_SIZE(im)) };
		png_image_finish_read(&im, nullptr, t->px.data(), 0, nullptr);
		return t;
	}
	tjhandle j = tjInitDecompress();
	int w = 0, h = 0, sub = 0, cs = 0;
	Tex * t = nullptr;
	if (tjDecompressHeader3(j, (const unsigned char *)data.data(), data.size(), &w, &h, &sub, &cs) == 0) {
		t = new Tex{ w, h, 4, std::vector<uint8_t>(size_t(w) * h * 4) };
		tjDecompress2(j, (const unsigned char *)data.data(), data.size(), t->px.data(), w, 0, h, TJPF_RGBA, 0);
	}
	tjDestroy(j);
	return t;
}

// ---- the checks ----

struct Rect { float x, y, w, h; PixelColor c; float stroke; };

struct Checks : draw_checks {
	std::string screen;
	std::vector<Rect> boxes;
	int problems = 0;
	FILE * words = nullptr; // every text drawn, "<screen>\t<text>": words.txt beside the shots
	// Every fill since the last whole-screen opaque one, for what is under a
	// text: contrast.txt lists each text under 4.5:1 against it.
	std::vector<Rect> fills;
	FILE * low = nullptr;
	int lows = 0;
	void box(float x, float y, float w, float h, PixelColor c, float stroke) override
	{
		if (w >= UI_W && h >= UI_H)
			boxes.clear(); // a whole-screen fill or dim: what was under it is out of play
		if (w > 0 && h > 0 && (c.a > 0 || stroke > 0))
			boxes.push_back(Rect{ x, y, w, h, c, stroke });
		if (w >= UI_W && h >= UI_H && c.a == 255)
			fills.clear();
		if (w > 0 && h > 0 && c.a > 0)
			fills.push_back(Rect{ x, y, w, h, c, stroke });
	}
	static double lum(double r, double g, double b)
	{
		double v[3] = { r / 255, g / 255, b / 255 };
		for (double & k : v)
			k = k <= 0.03928 ? k / 12.92 : pow((k + 0.055) / 1.055, 2.4);
		return 0.2126 * v[0] + 0.7152 * v[1] + 0.0722 * v[2];
	}
	// The color under x,y: the fills that hold it, blended in order from the
	// last opaque one. False when no opaque fill holds it (a picture, or the
	// frozen game, which the shots don't see as fills).
	bool under(float x, float y, double out[3])
	{
		int from = -1;
		for (int i = 0; i < int(fills.size()); i++) {
			const Rect & b = fills[i];
			if (x >= b.x && x <= b.x + b.w && y >= b.y && y <= b.y + b.h && b.c.a == 255)
				from = i;
		}
		if (from < 0)
			return false;
		out[0] = fills[from].c.r, out[1] = fills[from].c.g, out[2] = fills[from].c.b;
		for (int i = from + 1; i < int(fills.size()); i++) {
			const Rect & b = fills[i];
			if (!(x >= b.x && x <= b.x + b.w && y >= b.y && y <= b.y + b.h))
				continue;
			const double a = b.c.a / 255.0;
			out[0] = out[0] * (1 - a) + b.c.r * a, out[1] = out[1] * (1 - a) + b.c.g * a, out[2] = out[2] * (1 - a) + b.c.b * a;
		}
		return true;
	}
	void text(float x, float y, const char * s, int px, int width, uint32_t justify, PixelColor c) override
	{
		if (words)
			fprintf(words, "%s\t%s\n", screen.c_str(), s);
		{
			float tx = x;
			if (justify & GUI_TEXT_JUSTIFY_CENTER)
				tx = x - width / 2.0f;
			else if (justify & GUI_TEXT_JUSTIFY_RIGHT)
				tx = x - width;
			double bg[3];
			if (low && under(tx + width / 2.0f, y + px * 0.6f, bg)) {
				const double a = lum(c.r, c.g, c.b) + 0.05, b = lum(bg[0], bg[1], bg[2]) + 0.05;
				const double ratio = a > b ? a / b : b / a;
				if (ratio < 4.5) {
					lows++;
					fprintf(low, "%s\t%.2f\t%s\t%d,%d,%d on %.0f,%.0f,%.0f\n", screen.c_str(), ratio, s, c.r, c.g, c.b, bg[0],
					        bg[1], bg[2]);
				}
			}
		}
		float x0 = x;
		if (justify & GUI_TEXT_JUSTIFY_CENTER)
			x0 = x - width / 2.0f;
		else if (justify & GUI_TEXT_JUSTIFY_RIGHT)
			x0 = x - width;
		const float y0 = y, y1 = y + px * 1.15f, x1 = x0 + width;
		const float cx = (x0 + x1) / 2, cy = (y0 + y1) / 2;
		if (x0 < 0 || x1 > UI_W || y0 < 0 || y1 > UI_H) {
			report("off the screen", s, px, x0, y0, x1, y1, nullptr);
			return;
		}
		// the smallest box drawn so far that holds the text's middle
		const Rect * in = nullptr;
		for (const Rect & b : boxes)
			if (cx >= b.x && cx <= b.x + b.w && cy >= b.y && cy <= b.y + b.h && b.w * b.h < UI_W * UI_H * 0.9f &&
			    (in == nullptr || b.w * b.h < in->w * in->h))
				in = &b;
		if (in == nullptr)
			return; // on the screen's background
		const float pad_x = in->w < 120 ? 4 : PAD, pad_y = in->h < 40 ? 1 : 3;
		if (x0 < in->x + pad_x || x1 > in->x + in->w - pad_x || y0 < in->y + pad_y || y1 > in->y + in->h - pad_y)
			report("past its box", s, px, x0, y0, x1, y1, in);
	}
	void too_long(const char * s, int px, int got, int room) override
	{
		problems++;
		printf("  %-22s cut short at %dpx (%d for room %d): \"%s\"\n", screen.c_str(), px, got, room, s);
	}
	void report(const char * what, const char * s, int px, float x0, float y0, float x1, float y1, const Rect * in)
	{
		problems++;
		if (in != nullptr)
			printf("  %-22s %s: \"%s\" %dpx at %.0f,%.0f-%.0f,%.0f in box %.0f,%.0f %.0fx%.0f\n", screen.c_str(), what, s, px,
			       x0, y0, x1, y1, in->x, in->y, in->w, in->h);
		else
			printf("  %-22s %s: \"%s\" %dpx at %.0f,%.0f-%.0f,%.0f\n", screen.c_str(), what, s, px, x0, y0, x1, y1);
	}
};

Checks g_check;
Tex * g_frozen_tex;
std::string g_out;

void shot(const char * name, const ui_view & v)
{
	g_px.assign(size_t(UI_W) * K * UI_H * K * 3, 0.0f);
	g_check.screen = name;
	g_check.boxes.clear();
	g_check.fills.clear();
	draw_screen(v);
	if (!write_png(g_out + "/" + name + ".png"))
		printf("  could not write %s.png\n", name);
}

// A made-up game picture: sky, hills and a sun, so the frozen frame and thumbnails look like something.
uint16_t g_thumb[CARDS + 1][THUMB_W * THUMB_H];
void fake_picture(uint16_t * out, int seed)
{
	for (int y = 0; y < THUMB_H; y++)
		for (int x = 0; x < THUMB_W; x++) {
			float hill = 70 + 14 * sinf(x * 0.05f + seed) + 8 * sinf(x * 0.13f + seed * 2);
			int r, g, b;
			if (y > hill) { r = 40 + seed * 20; g = 140; b = 50; }
			else { r = 90; g = 150 + y / 2; b = 230; }
			if ((x - 120 + seed * 25) * (x - 120 + seed * 25) + (y - 30) * (y - 30) < 140) { r = 250; g = 220; b = 60; }
			out[y * THUMB_W + x] = uint16_t((r >> 3) << 11 | (g >> 2) << 5 | (b >> 3));
		}
}

Tex * frozen_from_thumb()
{
	Tex * t = new Tex{ THUMB_W, THUMB_H, 4, std::vector<uint8_t>(THUMB_W * THUMB_H * 4) };
	for (int i = 0; i < THUMB_W * THUMB_H; i++) {
		uint16_t c = g_thumb[CARDS][i];
		t->px[i * 4] = uint8_t((c >> 11) << 3);
		t->px[i * 4 + 1] = uint8_t(((c >> 5) & 63) << 2);
		t->px[i * 4 + 2] = uint8_t((c & 31) << 3);
		t->px[i * 4 + 3] = 255;
	}
	return t;
}

const time_t NOW = 1792242000; // 2026-10-17, a Saturday afternoon

pause_view base_pause()
{
	pause_view pv;
	memset(&pv, 0, sizeof(pv));
	pause_open(&pv.ui, 1);
	snprintf(pv.title, sizeof(pv.title), "Legend of Zelda, The - Majora's Mask");
	pause_autosave a = {};
	a.last_ok = NOW - 40;
	pv.autosave_lines = pause_autosave_lines(&a, NOW, 1, pv.autosave, &pv.autosave_red);
	pv.busy_card = -1;
	pv.now = NOW;
	pv.us = 1;
	pv.glyphs = INPUT_PRO;
	pv.now_thumb = g_thumb[CARDS];
	pv.now_gen = 1;
	const int status[CARDS] = { CARD_FILLED, CARD_EMPTY, CARD_FILLED, CARD_FILLED };
	for (int c = 0; c < CARDS; c++) {
		pv.ui.cards[c].status = status[c];
		pv.ui.cards[c].when = NOW - 3600 * (c + 1) * 7;
		pv.ui.cards[c].slot = c < CARD_SLOTS ? c + 1 : 2;
		pv.ui.cards[c].undo = c == CARD_UNDO ? UNDO_LOAD : 0;
		pv.thumb[c] = status[c] == CARD_FILLED ? g_thumb[c] : nullptr;
		pv.thumb_gen[c] = 1;
	}
	return pv;
}

ui_view base_view(int screen)
{
	static ui_view v;
	memset(&v, 0, sizeof(v));
	v.screen = screen;
	snprintf(v.title, sizeof(v.title), "Legend of Zelda, The - Majora's Mask");
	v.glyphs = INPUT_PRO;
	return v;
}

void shots_pause()
{
	static pause_view pv;
	ui_view v = base_view(UI_PAUSE);
	v.pause = &pv;

	pv = base_pause();
	shot("pause-menu", v);
	pv.ui.item = ITEM_EXIT;
	pause_exit_failed(&pv.ui);
	pause_autosave a = {};
	a.last_ok = NOW - 600;
	a.autosave_failed = 1;
	a.ingame_failed = NOW - 30;
	a.exit_failed = 1;
	pv.autosave_lines = pause_autosave_lines(&a, NOW, 1, pv.autosave, &pv.autosave_red);
	pv.ui.failure_shown = pv.autosave_red; // as pause_wiiu.c sets it
	shot("pause-menu-failed", v);

	pv = base_pause();
	pv.ui.screen = PAUSE_SLOTS;
	pv.ui.mode = SLOTS_SAVE;
	pv.ui.card = 1;
	shot("save-state", v);
	pv.ui.card = 0;
	snprintf(pv.ui.line, sizeof(pv.ui.line), "Saved to Slot 1");
	shot("save-state-saved", v);
	pv.ui.busy = 1;
	pv.busy_card = 2;
	pv.ui.card = 2;
	snprintf(pv.busy, sizeof(pv.busy), "Saving to Slot 3...");
	shot("save-state-busy", v);

	pv = base_pause();
	pv.ui.screen = PAUSE_SLOTS;
	pv.ui.mode = SLOTS_LOAD;
	pv.ui.card = 3;
	pv.ui.cards[1].status = CARD_DAMAGED;
	shot("load-state", v);
	pv.ui.card = 1;
	shot("load-state-damaged", v);

	const struct { const char * name; int ask, from, mode, card; } asks[] = {
		{ "ask-overwrite", ASK_OVERWRITE, PAUSE_SLOTS, SLOTS_SAVE, 0 },
		{ "ask-load", ASK_LOAD, PAUSE_SLOTS, SLOTS_LOAD, 2 },
		{ "ask-load-undo", ASK_LOAD, PAUSE_SLOTS, SLOTS_LOAD, 3 },
		{ "ask-delete", ASK_DELETE, PAUSE_SLOTS, SLOTS_LOAD, 0 },
		{ "ask-reset", ASK_RESET, PAUSE_MENU, SLOTS_SAVE, -1 },
		{ "ask-home", ASK_HOME, PAUSE_MENU, SLOTS_SAVE, -1 },
	};
	for (auto & k : asks) {
		pv = base_pause();
		pv.ui.screen = PAUSE_ASK;
		pv.ui.ask = k.ask;
		pv.ui.ask_from = k.from;
		pv.ui.mode = k.mode;
		pv.ui.ask_card = k.card;
		pv.ui.card = k.card < 0 ? 0 : k.card;
		pv.ui.focus = 1;
		shot(k.name, v);
	}
	pv = base_pause();
	pv.ui.screen = PAUSE_SLOTS;
	pv.ui.mode = SLOTS_SAVE;
	pause_fail(&pv.ui, "Couldn't save to Slot 2",
	           "The SD card is full or locked. Your game is fine and still paused. OUR-0201");
	shot("ask-failed", v);
}

std::vector<library_game> g_games;
std::vector<int> g_shown;
roms_view g_rv;
std::vector<Tex *> g_covers;

// The games: results/roms-ids.txt (file, tab, game id: the card's ROMs) when
// there is one, else a few made up. Covers from results/art.
void load_games()
{
	std::string list = read_file("../results/roms-ids.txt");
	const char * fallback = "Banjo-Kazooie (USA).z64\tBanjo-Kazooie-A4BF9306\nBlast Corps (USA) (Rev 1).z64\tBlast Corps-7C647E65\n"
	                        "Mario Kart 64 (USA).z64\tMARIOKART64-3E5055B6\n1080 Snowboarding (USA).z64\tnone\n";
	if (list.empty())
		list = fallback;
	size_t at = 0;
	while (at < list.size()) {
		size_t end = list.find('\n', at);
		if (end == std::string::npos)
			end = list.size();
		std::string line = list.substr(at, end - at);
		at = end + 1;
		size_t tab = line.find('\t');
		if (tab == std::string::npos)
			continue;
		library_game g;
		library_game_init(&g, line.substr(0, tab).c_str());
		g.readable = 1;
		g.known = 1;
		snprintf(g.rom.id, sizeof(g.rom.id), "%s", line.substr(tab + 1).c_str());
		g_games.push_back(g);
	}
	library_sort(g_games.data(), int(g_games.size()));
	g_covers.assign(g_games.size(), nullptr);
	g_shown.assign(g_games.size(), 0);
	for (size_t i = 0; i < g_games.size(); i++) // a favorite with save states: its badge is the star, not the count
		if (strstr(g_games[i].title, "Banjo-Kazooie")) {
			g_games[i].favorite = 1;
			g_games[i].states = 2;
		}
	for (size_t i = 0; i < g_games.size(); i++)
		if (strstr(g_games[i].title, "Majora")) {
			g_games[i].autosave = NOW - 3600 * 5;
			g_games[i].autosave_ok = 1;
			g_games[i].states = 3;
		}
}

// The art slots as library_wiiu.c fills them: the selection and three each side.
void fill_art()
{
	int k = 0;
	for (int d = -3; d <= 3 && k < ROMS_ART; d++) {
		int at = g_rv.sel + d;
		if (at < 0 || at >= g_rv.count)
			continue;
		int game = g_rv.shown[at];
		if (g_covers[game] == nullptr)
			g_covers[game] = load_image(std::string("../results/art/") + g_games[game].rom.id + ".png");
		Tex * c = g_covers[game];
		g_rv.art[k].game = c ? game : -1;
		g_rv.art[k].rgba = c ? c->px.data() : nullptr;
		g_rv.art[k].w = c ? c->w : 0;
		g_rv.art[k].h = c ? c->h : 0;
		g_rv.art[k].gen = 1 + game;
		k++;
	}
	for (; k < ROMS_ART; k++)
		g_rv.art[k].game = -1;
}

void roms_shot(const char * name, int category, const char * title, float slide)
{
	ui_view v = base_view(UI_ROMS);
	v.roms = &g_rv;
	g_rv.category = category;
	g_rv.count = library_show(g_games.data(), int(g_games.size()), category, g_shown.data());
	g_rv.sel = 0;
	for (int i = 0; i < g_rv.count; i++)
		if (strstr(g_games[g_rv.shown[i]].title, title))
			g_rv.sel = i;
	g_rv.pos = g_rv.sel - slide;
	fill_art();
	shot(name, v);
}

void shots_roms()
{
	load_games();
	memset(&g_rv, 0, sizeof(g_rv));
	g_rv.games = g_games.data();
	g_rv.shown = g_shown.data();
	g_rv.total = int(g_games.size());
	g_rv.now = NOW;
	g_rv.us = 1;
	g_rv.glyphs = INPUT_PRO;
	snprintf(g_rv.folder, sizeof(g_rv.folder), "SD card: /wiiu/our64/roms");
	library_categories(g_games.data(), int(g_games.size()), g_rv.has);
	roms_shot("roms-all", LIBRARY_ALL, "Majora", 0);
	{ // the cover beside the portrait while the game loads (Zach, 2026-10-20)
		ui_view v = base_view(UI_LOADING);
		v.roms = &g_rv;
		v.loading.game = -1;
		for (int g = 0; g < int(g_games.size()); g++)
			if (strstr(g_games[g].title, "Majora"))
				v.loading.game = g;
		Tex * c = v.loading.game < 0 ? nullptr :
			load_image(std::string("../results/art/") + g_games[v.loading.game].rom.id + ".png");
		g_rv.art[0].game = c ? v.loading.game : -1;
		g_rv.art[0].rgba = c ? c->px.data() : nullptr;
		g_rv.art[0].w = c ? c->w : 0;
		g_rv.art[0].h = c ? c->h : 0;
		g_rv.art[0].gen = 1000;
		if (v.loading.game >= 0)
			snprintf(v.title, sizeof(v.title), "%s", g_games[v.loading.game].title);
		snprintf(v.loading.step, sizeof(v.loading.step), "Reading the game");
		v.loading.progress = 0.4f;
		shot("loading-cover", v);
	}
	roms_shot("roms-all-sliding", LIBRARY_ALL, "Majora", 0.45f);
	roms_shot("roms-all-first", LIBRARY_ALL, "007", 0);
	roms_shot("roms-all-long-title", LIBRARY_ALL, "Master Quest", 0);
	roms_shot("roms-all-favorite", LIBRARY_ALL, "Banjo-Kazooie", 0);
	roms_shot("roms-favorites", LIBRARY_FAVORITES, "Banjo-Kazooie", 0);
	roms_shot("roms-letter-b", library_category_of("B"), "Banjo-Tooie", 0);
	g_rv.bar_focus = 1; // up from the covers: left/right move along the bar
	roms_shot("roms-bar", library_category_of("B"), "Banjo-Tooie", 0);
	g_rv.bar_focus = 0;
	theme_use_layout(LAYOUT_LIST); // Settings > Theme > Our ROMs: List, as it was
	roms_shot("roms-list", LIBRARY_ALL, "Majora", 0);
	roms_shot("roms-list-sliding", LIBRARY_ALL, "Majora", 0.45f);
	roms_shot("roms-list-long-title", LIBRARY_ALL, "Master Quest", 0);
	g_rv.usb = 1; // the folder on a USB drive: Delete greyed, and X says why
	snprintf(g_rv.notice, sizeof(g_rv.notice), "On a USB drive: delete it on a computer");
	roms_shot("roms-list-delete-usb", library_category_of("M"), "Mario Kart", 0);
	g_rv.notice[0] = 0;
	g_rv.usb = 0;
	theme_use_layout(LAYOUT_COVERS);
	{ // HOME on Our ROMs asks first (Zach, 2026-10-19)
		ui_view v = base_view(UI_ROMS);
		v.roms = &g_rv;
		v.exit_ask = 1;
		v.exit_glyphs = INPUT_PRO;
		shot("roms-exit-ask", v);
		// X on a game: the same question box asks before it goes to the trash (Zach, 2026-10-25)
		snprintf(v.ask_title, sizeof(v.ask_title), "Delete Banjo-Kazooie?");
		snprintf(v.ask_body, sizeof(v.ask_body), "Its saves and save states stay.");
		snprintf(v.ask_action, sizeof(v.ask_action), "Delete");
		shot("roms-delete-ask", v);
	}
	g_games[0].readable = 0; // the first # game
	roms_shot("roms-unreadable", library_category_of("#"), "", 0);
	g_rv.total = 0;
	roms_shot("roms-empty", LIBRARY_ALL, "", 0);
	g_rv.usb = 1; // an empty folder on a USB drive
	snprintf(g_rv.folder, sizeof(g_rv.folder), "USB drive OUR64 EXFAT: /ROMs/N64");
	roms_shot("roms-empty-usb", LIBRARY_ALL, "", 0);
	g_rv.missing = ROMS_NO_DRIVE; // the drive not plugged in: named by the label kept as it was picked; A opens the picker
	snprintf(g_rv.folder, sizeof(g_rv.folder), "USB drive OUR64 EXFAT: /ROMs/N64");
	roms_shot("roms-usb-missing", LIBRARY_ALL, "", 0);
	theme_use_layout(LAYOUT_LIST);
	roms_shot("roms-list-usb-missing", LIBRARY_ALL, "", 0);
	theme_use_layout(LAYOUT_COVERS);
	snprintf(g_rv.folder, sizeof(g_rv.folder), "USB drive: /ROMs/N64"); // a settings.cfg from before the label was kept
	roms_shot("roms-usb-missing-no-label", LIBRARY_ALL, "", 0);
	// a path of 120 characters, few spaces: whole, wrapped after its slashes
	snprintf(g_rv.folder, sizeof(g_rv.folder),
	         "USB drive OUR64 EXFAT: /Emulation/Nintendo64/No-Intro_2026-10/Every_cartridge_we_own_by_year/USA_and_Europe/Complete");
	roms_shot("roms-usb-missing-long", LIBRARY_ALL, "", 0);
	theme_use_layout(LAYOUT_LIST);
	roms_shot("roms-list-usb-missing-long", LIBRARY_ALL, "", 0);
	theme_use_layout(LAYOUT_COVERS);
	g_rv.usb = 0;
	g_rv.missing = ROMS_NO_FOLDER; // an SD folder deleted on a computer: its full path, whole
	snprintf(g_rv.folder, sizeof(g_rv.folder), "SD card: /Games/Nintendo 64/Every cartridge we own, by year/USA and Europe");
	roms_shot("roms-folder-missing", LIBRARY_ALL, "", 0);
	theme_use_layout(LAYOUT_LIST);
	roms_shot("roms-list-folder-missing", LIBRARY_ALL, "", 0);
	theme_use_layout(LAYOUT_COVERS);
	g_rv.missing = ROMS_FOUND;
}

void shots_setup()
{
	static setup_view sv;
	ui_view v = base_view(UI_SETUP);
	v.setup = &sv;
	memset(&sv, 0, sizeof(sv));
	sv.glyphs = INPUT_PRO;
	sv.screen = SETUP_SETTINGS;
	settings_open(&sv.settings);
	shot("settings", v);
	sv.settings.row = SET_GAMES;
	shot("settings-games", v);
	sv.screen = SETUP_PICTURE; // Settings > Picture
	picture_open(&sv.picture, MODE_BOTH, PICTURE_43, SHADER_OFF, 1);
	shot("picture", v);
	sv.picture.row = PIC_SHADER;
	sv.picture.pick[PIC_SHADER] = SHADER_CRT;
	shot("picture-shader-crt", v);
	picture_open(&sv.picture, MODE_GAMEPAD, PICTURE_43, SHADER_OFF, 1);
	sv.picture.row = PIC_SHADER;
	sv.picture.pick[PIC_SHADER] = SHADER_SCANLINES; // greyed: TV only
	shot("picture-shader-gamepad-only", v);
	sv.screen = SETUP_THEME; // Settings > Theme
	look_open(&sv.look, theme_comrade() ? THEME_COMRADE : THEME_STANDARD, LAYOUT_COVERS);
	shot("theme", v);
	snprintf(sv.line, sizeof(sv.line), "Colors: Comrade"); // what A just applied, under the header
	shot("theme-applied", v);
	sv.line[0] = 0;
	sv.look.row = LOOK_LAYOUT;
	sv.look.pick[LOOK_LAYOUT] = LAYOUT_LIST;
	shot("theme-layout", v);
	sv.screen = SETUP_GAMES; // Settings > Games: no game read as it opens
	games_open(&sv.games, "SD card: /wiiu/our64/roms", 1, START_RECENT);
	shot("games", v);
	// one line: a long path loses its middle, the drive and the last folder stay
	games_open(&sv.games, "USB drive OUR64 EXFAT: /Emulation/Nintendo64/No-Intro_2026-10/Every_cartridge_we_own_by_year/USA", 1, START_RECENT);
	shot("games-usb-long", v);
	games_open(&sv.games, "SD card: /wiiu/our64/roms", 1, START_RECENT);
	sv.games.row = GAMES_COVERS;
	shot("games-covers", v);
	sv.games.row = GAMES_RESUME; // Auto-Resume: On or Off, A switches it (off asks first)
	shot("games-auto-resume", v);
	v.exit_ask = 1; // A on it while On: the question first, Cancel focused
	v.exit_glyphs = INPUT_PRO;
	snprintf(v.ask_title, sizeof(v.ask_title), "Are you sure?");
	snprintf(v.ask_body, sizeof(v.ask_body), "You will lose all your current Auto-Saves.");
	snprintf(v.ask_action, sizeof(v.ask_action), "Turn Off");
	shot("games-auto-resume-ask", v);
	v.exit_ask = 0;
	v.ask_title[0] = 0;
	sv.games.auto_resume = 0;
	snprintf(sv.line, sizeof(sv.line), "Auto-Resume off. Auto-Saves deleted.");
	shot("games-auto-resume-off", v);
	sv.line[0] = 0;
	sv.games.auto_resume = 1;
	sv.games.row = GAMES_START;
	shot("games-start-on", v);
	sv.screen = SETUP_START; // A on Start on: its own page, the tab in use ticked
	start_open(&sv.start, START_RECENT);
	shot("start-on", v);
	sv.start.row = START_FAVORITES;
	shot("start-on-favorites", v);
	sv.screen = SETUP_GAMES;
	sv.games.row = GAMES_COVERS;
	sv.screen = SETUP_FETCH; // A on Fetch covers: its own screen, like the loading screen
	games_reading(&sv.games, 120, 340); // the headers not read yet, first
	shot("games-fetch-reading", v);
	games_fetching(&sv.games, 12, 40);
	shot("games-fetching", v);
	sv.screen = SETUP_GAMES;
	games_fetched(&sv.games); // the fetch ended: what it did, under the header
	covers_summary(COVERS_OK, 0, 9, 3, 0, sv.line, sizeof(sv.line));
	shot("games-fetched", v);
	covers_summary(COVERS_NO_NETWORK, 0, 0, 0, 0, sv.line, sizeof(sv.line));
	shot("games-no-network", v);
	covers_summary(COVERS_OK, 0, 0, 0, 0, sv.line, sizeof(sv.line)); // no game without a cover: straight back
	shot("games-none-missing", v);
	sv.line[0] = 0;
	sv.screen = SETUP_BROWSE; // Settings > Games > ROM folder
	browse_open(&sv.browse);
	browse_add(&sv.browse, "SD card", "sd:/", 0);
	browse_add(&sv.browse, "USB drive OUR64 EXFAT", "usb1p1:/", 0);
	browse_add(&sv.browse, "USB drive 2: no FAT32 or exFAT partition", "", 1);
	shot("browse-drives", v);
	sv.browse.row = 2;
	shot("browse-drives-greyed", v);
	sv.browse.row = 0;
	browse_event(&sv.browse, PEV_OK, -1); // into the SD card
	browse_entered(&sv.browse, 4);
	const char * folders[8] = { "3ds", "Games", "N64 ROMs", "retroarch", "switch", "wiiu", "Wii U backups", "music" };
	char path[64];
	for (int i = 0; i < 8; i++) {
		snprintf(path, sizeof(path), "sd:/%s", folders[i]);
		browse_add(&sv.browse, folders[i], path, 0);
	}
	sv.browse.row = 3;
	shot("browse-folder", v);
	sv.browse.row = 0;
	shot("browse-folder-use", v);
	browse_entered(&sv.browse, -1); // a folder it couldn't open: Use this folder greyed
	snprintf(sv.line, sizeof(sv.line), "Couldn't open this folder.");
	shot("browse-folder-unreadable", v);
	sv.line[0] = 0;
	browse_entered(&sv.browse, 0);
	char name[32];
	for (int i = 0; i < 30; i++) {
		snprintf(name, sizeof(name), "Folder %02d", i);
		snprintf(path, sizeof(path), "sd:/Folder %02d", i);
		browse_add(&sv.browse, name, path, 0);
	}
	for (int i = 0; i < 20; i++)
		browse_event(&sv.browse, PEV_DOWN, -1);
	shot("browse-long", v);
	sv.screen = SETUP_SETTINGS;

	sv.screen = SETUP_CONTROLLERS;
	controllers_open(&sv.controllers);
	const int types[INPUT_PORTS] = { INPUT_GAMEPAD, INPUT_PRO, INPUT_NUNCHUK, -1 };
	for (int i = 0; i < INPUT_PORTS; i++) {
		sv.controllers.ports[i].type = types[i];
		sv.controllers.ports[i].battery = i == 1 ? 3 : -1;
		sv.controllers.ports[i].charging = i == 0;
		sv.controllers.rumble[i] = i != 2;
	}
	sv.controllers.ports[2].gone = 1;
	shot("controllers", v);
	sv.controllers.in_actions = 1;
	sv.controllers.card = 1;
	sv.controllers.action = CTL_RUMBLE;
	shot("controllers-rumble", v);
	sv.controllers.card = 3;
	sv.controllers.action = CTL_EDIT;
	shot("controllers-edit-off", v);

	sv.screen = SETUP_ORDER;
	order_open(&sv.order, 3);
	order_press(&sv.order, 1);
	shot("change-order", v);

	sv.screen = SETUP_MAPPING;
	input_map m, prof;
	input_default_map(INPUT_PRO, &m);
	mapping_open(&sv.mapping, INPUT_PRO, &m, nullptr, nullptr); // from Our ROMs: no game tab
	shot("edit-buttons", v);
	mapping_open(&sv.mapping, INPUT_PRO, &m, nullptr, "The Legend of Zelda: Majora's Mask");
	shot("edit-buttons-tabs", v);
	sv.mapping.tab = MAP_TAB_GAME; // no profile yet: All games' buttons
	shot("edit-buttons-game-new", v);
	prof = m;
	prof.sources[N64_CRIGHT] = 1u << SRC_X;
	prof.sources[N64_CUP] = 1u << SRC_RSTICK_UP;
	prof.sources[N64_A] |= 1u << SRC_ZR;
	mapping_open(&sv.mapping, INPUT_PRO, &m, &prof, "The Legend of Zelda: Majora's Mask");
	sv.mapping.row = N64_CRIGHT;
	sv.mapping.top = N64_CRIGHT - 2;
	snprintf(sv.mapping.line, sizeof(sv.mapping.line), "X moved from N64 C-Up");
	shot("edit-buttons-game", v);
	sv.mapping.line[0] = 0;
	sv.mapping.can_undo = 1;
	shot("edit-buttons-undo", v);
	sv.mapping.can_undo = 0;
	sv.mapping.screen = MAP_CAPTURE;
	sv.mapping.capture_until = now_us() / 1e6 + 4.2;
	shot("edit-buttons-capture", v);
	sv.mapping.capture_add = 1;
	shot("edit-buttons-capture-add", v);
	sv.mapping.before = sv.mapping.edit; // what the failed write puts back
	mapping_fail(&sv.mapping, "Couldn't save your buttons",
	             "Check the SD card's lock switch or free space, then try again. OUR-0203");
	shot("edit-buttons-failed", v);
	sv.mapping.screen = MAP_LIST;
	sv.mapping.row = MAP_RANGE;
	sv.mapping.top = MAP_ROWS - MAP_SHOWN;
	shot("edit-buttons-end", v);
	sv.mapping.top = 0;
	for (int row : { N64_L, N64_R, N64_Z, N64_STICK_UP, N64_STICK_LEFT }) { // the arrows for the inputs a front photo can't show
		sv.mapping.row = row;
		sv.mapping.top = row > MAP_SHOWN - 1 ? row - (MAP_SHOWN - 1) : 0;
		shot(row == N64_L ? "edit-buttons-l" : row == N64_R ? "edit-buttons-r" : row == N64_Z ? "edit-buttons-z"
		     : row == N64_STICK_UP ? "edit-buttons-stick-up" : "edit-buttons-stick-left", v);
	}
	sv.glyphs = INPUT_WIIMOTE;
	sv.mapping.row = N64_A;
	sv.mapping.top = 0;
	shot("edit-buttons-wiimote", v);
	sv.glyphs = -1;
}

void shots_other()
{
	ui_view v = base_view(UI_PROMPT);
	prompt_init(&v.prompt, MODE_BOTH, 1);
	snprintf(v.line, sizeof(v.line), "A: Our ROMs");
	shot("prompt", v);
	prompt_init(&v.prompt, MODE_BOTH, 0);
	snprintf(v.warning, sizeof(v.warning), "Turn on the GamePad to play on it.");
	shot("prompt-no-gamepad", v);

	v = base_view(UI_LOADING);
	snprintf(v.loading.step, sizeof(v.loading.step), "Preparing graphics 12/17");
	v.loading.progress = 0.7f;
	v.loading.game = -1;
	shot("loading", v);
	snprintf(v.loading.stopped, sizeof(v.loading.stopped),
	         "The autosave is from another Our64 version, so it can't be used. Start from the title screen? "
	         "Your in-game saves are kept. OUR-0301");
	shot("loading-stopped", v);

	v = base_view(UI_CLOSING);
	shot("closing", v);

	v = base_view(UI_MESSAGE);
	snprintf(v.headline, sizeof(v.headline), "CAN'T READ THE SD CARD");
	snprintf(v.body, sizeof(v.body), "Our64 needs the SD card for your games and saves. Check that it is in and not "
	                                 "locked, then start Our64 again.");
	snprintf(v.code, sizeof(v.code), "OUR-0102");
	snprintf(v.legend, sizeof(v.legend), "\xee\x81\x84 Wii U Menu");
	v.failure = 1;
	shot("message", v);

	v = base_view(UI_PAUSED);
	lock_notices();
	notices_add(&g_notices, 0, NOTICE_WARN, "Player 1's controller disconnected", "Connect it again to keep playing", 30);
	notices_add(&g_notices, 1, NOTICE_INFO, "Player 2: Pro Controller", "", 30);
	unlock_notices();
	shot("paused-notices", v);

	g_px.assign(size_t(UI_W) * K * UI_H * K * 3, 0.0f);
	g_check.screen = "tv-card";
	g_check.boxes.clear();
	draw_tv_card("Legend of Zelda, The - Majora's Mask");
	write_png(g_out + "/tv-card.png");
}

} // namespace

Platform * platform; // libgui's; no threads here, so its mutexes do nothing

// libgui builds its FreeType from three modules it names, the way the
// console's static FreeType allows. A PC's shared FreeType keeps them
// internal, so they are copied out of a library FreeType set up itself.
extern "C" {
unsigned char tt_driver_class[512], sfnt_module_class[512], ft_smooth_renderer_class[512];
}
#include FT_MODULE_H
static bool copy_freetype_modules()
{
	FT_Library lib;
	if (FT_Init_FreeType(&lib) != 0)
		return false;
	const struct { const char * name; unsigned char * to; } m[] = {
		{ "truetype", tt_driver_class }, { "sfnt", sfnt_module_class }, { "smooth", ft_smooth_renderer_class } };
	for (auto & k : m) {
		FT_Module mod = FT_Get_Module(lib, k.name);
		if (mod == nullptr)
			return false;
		const FT_Module_Class * clazz = *reinterpret_cast<const FT_Module_Class * const *>(mod); // FT_ModuleRec's first field
		memcpy(k.to, clazz, 512);
	}
	return true; // lib stays: the classes point into it
}

namespace our64ui {
notices g_notices;
double now_us() { return 1e12; }
void lock_notices() {}
void unlock_notices() {}
void * frozen_texture() { return g_frozen_tex; }
} // namespace our64ui

int main(int argc, char ** argv)
{
	setvbuf(stdout, nullptr, _IOLBF, 0);
	const char * font_path = "../results/font/system-font.ttf";
	g_out = "../results/shots";
	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--scale") && i + 1 < argc)
			g_ui_scale = float(atof(argv[++i]));
		else if (!strcmp(argv[i], "--out") && i + 1 < argc)
			g_out = argv[++i];
		else if (!strcmp(argv[i], "--font") && i + 1 < argc)
			font_path = argv[++i];
		else if (!strcmp(argv[i], "--theme") && i + 1 < argc) // Standard unless "comrade"
			theme_use(!strcmp(argv[++i], "comrade") ? THEME_COMRADE : THEME_STANDARD);
	}
	static std::string font = read_file(font_path);
	if (font.empty()) {
		printf("No font at %s: fetch sd:/wiiu/our64/debug/system-font.ttf from the console.\n", font_path);
		return 2;
	}
	std::string mk = "mkdir -p '" + g_out + "'";
	if (system(mk.c_str()) != 0)
		return 2;
	if (!copy_freetype_modules()) {
		printf("FreeType has no truetype, sfnt or smooth module.\n");
		return 2;
	}
	fontSystem = new GuiTextRenderer((const uint8_t *)font.data(), FT_Long(font.size()), &g_glyph_renderer, g_ui_scale);
	g_images = &g_img_renderer;
	g_cat = load_image("../player/data/cat1.jpg");
	Tex * cat2 = load_image("../player/data/cat2.jpg");
	g_cat2 = cat2;
	g_cat2_w = cat2 ? cat2->w : 0;
	g_cat2_h = cat2 ? cat2->h : 0;
	g_pad = load_image("../player/data/n64-controller.png");
	notices_init(&g_notices);
	for (int i = 0; i <= CARDS; i++)
		fake_picture(g_thumb[i], i);
	g_frozen_tex = frozen_from_thumb();
	g_checks = &g_check;
	g_check.words = fopen((g_out + "/words.txt").c_str(), "w");
	g_check.low = fopen((g_out + "/contrast.txt").c_str(), "w");
	printf("Text scale %.2f; %s theme; shots in %s\n", g_ui_scale, theme_comrade() ? "Comrade" : "Standard", g_out.c_str());
	shots_pause();
	shots_roms();
	shots_setup();
	shots_other();
	if (g_check.words)
		fclose(g_check.words);
	if (g_check.low)
		fclose(g_check.low);
	printf("%d text%s under 4.5:1 on what is drawn under %s (contrast.txt)\n", g_check.lows, g_check.lows == 1 ? "" : "s",
	       g_check.lows == 1 ? "it" : "them");
	printf("%d text problem%s\n", g_check.problems, g_check.problems == 1 ? "" : "s");
	return g_check.problems ? 1 : 0;
}
