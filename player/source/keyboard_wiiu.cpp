// Our64 player: text entry with the Wii U's own keyboard (nn::swkbd), the one
// the Wii U Menu shows (Zach, 2026-11-11: what every owner already knows; for
// Rename in Game options and the Our ROMs search). It is drawn over Our64's
// last screen on the TV and the GamePad, and takes the GamePad's touch and
// buttons and every Wii Remote and Pro Controller.
//
// GX2 belongs to the graphics core, so the keyboard is made, fed and drawn
// there: keyboard_ask posts the opening with video_call and then redraws the
// last screen each frame; the video driver calls the hook below after the
// screen's own drawing and before it presents.
#include <string>

#include <coreinit/filesystem.h>
#include <coreinit/memdefaultheap.h>
#include <nn/swkbd.h>
#include <padscore/kpad.h>
#include <vpad/input.h>
#include <whb/gfx.h>

#include "video_ui.h"

extern "C" {
#include "pause_wiiu.h"
#include "report.h"
#include "ui.h"
extern void (*our64_ui_before_present)(void); // third_party/libgui's WutVideoDriver.cpp
}

namespace {

enum { KB_IDLE, KB_SHOWN, KB_OK, KB_CANCEL };
bool g_created;
FSClient * g_fs;
void * g_work;
volatile int g_state = KB_IDLE;
std::u16string g_initial, g_result;
bool g_opened;

// Each frame, on the graphics core: the controllers to the keyboard, its own
// bookkeeping, what the player decided, and its picture on both screens.
void frame_hook()
{
	if (g_state == KB_IDLE)
		return;
	nn::swkbd::ControllerInfo info;
	static VPADStatus vpad;
	static KPADStatus kpad[4];
	VPADReadError ve = VPAD_READ_NO_SAMPLES;
	if (VPADRead(VPAD_CHAN_0, &vpad, 1, &ve) > 0 && ve == VPAD_READ_SUCCESS) {
		VPADGetTPCalibratedPoint(VPAD_CHAN_0, &vpad.tpNormal, &vpad.tpNormal);
		info.vpad = &vpad;
	}
	for (int ch = 0; ch < 4; ch++) {
		KPADError ke = KPAD_ERROR_NO_SAMPLES;
		if (KPADReadEx(KPADChan(ch), &kpad[ch], 1, &ke) > 0 && ke == KPAD_ERROR_OK)
			info.kpad[ch] = &kpad[ch];
	}
	nn::swkbd::Calc(info);
	if (nn::swkbd::IsNeedCalcSubThreadFont())
		nn::swkbd::CalcSubThreadFont();
	if (nn::swkbd::IsNeedCalcSubThreadPredict())
		nn::swkbd::CalcSubThreadPredict();
	if (g_state == KB_SHOWN) {
		if (nn::swkbd::IsDecideOkButton(nullptr)) {
			const char16_t * s = nn::swkbd::GetInputFormString();
			g_result = s != nullptr ? s : u"";
			nn::swkbd::DisappearInputForm();
			g_state = KB_OK;
		} else if (nn::swkbd::IsDecideCancelButton(nullptr)) {
			nn::swkbd::DisappearInputForm();
			g_state = KB_CANCEL;
		}
	}
	WHBGfxBeginRenderTV();
	nn::swkbd::DrawTV();
	WHBGfxBeginRenderDRC();
	nn::swkbd::DrawDRC();
}

void open_on_graphics_core(void *)
{
	g_opened = false;
	if (!g_created) {
		g_fs = static_cast<FSClient *>(MEMAllocFromDefaultHeap(sizeof(FSClient)));
		if (g_fs == nullptr || FSAddClient(g_fs, FS_ERROR_FLAG_NONE) != FS_STATUS_OK) {
			report_line("KEYBOARD: no file system client");
			return;
		}
		const uint32_t size = nn::swkbd::GetWorkMemorySize(0);
		g_work = MEMAllocFromDefaultHeap(size);
		if (g_work == nullptr) {
			report_line("KEYBOARD: no memory for its %u KB", size / 1024);
			return;
		}
		// The keyboard's files are the console's region's: the first that starts.
		static const nn::swkbd::RegionType regions[] = { nn::swkbd::RegionType::USA, nn::swkbd::RegionType::Europe,
		                                                 nn::swkbd::RegionType::Japan };
		for (const nn::swkbd::RegionType region : regions) {
			nn::swkbd::CreateArg create;
			create.regionType = region;
			create.workMemory = g_work;
			create.fsClient = g_fs;
			if (nn::swkbd::Create(create)) {
				g_created = true;
				report_line("KEYBOARD: the system keyboard, %u KB of work memory, region %d", size / 1024, int(region));
				break;
			}
		}
		if (!g_created) {
			report_line("KEYBOARD: the system keyboard did not start");
			MEMFreeToDefaultHeap(g_work);
			g_work = nullptr;
			return;
		}
	}
	nn::swkbd::AppearArg appear;
	appear.keyboardArg.configArg.languageType = nn::swkbd::LanguageType::English;
	appear.keyboardArg.configArg.disableNewLine = true;
	appear.keyboardArg.configArg.showWordSuggestions = false;
	appear.inputFormArg.initialText = g_initial.c_str();
	appear.inputFormArg.maxTextLength = 100;
	appear.inputFormArg.higlightInitialText = true;
	if (!nn::swkbd::AppearInputForm(appear)) {
		report_line("KEYBOARD: its input form did not open");
		return;
	}
	g_result.clear();
	g_state = KB_SHOWN;
	our64_ui_before_present = frame_hook;
	g_opened = true;
}

void cancel_on_graphics_core(void *)
{
	if (g_state == KB_SHOWN) {
		nn::swkbd::DisappearInputForm();
		g_state = KB_CANCEL;
	}
}

void hidden_on_graphics_core(void * arg)
{
	*static_cast<bool *>(arg) = nn::swkbd::GetStateInputForm() == nn::swkbd::State::Hidden;
}

void close_on_graphics_core(void *)
{
	our64_ui_before_present = nullptr;
	g_state = KB_IDLE;
}

// UTF-8 to and from the keyboard's UTF-16 (the basic plane; anything else becomes '?').
std::u16string widen(const char * s)
{
	std::u16string out;
	const unsigned char * p = reinterpret_cast<const unsigned char *>(s ? s : "");
	while (*p) {
		uint32_t c = *p++;
		int more = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;
		if (more)
			c &= 0x3F >> more;
		for (; more > 0 && (*p & 0xC0) == 0x80; more--)
			c = c << 6 | (*p++ & 0x3F);
		out.push_back(c < 0x10000 && more == 0 ? char16_t(c) : u'?');
	}
	return out;
}

std::string narrow(const std::u16string & s)
{
	std::string out;
	for (const char16_t c : s) {
		if (c < 0x80) {
			out.push_back(char(c));
		} else if (c < 0x800) {
			out.push_back(char(0xC0 | c >> 6));
			out.push_back(char(0x80 | (c & 0x3F)));
		} else if (c >= 0xD800 && c <= 0xDFFF) {
			out.push_back('?');
		} else {
			out.push_back(char(0xE0 | c >> 12));
			out.push_back(char(0x80 | ((c >> 6) & 0x3F)));
			out.push_back(char(0x80 | (c & 0x3F)));
		}
	}
	return out;
}

} // namespace

// The keyboard over the last screen, starting from `initial`. 1: OK was
// chosen, the text in out (UTF-8); 0: cancelled; -1: the keyboard could not
// be shown. Any thread but the graphics core's (it waits on it).
extern "C" int keyboard_ask(const char * initial, char * out, unsigned size)
{
	g_initial = widen(initial);
	video_call(open_on_graphics_core, nullptr);
	if (!g_opened)
		return -1;
	while (g_state == KB_SHOWN) {
		ui_redraw();
		player_menu_wait();
		if (player_closing())
			video_call(cancel_on_graphics_core, nullptr);
	}
	// its closing animation, a second at most
	for (int i = 0; i < 60; i++) {
		bool hidden = false;
		video_call(hidden_on_graphics_core, &hidden);
		if (hidden)
			break;
		ui_redraw();
		player_menu_wait();
	}
	const bool ok = g_state == KB_OK;
	video_call(close_on_graphics_core, nullptr);
	ui_redraw();
	if (!ok)
		return 0;
	snprintf(out, size, "%s", narrow(g_result).c_str());
	return 1;
}
