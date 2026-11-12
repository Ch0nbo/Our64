// Our64 player: Our64's own screens (ui.h), drawn on the graphics core with
// libgui's GX2 renderers and the console's system font.
//
// Screens outside play are whole frames on WHBGfx's TV and GamePad buffers
// (libgui's frame cycle). Over a running game only the controller notices
// are drawn: onto a copy of the game's frame, so GLideN64's own buffer stays
// clean for the frozen frame behind a pause. The frozen frame is copied out
// of GLideN64 when the game freezes, so it outlives GLideN64 for the
// closing screen.
#include <cstdio>
#include <malloc.h>
#include <cstring>
#include <string>

#include <coreinit/cache.h>
#include <coreinit/memdefaultheap.h>
#include <coreinit/memory.h>
#include <coreinit/mutex.h>
#include <gx2/context.h>
#include <gx2/display.h>
#include <gx2/event.h>
#include <gx2/mem.h>
#include <gx2/surface.h>
#include <gx2/texture.h>
#include <gx2/state.h>
#include <gx2/swap.h>
#include <gx2/utils.h>
#include <proc_ui/procui.h>
#include <sys/stat.h>
#include <png.h>
#include <turbojpeg.h>
#include <vpad/input.h>
#include <whb/gfx.h>

#include "libgui/Gui.h"
#include "drivers/wut/WutThreadDriver.h"
#include "drivers/wut/WutVideoDriver.h"
#include "drivers/wut/shaders/Shader.h" // GX2InitTexture

#include "ui.h"
#include "ui_draw.h"
#include "video_ui.h"
#include "prompt.h"
extern "C" {
#include "player.h"
#include "report.h"
}

// cat1.jpg (225x225), the portrait in the headers.
__asm__(".section .rodata\n.balign 64\n.global our64_cat1_jpg\nour64_cat1_jpg:\n.incbin \"" OUR64_DATA
        "/cat1.jpg\"\n.global our64_cat1_jpg_end\nour64_cat1_jpg_end:\n.previous\n");
extern "C" const unsigned char our64_cat1_jpg[], our64_cat1_jpg_end[];
// cat2.jpg (194x259) on the loading screen; the N64 controller (Evan-Amos, public domain) on Edit buttons.
__asm__(".section .rodata\n.balign 64\n.global our64_cat2_jpg\nour64_cat2_jpg:\n.incbin \"" OUR64_DATA
        "/cat2.jpg\"\n.global our64_cat2_jpg_end\nour64_cat2_jpg_end:\n"
        ".balign 64\n.global our64_pad_png\nour64_pad_png:\n.incbin \"" OUR64_DATA
        "/n64-controller.png\"\n.global our64_pad_png_end\nour64_pad_png_end:\n.previous\n");
extern "C" const unsigned char our64_cat2_jpg[], our64_cat2_jpg_end[], our64_pad_png[], our64_pad_png_end[];

Platform * platform;
namespace gx2 { void restoreState(); } // GLideN64's GX2 backend

namespace {

// ---- libgui's platform: video and threads; no sound, files or input (ours feed the screens) ----

class OurPlatform : public Platform
{
public:
	void init(const PlatformConfig & c) override
	{
		config = c;
		thread.init();
		video.init(c.canvasWidth, c.canvasHeight);
	}
	void requestExit() override {}
	AudioDriver * getAudio() override { return nullptr; }
	VideoDriver * getVideo() override { return &video; }
	InputDriver * getInput() override { return nullptr; }
	FileSystemDriver * getFileSystem() override { return nullptr; }
	ThreadDriver * getThread() override { return &thread; }
	Logger * getLogger() override { return nullptr; }
	SystemEvent getSystemEvent() override { return SystemEvent::None; }
	Status getStatus() const override { return ProcUIInForeground() ? Status::Running : Status::Paused; }
	void triggerExit() override {}

	WutVideoDriver video;
	WutThreadDriver thread;

protected:
	void shutdown() override {}
};

OurPlatform * g_platform;
WutVideoDriver * g_video;
GX2Texture g_frozen;         // the game's last frame, behind pauses and the closing screen
bool g_frozen_ok;            // g_frozen holds a frame
bool g_frozen_now;           // ... of the current freeze (cleared when the game runs again)
GX2ColorBuffer g_overlay;    // a game frame with the notices on
GX2ColorBuffer g_tv_card;    // GamePad only: what the TV shows (black, small: the copy scales it)
bool g_tv_card_ok;
// GamePad only: the TV's black is copied to the TV's scan buffers until each
// holds it, then no more. A full-size copy every VI (8 MB at 1080p) was GPU
// time the game's frames waited behind (frame test, 2026-11-06). Any other
// swap in between (a menu, Our ROMs) or a new card or new scan buffers starts
// the count over. More copies than buffers (two or three), so each has one.
constexpr unsigned kCardCopies = 4;
unsigned g_card_copies;
uint32_t g_card_swap = ~0u; // the swap count after the last scan with the card
GX2ContextState * g_copy_ctx;

volatile int g_mode = MODE_BOTH; // before the prompt's choice both screens are on
ui_view g_last;
bool g_have_last;

OSMutex g_notice_lock;
bool g_notice_lock_ok;

} // namespace

// ---- ui_draw.h's platform side ----

namespace our64ui {

notices g_notices;

double now_us() { return double(OSTicksToMicroseconds(OSGetSystemTime())); }

void lock_notices()
{
	if (!g_notice_lock_ok) {
		OSInitMutex(&g_notice_lock);
		notices_init(&g_notices);
		g_notice_lock_ok = true;
	}
	OSLockMutex(&g_notice_lock);
}

void unlock_notices() { OSUnlockMutex(&g_notice_lock); }

void * frozen_texture() { return g_frozen_ok ? &g_frozen : nullptr; }

} // namespace our64ui

using namespace our64ui;

namespace {

bool alloc_texture(GX2Texture & t, uint32_t w, uint32_t h, GX2TileMode tile)
{
	memset(&t, 0, sizeof(t));
	GX2InitTexture(&t, w, h, 1, 0, GX2_SURFACE_FORMAT_UNORM_R8_G8_B8_A8, GX2_SURFACE_DIM_TEXTURE_2D, tile);
	GX2CalcSurfaceSizeAndAlignment(&t.surface);
	GX2InitTextureRegs(&t);
	t.surface.image = MEMAllocFromDefaultHeapEx(t.surface.imageSize, t.surface.alignment);
	return t.surface.image != nullptr;
}

bool alloc_color_buffer(GX2ColorBuffer & cb, uint32_t w, uint32_t h)
{
	memset(&cb, 0, sizeof(cb));
	cb.surface.dim = GX2_SURFACE_DIM_TEXTURE_2D;
	cb.surface.width = w;
	cb.surface.height = h;
	cb.surface.depth = 1;
	cb.surface.mipLevels = 1;
	cb.surface.format = GX2_SURFACE_FORMAT_UNORM_R8_G8_B8_A8;
	cb.surface.aa = GX2_AA_MODE1X;
	cb.surface.use = GX2_SURFACE_USE_TEXTURE_COLOR_BUFFER_TV;
	cb.surface.tileMode = GX2_TILE_MODE_DEFAULT;
	cb.viewNumSlices = 1;
	GX2CalcSurfaceSizeAndAlignment(&cb.surface);
	GX2InitColorBufferRegs(&cb);
	cb.surface.image = MEMAllocFromDefaultHeapEx(cb.surface.imageSize, cb.surface.alignment);
	return cb.surface.image != nullptr;
}

// A surface copy on the GPU, in a state of our own.
void copy_surface(GX2Surface * from, GX2Surface * to)
{
	if (g_copy_ctx == nullptr) {
		g_copy_ctx = static_cast<GX2ContextState *>(MEMAllocFromDefaultHeapEx(sizeof(GX2ContextState), GX2_CONTEXT_STATE_ALIGNMENT));
		if (g_copy_ctx == nullptr)
			return;
		GX2SetupContextStateEx(g_copy_ctx, TRUE);
	}
	GX2SetContextState(g_copy_ctx);
	GX2CopySurface(from, 0, 0, to, 0, 0);
	GX2Invalidate(GX2_INVALIDATE_MODE_TEXTURE, to->image, to->imageSize);
	GX2Flush();
}


GX2Texture * load_jpeg(const unsigned char * data, const unsigned char * end, int * pw, int * ph)
{
	GX2Texture * t = nullptr;
	tjhandle j = tjInitDecompress();
	if (j == nullptr)
		return nullptr;
	int w = 0, h = 0, sub = 0, cs = 0;
	unsigned long size = (unsigned long)(end - data);
	if (tjDecompressHeader3(j, data, size, &w, &h, &sub, &cs) == 0 && w > 0 && h > 0) {
		std::string rgba(size_t(w) * h * 4, '\0');
		if (tjDecompress2(j, data, size, (unsigned char *)&rgba[0], w, 0, h, TJPF_RGBA, 0) == 0) {
			t = static_cast<GX2Texture *>(g_images->createTexture(w, h));
			if (t != nullptr)
				g_images->loadTextureData(t, (const uint8_t *)rgba.data(), w, h);
		}
	}
	tjDestroy(j);
	if (pw) *pw = w;
	if (ph) *ph = h;
	return t;
}

GX2Texture * load_png(const unsigned char * data, const unsigned char * end)
{
	png_image im;
	memset(&im, 0, sizeof(im));
	im.version = PNG_IMAGE_VERSION;
	if (!png_image_begin_read_from_memory(&im, data, size_t(end - data)))
		return nullptr;
	im.format = PNG_FORMAT_RGBA;
	std::string rgba(PNG_IMAGE_SIZE(im), '\0');
	GX2Texture * t = nullptr;
	if (png_image_finish_read(&im, nullptr, &rgba[0], 0, nullptr)) {
		t = static_cast<GX2Texture *>(g_images->createTexture(int(im.width), int(im.height)));
		if (t != nullptr)
			g_images->loadTextureData(t, (const uint8_t *)rgba.data(), int(im.width), int(im.height));
	}
	png_image_free(&im);
	return t;
}

void load_portrait()
{
	g_cat = load_jpeg(our64_cat1_jpg, our64_cat1_jpg_end, nullptr, nullptr);
	g_cat2 = load_jpeg(our64_cat2_jpg, our64_cat2_jpg_end, &g_cat2_w, &g_cat2_h);
	g_pad = load_png(our64_pad_png, our64_pad_png_end);
}

// The system font. A copy goes to sd:/wiiu/our64/debug once, so the
// button glyphs can be looked at on a computer (it never leaves the SD).
void load_font()
{
	void * font = nullptr;
	uint32_t size = 0;
	if (!OSGetSharedData(OS_SHAREDDATATYPE_FONT_STANDARD, 0, &font, &size) || font == nullptr) {
		report_line("UI: no system font");
		return;
	}
	report_line("UI: system font, %u bytes", size);
	report_save();
	fontSystem = new GuiTextRenderer(static_cast<const uint8_t *>(font), size, g_video->getGlyphRenderer(),
	                                 g_video->getUIScale());
	if (const char * root = report_sd_root()) {
		char path[256];
		// A mkdir on the SD took 15 s once (console, 2026-10-17): only when the copy is missing.
		snprintf(path, sizeof(path), "%s/debug/system-font.ttf", root);
		struct stat st;
		if (stat(path, &st) != 0) {
			char dir[256];
			snprintf(dir, sizeof(dir), "%s/debug", root);
			mkdir(dir, 0777);
		}
		// Through a buffer of our own: an SD write straight from the shared
		// font's memory never finished (console, 2026-10-16).
		static uint8_t chunk[64 * 1024] __attribute__((aligned(64)));
		if (stat(path, &st) != 0 || st.st_size != (off_t)size)
			if (FILE * f = fopen(path, "wb")) {
				for (uint32_t at = 0; at < size; at += sizeof(chunk)) {
					uint32_t n = size - at < sizeof(chunk) ? size - at : sizeof(chunk);
					memcpy(chunk, static_cast<const uint8_t *>(font) + at, n);
					if (fwrite(chunk, 1, n, f) != n)
						break;
				}
				fclose(f);
			}
	}
}

bool ready()
{
	if (g_platform != nullptr)
		return true;
	if (!ProcUIInForeground())
		return false;
	g_platform = new OurPlatform();
	platform = g_platform;
	PlatformConfig c = {};
	c.canvasWidth = UI_W;
	c.canvasHeight = UI_H;
	g_platform->init(c);
	g_video = &g_platform->video;
	g_images = g_video->getImageRenderer();
	load_font();
	load_portrait();
	report_line("UI: ready (portrait %s)", g_cat ? "loaded" : "missing");
	return true;
}

// The frozen frame's 4:3 middle, drawn small and read back for a save state's card.
void thumb_on_graphics_core(void * arg)
{
	uint16_t * out = static_cast<uint16_t *>(arg);
	memset(out, 0, THUMB_BYTES);
	if (!ready() || !g_frozen_ok)
		return;
	static GX2ColorBuffer cb;
	static bool cb_ok;
	if (!cb_ok)
		cb_ok = alloc_color_buffer(cb, THUMB_W, THUMB_H);
	GX2Surface lin;
	memset(&lin, 0, sizeof(lin));
	lin.dim = GX2_SURFACE_DIM_TEXTURE_2D;
	lin.width = THUMB_W;
	lin.height = THUMB_H;
	lin.depth = 1;
	lin.mipLevels = 1;
	lin.format = GX2_SURFACE_FORMAT_UNORM_R8_G8_B8_A8;
	lin.aa = GX2_AA_MODE1X;
	lin.use = GX2_SURFACE_USE_TEXTURE;
	lin.tileMode = GX2_TILE_MODE_LINEAR_ALIGNED;
	GX2CalcSurfaceSizeAndAlignment(&lin);
	lin.image = MEMAllocFromDefaultHeapEx(lin.imageSize, lin.alignment);
	if (!cb_ok || lin.image == nullptr) {
		if (lin.image != nullptr)
			MEMFreeToDefaultHeap(lin.image);
		report_line("UI: no memory for a thumbnail");
		return;
	}
	const float fw = g_frozen.surface.width, fh = g_frozen.surface.height;
	const float cw = fh * 4 / 3 < fw ? fh * 4 / 3 : fw, ch = fw * 3 / 4 < fh ? fw * 3 / 4 : fh;
	const float dw = UI_W * fw / cw, dh = UI_H * fh / ch;
	g_images->drawTexture(&g_frozen, (UI_W - dw) / 2, (UI_H - dh) / 2, uint16_t(dw), uint16_t(dh), 0, 1, 1, 255);
	g_video->drawOverlay(&cb);
	if (g_copy_ctx == nullptr) {
		g_copy_ctx = static_cast<GX2ContextState *>(MEMAllocFromDefaultHeapEx(sizeof(GX2ContextState), GX2_CONTEXT_STATE_ALIGNMENT));
		if (g_copy_ctx != nullptr)
			GX2SetupContextStateEx(g_copy_ctx, TRUE);
	}
	if (g_copy_ctx != nullptr) {
		GX2SetContextState(g_copy_ctx);
		GX2CopySurface(&cb.surface, 0, 0, &lin, 0, 0);
		GX2DrawDone();
		DCInvalidateRange(lin.image, lin.imageSize);
		const uint8_t * px = static_cast<const uint8_t *>(lin.image);
		for (int y = 0; y < THUMB_H; y++)
			for (int x = 0; x < THUMB_W; x++) {
				const uint8_t * q = px + (size_t(y) * lin.pitch + x) * 4;
				out[y * THUMB_W + x] = uint16_t((q[0] >> 3) << 11 | (q[1] >> 2) << 5 | (q[2] >> 3));
			}
	}
	MEMFreeToDefaultHeap(lin.image);
	if (video_game_started())
		gx2::restoreState();
}

void apply_screens()
{
	GX2SetTVEnable(TRUE);
	GX2SetDRCEnable(g_mode == MODE_TV ? FALSE : TRUE);
}

// One frame of a screen on WHBGfx's buffers, presented.
void frame(const ui_view & v)
{
	g_video->prepareFrame();
	const bool chosen = v.screen != UI_PROMPT;
	if (chosen && g_mode == MODE_GAMEPAD) {
		g_video->setUiTargets(1u << unsigned(OutputTarget::TV));
		draw_tv_black();
		g_video->flushDrawQueue();
		g_video->setUiTargets(1u << unsigned(OutputTarget::DRC));
	} else if (chosen && g_mode == MODE_TV) {
		g_video->setUiTargets(1u << unsigned(OutputTarget::TV));
	}
	draw_screen(v);
	g_video->presentBuffer();
	g_video->setUiTargets(WutVideoDriver::kAllTargets);
	apply_screens();
}

void freeze()
{
	GX2ColorBuffer * last = video_game_started() ? video_last_frame() : nullptr;
	if (last == nullptr)
		return;
	if (g_frozen_ok && (g_frozen.surface.width != last->surface.width || g_frozen.surface.height != last->surface.height)) {
		MEMFreeToDefaultHeap(g_frozen.surface.image);
		g_frozen_ok = false;
	}
	if (!g_frozen_ok)
		g_frozen_ok = alloc_texture(g_frozen, last->surface.width, last->surface.height, GX2_TILE_MODE_DEFAULT);
	if (g_frozen_ok)
		copy_surface(&last->surface, &g_frozen.surface);
}

void resume_on_graphics_core(void *)
{
	g_frozen_now = false;
	video_set_hold(false);
}

void show_on_graphics_core(void * arg)
{
	const ui_view & v = *static_cast<const ui_view *>(arg);
	if (!ready() || !ProcUIInForeground()) // another app (the Wii U Menu) has the screen
		return;
	// The first frozen screen takes the game's last frame; later ones keep it.
	if ((v.screen == UI_PAUSED || v.screen == UI_CLOSING || v.screen == UI_PAUSE) && !g_frozen_now) {
		freeze();
		g_frozen_now = true;
	}
	video_set_hold(true);
	g_last = v;
	g_have_last = true;
	frame(v);
	if (video_game_started())
		gx2::restoreState();
}

void tv_card_on_graphics_core(void *)
{
	if (!ready())
		return;
	g_card_copies = 0; // each scan buffer gets the black again
	if (g_tv_card_ok)
		return;
	g_tv_card_ok = alloc_color_buffer(g_tv_card, 64, 64);
	if (!g_tv_card_ok)
		return;
	memset(g_tv_card.surface.image, 0, g_tv_card.surface.imageSize); // black (the scan copy leaves alpha out)
	DCFlushRange(g_tv_card.surface.image, g_tv_card.surface.imageSize);
	GX2Invalidate(GX2_INVALIDATE_MODE_CPU, g_tv_card.surface.image, g_tv_card.surface.imageSize);
}

void restore_on_graphics_core(void *)
{
	g_mode = MODE_BOTH;
	apply_screens();
}

} // namespace

namespace gx2 { void restoreState(); }

// ---- the graphics core's side, for video.cpp ----

// While GLideN64 builds a game's saved shaders (inside RomOpen): the loading
// screen's bar and "Preparing graphics 12/17", at most every 80 ms.
void ui_gfx_loading_progress(unsigned done, unsigned total)
{
	static double last;
	const double now = now_us();
	if (!g_have_last || g_last.screen != UI_LOADING || total == 0 || (now - last < 80000 && done + 1 < total))
		return;
	last = now;
	g_last.loading.progress = 0.7f + 0.3f * float(done + 1) / float(total);
	snprintf(g_last.loading.step, sizeof(g_last.loading.step), "Preparing graphics %u/%u", done + 1, total);
	frame(g_last);
	gx2::restoreState(); // GLideN64 is mid-RomOpen: its GPU state back before it draws on
}

static volatile int g_speed_percent = -1;
extern "C" void ui_speed(int percent) { g_speed_percent = percent; }

GX2ColorBuffer * ui_gfx_overlay(GX2ColorBuffer * frame)
{
	lock_notices();
	const notice * shown[NOTICES_SHOWN];
	int n = notices_visible(&g_notices, now_us(), shown);
	unlock_notices();
	const int speed = g_speed_percent;
	if ((n == 0 && speed < 0) || !ready())
		return frame;
	if (g_overlay.surface.image != nullptr &&
	    (g_overlay.surface.width != frame->surface.width || g_overlay.surface.height != frame->surface.height)) {
		MEMFreeToDefaultHeap(g_overlay.surface.image);
		g_overlay.surface.image = nullptr;
	}
	if (g_overlay.surface.image == nullptr && !alloc_color_buffer(g_overlay, frame->surface.width, frame->surface.height))
		return frame;
	copy_surface(&frame->surface, &g_overlay.surface);
	draw_notices(false);
	draw_speed(speed);
	g_video->drawOverlay(&g_overlay);
	return &g_overlay;
}

bool ui_gfx_copy_frame(GX2ColorBuffer * frame, GX2ColorBuffer * to)
{
	if (to->surface.image != nullptr &&
	    (to->surface.width != frame->surface.width || to->surface.height != frame->surface.height)) {
		MEMFreeToDefaultHeap(to->surface.image);
		to->surface.image = nullptr;
	}
	if (to->surface.image == nullptr && !alloc_color_buffer(*to, frame->surface.width, frame->surface.height))
		return false;
	copy_surface(&frame->surface, &to->surface);
	return true;
}

// Our Settings' Picture. GLideN64 draws into a 640x480 window, already 4:3,
// and the copy to a scan buffer fills the whole 16:9 screen, so both settings
// showed the game stretched (Zach, 2026-10-27). 4:3 now puts the frame in the
// middle of a 16:9 one, black at the sides; stretched sends it as it is.
static bool g_picture_43 = true;
static GX2ColorBuffer g_shaped;

void ui_gfx_set_picture_43(bool on) { g_picture_43 = on; }

// Settings > Picture's Filter (SHADER_, prompt.h). Scanlines, CRT and CRT
// Curved are drawn for each screen at its own size (tv_pass.cpp), into
// g_fx: for the TV WHB's TV buffer size, 1920x1080 for a 1080i/p TV, 1280x720
// for 720p, 854x480 for 480i/p; for the GamePad 854x480 (2026-11-12, before
// that the GamePad kept the plain frame). 8 MB at 1080p and 1.6 MB for the
// GamePad, so they are only kept while one of the three is chosen.
static int g_shader;
enum { FX_TV, FX_PAD, FXS };
struct Fx {
	GX2ColorBuffer buffer;
	int shape = -1;        // the Picture shape its black was drawn for, -1: none yet
	bool failed[SHADERS];  // the shader could not be built: not tried again this session
};
static Fx g_fx[FXS];

static void free_fx(Fx & fx)
{
	if (fx.buffer.surface.image == nullptr)
		return;
	GX2DrawDone(); // the last copy to the screen may still read it
	MEMFreeToDefaultHeap(fx.buffer.surface.image);
	fx.buffer.surface.image = nullptr;
	fx.shape = -1;
}

void ui_gfx_set_shader(int shader)
{
	g_shader = shader;
	if (!shader_is_filter(shader))
		for (Fx & fx : g_fx)
			free_fx(fx);
}

// A game ended: their memory goes back, and the next game's first frame allocates it again.
void ui_gfx_game_stopped()
{
	for (Fx & fx : g_fx)
		free_fx(fx);
}

#ifndef OUR64_RELEASE
// Dev build: what the GamePad's filter drew, read back once for each filter
// chosen, two seconds in: the light of its even rows against its odd rows
// over the picture's middle. Zach, 2026-11-12: "scanlines on all filters on
// gamepad arent very noticable": drawn faint (the rows not on the lines'
// middles), or drawn as on the PC (odd rows at about half) and lost on the
// way to the GamePad's screen?
static void pad_rows_check(GX2ColorBuffer & cb)
{
	static int checked = -1, frames;
	if (checked == g_shader || g_copy_ctx == nullptr)
		return;
	if (++frames < 120)
		return;
	frames = 0;
	checked = g_shader;
	GX2Surface lin;
	memset(&lin, 0, sizeof(lin));
	lin.dim = GX2_SURFACE_DIM_TEXTURE_2D;
	lin.width = cb.surface.width;
	lin.height = cb.surface.height;
	lin.depth = 1;
	lin.mipLevels = 1;
	lin.format = GX2_SURFACE_FORMAT_UNORM_R8_G8_B8_A8;
	lin.aa = GX2_AA_MODE1X;
	lin.use = GX2_SURFACE_USE_TEXTURE;
	lin.tileMode = GX2_TILE_MODE_LINEAR_ALIGNED;
	GX2CalcSurfaceSizeAndAlignment(&lin);
	lin.image = MEMAllocFromDefaultHeapEx(lin.imageSize, lin.alignment);
	if (lin.image == nullptr)
		return;
	GX2SetContextState(g_copy_ctx);
	GX2CopySurface(&cb.surface, 0, 0, &lin, 0, 0);
	GX2DrawDone();
	DCInvalidateRange(lin.image, lin.imageSize);
	const uint8_t * px = static_cast<const uint8_t *>(lin.image);
	const uint32_t W = lin.width, H = lin.height;
	uint64_t sum[2] = {0, 0};
	unsigned row16[16];
	for (uint32_t y = H / 4; y < H * 3 / 4; y++) {
		unsigned row = 0;
		for (uint32_t x = W / 3; x < W * 2 / 3; x++) {
			const uint8_t * q = px + (size_t(y) * lin.pitch + x) * 4;
			row += q[0] + q[1] + q[2];
		}
		sum[y & 1] += row;
		if (y - H / 4 < 16)
			row16[y - H / 4] = row / (3 * (W * 2 / 3 - W / 3));
	}
	const double n = double(H / 4) * 3.0 * (W * 2 / 3 - W / 3);
	report_line("TV SHADER: GamePad %s as drawn: even rows %.1f, odd rows %.1f of 255 (middle of the picture); 16 rows from %u: "
	            "%u %u %u %u %u %u %u %u %u %u %u %u %u %u %u %u", shader_name(g_shader), sum[0] / n, sum[1] / n, unsigned(H / 4),
	            row16[0], row16[1], row16[2], row16[3], row16[4], row16[5], row16[6], row16[7], row16[8], row16[9],
	            row16[10], row16[11], row16[12], row16[13], row16[14], row16[15]);
	MEMFreeToDefaultHeap(lin.image);
}
#endif

// A screen's picture through the filter, from the unshaped frame (the pass
// places the 4:3 or 16:9 picture itself), or nullptr: none chosen, the
// screen is off or shows the GamePad-only card, or the pass cannot run.
static GX2ColorBuffer * filtered(GX2ColorBuffer * frame, int which)
{
	Fx & fx = g_fx[which];
	const bool pad = which == FX_PAD;
	if (!shader_is_filter(g_shader) || g_mode == (pad ? MODE_TV : MODE_GAMEPAD) || fx.failed[g_shader])
		return nullptr;
	const GX2ColorBuffer * screen = pad ? WHBGfxGetDRCColourBuffer() : WHBGfxGetTVColourBuffer();
	if (fx.buffer.surface.image != nullptr &&
	    (fx.buffer.surface.width != screen->surface.width || fx.buffer.surface.height != screen->surface.height))
		free_fx(fx);
	if (fx.buffer.surface.image == nullptr) {
		static bool logged;
		// the heap as the watchdog measures it (main.c), once per allocation:
		// MEMAllocFromDefaultHeapEx is newlib's memalign under wut
		const struct mallinfo before = mallinfo();
		if (!alloc_color_buffer(fx.buffer, screen->surface.width, screen->surface.height)) {
			fx.buffer.surface.image = nullptr;
			if (!logged)
				report_line("TV SHADER: no memory for a %ux%u picture, the %s shows the plain one; heap %u KB in use of %u KB "
				            "taken from the system", unsigned(screen->surface.width), unsigned(screen->surface.height),
				            pad ? "GamePad" : "TV", unsigned(before.uordblks / 1024), unsigned(before.arena / 1024));
			logged = true;
			return nullptr;
		}
		const struct mallinfo after = mallinfo();
		// the TV shaders' own share of "in use" (tv_pass_held_bytes), so
		// growth between two of these lines can be told from the game's
		report_line("TV SHADER: %s%ux%u picture, %u KB; heap %u KB in use of %u KB taken before, %u KB of %u KB after; "
		            "the TV shaders hold %u KB", pad ? "GamePad " : "",
		            unsigned(screen->surface.width), unsigned(screen->surface.height), unsigned(fx.buffer.surface.imageSize / 1024),
		            unsigned(before.uordblks / 1024), unsigned(before.arena / 1024), unsigned(after.uordblks / 1024),
		            unsigned(after.arena / 1024), (tv_pass_held_bytes() + 1023) / 1024);
		fx.shape = -1;
	}
	if (!tv_pass_draw(frame, &fx.buffer, g_shader, g_picture_43, fx.shape != int(g_picture_43), pad)) {
		fx.failed[g_shader] = true;
		free_fx(fx);
		return nullptr;
	}
	fx.shape = g_picture_43;
#ifndef OUR64_RELEASE
	if (pad)
		pad_rows_check(fx.buffer);
#endif
	return &fx.buffer;
}

static GX2ColorBuffer * shaped(GX2ColorBuffer * frame)
{
	const uint32_t w = frame->surface.width, h = frame->surface.height;
	const uint32_t wide = (h * 16 / 9 + 1) & ~1u; // 854 for 480
	if (!g_picture_43 || wide <= w)
		return frame;
	if (g_shaped.surface.image != nullptr && (g_shaped.surface.width != wide || g_shaped.surface.height != h)) {
		MEMFreeToDefaultHeap(g_shaped.surface.image);
		g_shaped.surface.image = nullptr;
	}
	if (g_shaped.surface.image == nullptr) {
		if (!alloc_color_buffer(g_shaped, wide, h))
			return frame;
		// Black once: only the middle is written after.
		memset(g_shaped.surface.image, 0, g_shaped.surface.imageSize);
		DCFlushRange(g_shaped.surface.image, g_shaped.surface.imageSize);
	}
	GX2Rect from = { 0, 0, int32_t(w), int32_t(h) };
	GX2Point at = { int32_t((wide - w) / 2), 0 };
	GX2SetContextState(g_copy_ctx);
	GX2CopySurfaceEx(&frame->surface, 0, 0, &g_shaped.surface, 0, 0, 1, &from, &at);
	GX2Flush();
	return &g_shaped;
}

void ui_gfx_screens_changed() { g_card_copies = 0; }

// GPU profile marks (the frame test's, gx2_ContextImpl.cpp): each names the GPU
// work since the one before.
extern "C" void our64_gx2_prof_mark(const char * what);

void ui_gfx_scan(GX2ColorBuffer * frame)
{
	our64_gx2_prof_mark("player: notices, kept copies");
	if (g_copy_ctx == nullptr) {
		g_copy_ctx = static_cast<GX2ContextState *>(MEMAllocFromDefaultHeapEx(sizeof(GX2ContextState), GX2_CONTEXT_STATE_ALIGNMENT));
		if (g_copy_ctx != nullptr)
			GX2SetupContextStateEx(g_copy_ctx, TRUE);
	}
	// The filters draw each screen's picture at its own size; a screen
	// without one (Off, or its pass failed) gets the plain frame, shaped.
	GX2ColorBuffer * tv = filtered(frame, FX_TV);
	if (tv != nullptr)
		our64_gx2_prof_mark("TV shader");
	GX2ColorBuffer * pad = filtered(frame, FX_PAD);
	if (pad != nullptr)
		our64_gx2_prof_mark("GamePad shader");
	const bool tv_plain = tv == nullptr && !(g_mode == MODE_GAMEPAD && g_tv_card_ok);
	const bool pad_plain = pad == nullptr && g_mode != MODE_TV;
	if (g_copy_ctx != nullptr && (tv_plain || pad_plain)) {
		frame = shaped(frame);
		our64_gx2_prof_mark("4:3 copy");
	}
	uint32_t swaps = 0, flips = 0;
	OSTime last_flip, last_vsync;
	GX2GetSwapStatus(&swaps, &flips, &last_flip, &last_vsync);
	if (g_mode == MODE_GAMEPAD && g_tv_card_ok) {
		if (swaps != g_card_swap)
			g_card_copies = 0;
		if (g_card_copies < kCardCopies) {
			GX2CopyColorBufferToScanBuffer(&g_tv_card, GX2_SCAN_TARGET_TV);
			g_card_copies++;
			our64_gx2_prof_mark("TV copy (GamePad only black)");
		}
		g_card_swap = swaps + 1;
	} else {
		GX2CopyColorBufferToScanBuffer(tv != nullptr ? tv : frame, GX2_SCAN_TARGET_TV);
		our64_gx2_prof_mark("TV copy");
	}
	if (g_mode != MODE_TV) {
		GX2CopyColorBufferToScanBuffer(pad != nullptr ? pad : frame, GX2_SCAN_TARGET_DRC);
		our64_gx2_prof_mark("GamePad copy");
	}
	GX2SwapScanBuffers();
	our64_gx2_prof_mark("swap");
	GX2Flush();
	apply_screens();
}

// ---- ui.h ----

extern "C" void ui_show(const ui_view * v) { video_call(show_on_graphics_core, const_cast<ui_view *>(v)); }

namespace {
struct exit_ask_args { int focus, glyphs; const char * title, * body, * action; };
void exit_ask_on_graphics_core(void * arg)
{
	const exit_ask_args & a = *static_cast<const exit_ask_args *>(arg);
	if (!g_have_last || !ready() || !ProcUIInForeground())
		return;
	ui_view v = g_last; // the screen under the question is kept as it was
	v.exit_ask = 1;
	v.exit_focus = a.focus;
	v.exit_glyphs = a.glyphs;
	snprintf(v.ask_title, sizeof(v.ask_title), "%s", a.title ? a.title : "");
	snprintf(v.ask_body, sizeof(v.ask_body), "%s", a.body ? a.body : "");
	snprintf(v.ask_action, sizeof(v.ask_action), "%s", a.action ? a.action : "");
	frame(v);
	if (video_game_started())
		gx2::restoreState();
}
} // namespace

extern "C" void ui_show_exit_ask(int focus, int glyphs) { ui_show_ask(nullptr, nullptr, nullptr, focus, glyphs); }

extern "C" void ui_show_ask(const char * title, const char * body, const char * action, int focus, int glyphs)
{
	exit_ask_args a = { focus, glyphs, title, body, action };
	video_call(exit_ask_on_graphics_core, &a);
}

extern "C" void ui_redraw(void)
{
	if (g_have_last) {
		ui_view v = g_last;
		video_call(show_on_graphics_core, &v);
	}
}

extern "C" void ui_thumbnail(uint16_t * out) { video_call(thumb_on_graphics_core, out); }

extern "C" void ui_set_mode(int mode)
{
	g_mode = mode;
	audio_route(mode != MODE_GAMEPAD, mode != MODE_TV);
	VPADSetLcdMode(VPAD_CHAN_0, mode == MODE_TV ? VPAD_LCD_OFF : VPAD_LCD_ON);
	if (mode == MODE_GAMEPAD)
		video_call(tv_card_on_graphics_core, nullptr);
	report_line("UI: screen mode %s", mode_name(mode));
}

extern "C" int ui_mode(void) { return g_mode; }

extern "C" void ui_resume(void) { video_call(resume_on_graphics_core, nullptr); }

extern "C" void ui_restore_screens(void)
{
	if (g_mode == MODE_TV)
		VPADSetLcdMode(VPAD_CHAN_0, VPAD_LCD_ON);
	video_call(restore_on_graphics_core, nullptr);
}

extern "C" void ui_notice(int key, int kind, double secs, const char * line1, const char * line2)
{
	lock_notices();
	notices_add(&g_notices, key, kind, line1, line2, secs);
	unlock_notices();
}

extern "C" void ui_notice_clear(int key)
{
	lock_notices();
	notices_remove(&g_notices, key);
	unlock_notices();
}
