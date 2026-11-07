// Our64 replay app (stage 2): plays the frames the bench captured
// (sd:/wiiu/our64/frames/<game>-<VI>.o64f) through GLideN64 with the null
// backend, and times ProcessDList per frame. Those times decide the graphics
// decoder (docs/feasibility.md, stage 2).
//
// For each game it also writes the null backend's input trace to
// sd:/wiiu/our64/traces/<game>.trace, to compare with the PC build
// (tools/replay-pc/replay64 with REPLAY_TRACE): equal traces mean the
// big-endian build decodes like the little-endian one.
#include <coreinit/thread.h>
#include <coreinit/time.h>
#include <whb/log.h>
#include <whb/log_cafe.h>
#include <whb/log_console.h>
#include <whb/log_udp.h>
#include <whb/proc.h>
#include <proc_ui/procui.h>
#include <sysapp/launch.h>
#include <vpad/input.h>

#include <dirent.h>
#include <sys/stat.h>

#include <algorithm>
#include <atomic>
#include <map>

#define M64P_PLUGIN_PROTOTYPES 1
#include "../replay_engine.h"

extern "C" {
#include "report.h"
}
#include "profile.h"

extern "C" int our64_config_override(const char * list); // core_config.cpp
extern "C" void our64_config_shared_dir(const char * dir);
extern uint64_t our64_cmd_ticks[256]; // GLideN64 RSP.cpp, OUR64_CMD_TIMING
extern uint32_t our64_cmd_count[256];
#include <Graphics/NullContext/null_ContextImpl.h>
#include <gDP.h>
#ifdef OUR64_GX2_GFX
#include <Graphics/GX2Context/gx2_ContextImpl.h>
#include <Graphics/GX2Context/gx2_Internal.h>
#include <gx2/display.h>
#include <gx2/state.h>
#include <gx2/event.h>
#include <gx2/swap.h>
#include <whb/gfx.h>
extern const char * our64_gx2_log_path;
extern "C" const char * our64_gx2_shader_dir; // gx2_ShaderCache.cpp
extern "C" const char * our64_gx2_shader_source_dir; // gx2_GL.cpp
#endif

namespace {

// Set by the main thread when HOME is pressed: the replay stops after the
// current frame. The replay thread holds the GPU, so the main thread can't
// hand the console back to the system until it ends.
std::atomic<bool> g_quit{false};

constexpr int kTimedPasses = 5;
constexpr int kProfiledPasses = 10; // after the timed ones, sampled every 100 us

double now_ms() { return report_now_us() / 1000.0; }

// Frames by game: the part of the file name before the last '-'.
std::map<std::string, std::vector<std::string>> list_frames(const std::string & dir)
{
	std::map<std::string, std::vector<std::string>> games;
	if (DIR * d = opendir(dir.c_str())) {
		while (dirent * e = readdir(d)) {
			std::string n = e->d_name;
			if (n.size() < 6 || n.substr(n.size() - 5) != ".o64f" || n.find('-') == std::string::npos) continue;
			games[n.substr(0, n.find_last_of('-'))].push_back(dir + "/" + n);
		}
		closedir(d);
	}
	for (auto & g : games) std::sort(g.second.begin(), g.second.end());
	return games;
}

// The display-list commands that took most of the profiled passes' time.
void report_commands(const std::string & game, double total_ms, size_t frames)
{
	const double tick_ms = 1000.0 / OSTimerClockSpeed;
	int order[256];
	for (int i = 0; i < 256; i++) order[i] = i;
	std::sort(order, order + 256, [](int a, int b) { return our64_cmd_ticks[a] > our64_cmd_ticks[b]; });
	double in_cmds = 0;
	for (int i = 0; i < 256; i++) in_cmds += our64_cmd_ticks[i] * tick_ms;
	char line[400];
	int n = snprintf(line, sizeof(line), "%s: commands %.0f%% of ProcessDList; top:", game.c_str(),
	                 100.0 * in_cmds / total_ms);
	for (int k = 0; k < 8 && our64_cmd_ticks[order[k]] && n < (int)sizeof(line) - 40; k++)
		n += snprintf(line + n, sizeof(line) - n, " %02X %.0f%% (%.2f ms, %u/frame)", order[k],
		              100.0 * our64_cmd_ticks[order[k]] * tick_ms / total_ms,
		              our64_cmd_ticks[order[k]] * tick_ms / frames, unsigned(our64_cmd_count[order[k]] / frames));
	report_line("%s", line);
}

#ifdef OUR64_GX2_GFX
// Debug variant runs (gx2-debug.txt): the suffix ("-d<n>") on each saved image,
// and the one frame the variant applies to ("frame=<name>"; empty: all).
std::string g_variant_suffix, g_variant_frame;
const char * g_variant_debug = nullptr;
// sd:/wiiu/our64/gx2-hold.txt, "<frame name> <seconds>": that frame stays on the
// TV that long after it is drawn (each pass with a debug variant).
std::string g_hold_frame;
unsigned g_hold_seconds = 0;

// Saves what the GX2 backend shows as sd:/wiiu/our64/gx2/<frame>.ppm, to
// compare with the PC's OpenGL images (tools/replay-pc/replay64).
void save_screen(const std::string & root, const std::string & base)
{
	const std::string name = base + g_variant_suffix;
	int w = 0, h = 0;
	ReadScreen2(nullptr, &w, &h, 0);
	if (w <= 0 || h <= 0) return;
	std::vector<uint8_t> rgb(size_t(w) * h * 3);
	ReadScreen2(rgb.data(), &w, &h, 0);
	const std::string path = root + "/gx2/" + name + ".ppm";
	FILE * f = fopen(path.c_str(), "wb");
	if (!f) return;
	fprintf(f, "P6\n%d %d\n255\n", w, h);
	for (int y = h - 1; y >= 0; y--) // rows come bottom-up, as from glReadPixels
		fwrite(rgb.data() + size_t(y) * w * 3, 3, w, f);
	fclose(f);
}

// Shows each finished frame on the TV and the GamePad.
void present(GX2ColorBuffer * screen)
{
	GX2CopyColorBufferToScanBuffer(screen, GX2_SCAN_TARGET_TV);
	GX2CopyColorBufferToScanBuffer(screen, GX2_SCAN_TARGET_DRC);
	GX2SwapScanBuffers();
	GX2Flush();
	GX2SetTVEnable(TRUE);
	GX2SetDRCEnable(TRUE);
}
#endif

// config: the index of the settings run (0 = GLideN64's defaults). Only run 0
// writes traces; each run writes its own profiles.
void replay_game(const std::string & root, const std::string & game, const std::vector<std::string> & paths,
                 unsigned config)
{
	std::vector<replay::Frame> frames(paths.size());
	for (size_t i = 0; i < paths.size(); i++)
		if (!replay::load(paths[i].c_str(), frames[i])) {
			report_line("%s: bad capture %s", game.c_str(), paths[i].c_str());
			return;
		}

	replay::Plugin p = {PluginStartup, InitiateGFX, RomOpen, RomClosed, ProcessDList,
	                    UpdateScreen, ViStatusChanged, ViWidthChanged, ReadScreen2, PluginShutdown};
	// Pass 1 writes the trace, from the plugin's start like the PC tool's; the
	// timed passes after it run without one.
	const std::string trace_path = root + "/traces/" + game + ".trace";
#ifdef OUR64_GX2_GFX
	our64_trace = nullptr;  // the trace hooks are in the null backend
#else
	our64_trace = config == 0 ? fopen(trace_path.c_str(), "w") : nullptr;
	if (config == 0 && !our64_trace) report_line("%s: cannot write %s", game.c_str(), trace_path.c_str());
#endif
	// A file named "verbose" next to the traces adds every hashed value.
	struct stat st;
	our64_trace_verbose = stat((root + "/traces/verbose").c_str(), &st) == 0;

	static replay::Host host;
	if (!host.start(p, nullptr, frames[0], [](void *, int level, const char * msg) {
		    if (level <= M64MSG_WARNING) report_line("plugin: %s", msg);
	    })) {
		report_line("%s: %s", game.c_str(), host.error());
		return;
	}

	std::vector<double> best(frames.size(), 1e9);
	unsigned draws = 0;
	for (int pass = 0; pass <= kTimedPasses; pass++) {
#ifdef OUR64_GX2_GFX
		if (pass > 0 && !g_variant_suffix.empty())
			break; // debug variants only need the images
#endif
		for (size_t i = 0; i < frames.size() && !g_quit; i++) {
#ifdef OUR64_GX2_GFX
			if (pass == 0) { // a hang leaves the frame it was on in the log
				report_line("%s: replaying", frames[i].name.c_str());
				report_save();
			}
			if (!g_variant_frame.empty())
				our64_gx2_debug = frames[i].name == g_variant_frame ? g_variant_debug : nullptr;
#endif
			host.apply(frames[i]);
			if (our64_trace) fprintf(our64_trace, "frame %s\n", frames[i].name.c_str());
			const double t0 = now_ms();
			ProcessDList();
			const double t1 = now_ms();
			UpdateScreen();
#ifdef OUR64_GX2_GFX
			if (pass == 0) save_screen(root, frames[i].name);
			// Cemu never writes GPU results back to memory, so the saved images
			// are black there: gx2-hold keeps one frame on the TV for a screenshot.
			if (pass == 0 && frames[i].name == g_hold_frame) {
				report_line("%s: held on screen for %u s", frames[i].name.c_str(), g_hold_seconds);
				report_save();
				OSSleepTicks(OSSecondsToTicks(g_hold_seconds));
			}
#endif
			if (pass > 0) best[i] = std::min(best[i], t1 - t0);
		}
		if (pass == 0) draws = our64_null_draws;
		if (our64_trace) {
			fclose(our64_trace);
			our64_trace = nullptr;
		}
	}
#ifndef OUR64_GX2_GFX
	// Profiled passes: sampling costs time, so they are not timed. They also
	// total the time in each display-list command.
	memset(our64_cmd_ticks, 0, sizeof(our64_cmd_ticks));
	memset(our64_cmd_count, 0, sizeof(our64_cmd_count));
	double prof_ms = 0;
	// Sampling is gated to ProcessDList: host.apply copies 8 MB per frame and
	// otherwise takes most of the samples.
	for (int pass = 0; pass < kProfiledPasses; pass++)
		for (size_t i = 0; i < frames.size() && !g_quit; i++) {
			host.apply(frames[i]);
			const double t0 = now_ms();
			profile_start(100);
			ProcessDList();
			profile_stop();
			prof_ms += now_ms() - t0;
			UpdateScreen();
		}
	report_commands(game, prof_ms, kProfiledPasses * frames.size());
	const std::string prof_path = root + "/profile/" + game + "-c" + std::to_string(config) + ".prof";
	if (!profile_save(prof_path.c_str())) report_line("%s: cannot write %s", game.c_str(), prof_path.c_str());
#endif
	host.stop();

	double sum = 0;
	for (size_t i = 0; i < frames.size(); i++) {
		report_line("%s: VI %u, ProcessDList %.2f ms", frames[i].name.c_str(), frames[i].h.vi_count, best[i]);
		sum += best[i];
	}
	report_line("%s: average %.2f ms per frame (best of %d passes), %u draws in pass 1", game.c_str(),
	            sum / frames.size(), kTimedPasses, draws);
	report_save();
}

// The big-endian bitfield layouts in gDP.h must put each field on the N64
// command bits the little-endian ones use.
bool check_bitfields()
{
	gDPInfo::OtherMode m;
	m._u64 = 0;
	m.cycleType = 3;      // G_MDSFT_CYCLETYPE 20, high word
	m.alphaCompare = 3;   // G_MDSFT_ALPHACOMPARE 0, low word
	m.c1_m1a = 2;         // blender bits 30-31, low word
	bool ok = m.h == (3u << 20) && m.l == (3u | 2u << 30) && m._u64 == ((u64(3u << 20) << 32) | m.l);
	gDPCombine c;
	c.mux = 0;
	c.saRGB0 = 0xF;       // bits 52-55 of the SETCOMBINE command
	c.aA1 = 7;            // bits 0-2
	ok = ok && c.muxs0 == (0xFu << 20) && c.muxs1 == 7u && c.mux == ((u64(0xFu << 20) << 32) | 7u);
	gDPTile t;
	t.cms = 0;
	t.cmt = 0;
	t.clamps = 1;         // G_TX_CLAMP is bit 1
	t.mirrort = 1;        // G_TX_MIRROR is bit 0
	ok = ok && t.cms == 2u && t.cmt == 1u;
	return ok;
}

int replay_thread(int, const char **)
{
	if (!check_bitfields()) {
		report_line("FAILED: gDP.h bitfield layout does not match the N64 bits.");
		return 0;
	}
	const char * root = report_sd_root();
	if (!root) {
		report_line("No SD card.");
		return 0;
	}
	const std::string r = root;
	our64_config_shared_dir(root); // GLideN64's per-game settings: sd:/wiiu/our64/GLideN64.custom.ini
	mkdir((r + "/traces").c_str(), 0777);
	mkdir((r + "/profile").c_str(), 0777);
#ifdef OUR64_GX2_GFX
	mkdir((r + "/gx2").c_str(), 0777);
	static std::string log_path = r + "/gx2-log.txt";
	remove(log_path.c_str());
	our64_gx2_log_path = log_path.c_str();
	// The player's compiled shaders (sd:/wiiu/our64/shaders), when
	// gx2-shader-cache exists: Cemu hangs in the shader compiler, so this is
	// what lets the GX2 replay run there.
	static std::string shader_dir = r + "/shaders";
	struct stat cache_flag;
	if (stat((r + "/gx2-shader-cache").c_str(), &cache_flag) == 0)
		our64_gx2_shader_dir = shader_dir.c_str();
	if (FILE * f = fopen((r + "/gx2-hold.txt").c_str(), "r")) {
		char name[128] = {};
		if (fscanf(f, "%127s %u", name, &g_hold_seconds) >= 1)
			g_hold_frame = name;
		if (g_hold_seconds == 0) g_hold_seconds = 5;
		fclose(f);
	}
	// sd:/wiiu/our64/gx2-shader-sources (a folder): shaders missing from the
	// cache are saved there instead of compiled, for tools/shader-dump --cache
	// on the PC to build into the cache (the way to fill it for Cemu).
	static std::string source_dir = r + "/gx2-shader-sources";
	if (stat(source_dir.c_str(), &cache_flag) == 0 && S_ISDIR(cache_flag.st_mode))
		our64_gx2_shader_source_dir = source_dir.c_str();
	// One run, with the color copy to RDRAM off: the GX2 backend has no readback yet.
	// Texture coordinates use the simpler shaders, as in the player (video.cpp):
	// with the exact ones, Mario Golf's swing meter replayed clean while it
	// speckled in the player.
	std::vector<std::string> configs = {"EnableCopyColorToRDRAM=0,EnableInaccurateTextureCoordinates=1"};
#else
	// Every game runs with GLideN64's defaults, then once more for each line of
	// sd:/wiiu/our64/replay-config.txt ("Name=value,Name=value", the same
	// overrides as REPLAY_CONFIG on the PC).
	std::vector<std::string> configs = {""};
	if (FILE * f = fopen((r + "/replay-config.txt").c_str(), "r")) {
		char line[512];
		while (fgets(line, sizeof(line), f)) {
			std::string l = line;
			while (!l.empty() && (l.back() == '\n' || l.back() == '\r' || l.back() == ' ')) l.pop_back();
			if (!l.empty() && l[0] != '#') configs.push_back(l);
		}
		fclose(f);
	}
#endif
	auto games = list_frames(r + "/frames");
	// sd:/wiiu/our64/gx2-games.txt (GX2 flavor) or replay-games.txt (null flavor), one
	// game name per line, limits the run (for debugging).
#ifdef OUR64_GX2_GFX
	if (FILE * f = fopen((r + "/gx2-games.txt").c_str(), "r")) {
#else
	if (FILE * f = fopen((r + "/replay-games.txt").c_str(), "r")) {
#endif
		std::map<std::string, std::vector<std::string>> only;
		char line[256];
		while (fgets(line, sizeof(line), f)) {
			std::string l = line;
			while (!l.empty() && (l.back() == '\n' || l.back() == '\r' || l.back() == ' ')) l.pop_back();
			// "<game> last=<n>": only its last n frames. Each frame holds a whole
			// RDRAM image (up to 8 MB), so a long capture does not fit in memory.
			size_t keep = 0;
			const size_t space = l.find(" last="); // names have spaces ("Resident Evil 2 (USA) (Rev 1)-v7335")
			if (space != std::string::npos) {
				sscanf(l.c_str() + space + 1, "last=%zu", &keep);
				l.resize(space);
			}
			if (games.count(l)) {
				std::vector<std::string> paths = games[l];
				if (keep > 0 && keep < paths.size())
					paths.erase(paths.begin(), paths.end() - keep);
				only[l] = paths;
			}
		}
		fclose(f);
		games = only;
	}
	report_line("%u game(s) in sd:/wiiu/our64/frames, %u setting(s)", (unsigned)games.size(), (unsigned)configs.size());
#ifdef OUR64_GX2_GFX
	// sd:/wiiu/our64/gx2-debug.txt: one variant per line, a GX2 debug variant (see
	// our64_gx2_debug) or "config:Name=value,..." for GLideN64 settings; each
	// runs every game once more, saving <frame>-d<n>.ppm.
	std::vector<std::string> variants;
	if (FILE * f = fopen((r + "/gx2-debug.txt").c_str(), "r")) {
		char line[256];
		while (fgets(line, sizeof(line), f)) {
			std::string l = line;
			while (!l.empty() && (l.back() == '\n' || l.back() == '\r' || l.back() == ' ')) l.pop_back();
			if (!l.empty() && l[0] != '#') variants.push_back(l);
		}
		fclose(f);
	}
	for (unsigned v = 0; v < variants.size(); v++) {
		report_line(" ");
		report_line("== GX2 debug variant d%u: %s", v + 1, variants[v].c_str());
		// "config:Name=value,..." changes GLideN64 settings; anything else is a GX2 debug variant.
		const bool settings = variants[v].compare(0, 7, "config:") == 0;
		our64_gx2_debug = settings ? nullptr : variants[v].c_str();
		g_variant_debug = our64_gx2_debug;
		const size_t fp = variants[v].find("frame=");
		g_variant_frame = fp == std::string::npos ? "" : variants[v].substr(fp + 6, variants[v].find(' ', fp) - fp - 6);
		if (!g_variant_frame.empty()) our64_gx2_debug = nullptr;
		g_variant_suffix = "-d" + std::to_string(v + 1);
		const std::string list = settings ? configs[0] + "," + variants[v].substr(7) : configs[0];
		our64_config_override(list.c_str());
		for (const auto & g : games) {
			if (g_quit) return 0;
			replay_game(r, g.first, g.second, 0);
		}
	}
	our64_gx2_debug = nullptr;
	g_variant_frame.clear();
	g_variant_suffix.clear();
	if (!variants.empty()) return 0;
#endif
	if (games.empty()) report_line("Run Our64 CPU Bench first: it captures the frames.");
	for (unsigned c = 0; c < configs.size(); c++) {
		report_line(" ");
		report_line("== settings c%u: %s", c, configs[c].empty() ? "GLideN64 defaults" : configs[c].c_str());
		our64_config_override(configs[c].c_str());
		for (const auto & g : games) {
			if (g_quit) {
				report_line("Stopped: HOME pressed.");
				return 0;
			}
			our64_null_draws = 0;
			replay_game(r, g.first, g.second, c);
		}
	}
	return 0;
}

#ifdef OUR64_GX2_GFX
// Debug: the exit path is logged to gx2-log.txt until HOME works in this flavor.
#define EXIT_LOG(...) gx2::log(__VA_ARGS__)
#else
#define EXIT_LOG(...) do {} while (0)
#endif

bool home_pressed()
{
	static uint32_t last_hold = 0;
	VPADStatus status;
	VPADReadError error;
	if (VPADRead(VPAD_CHAN_0, &status, 1, &error) <= 0 || error != VPAD_READ_SUCCESS)
		return false;
	if (status.hold != last_hold) {
		EXIT_LOG("vpad hold %08x\n", (unsigned)status.hold);
		last_hold = status.hold;
	}
	return (status.trigger & VPAD_BUTTON_HOME) != 0;
}

uint32_t on_release(void *)
{
	EXIT_LOG("procui: release foreground\n");
	return 0;
}

uint32_t on_exit(void *)
{
	EXIT_LOG("procui: exit\n");
	return 0;
}

OSThread g_thread __attribute__((aligned(16)));
uint8_t g_stack[8 * 1024 * 1024] __attribute__((aligned(16)));

} // namespace

int main(int, char **)
{
	WHBProcInit();
	WHBLogCafeInit();
	WHBLogUdpInit();
#ifdef OUR64_GX2_GFX
	// The TV shows the frames, so there is no text console.
	WHBGfxInit();
	our64_gx2_present = present;
	ProcUIRegisterCallback(PROCUI_CALLBACK_RELEASE, on_release, nullptr, 50);
	ProcUIRegisterCallback(PROCUI_CALLBACK_EXIT, on_exit, nullptr, 50);
	report_line("Our64 stage-3 replay (" __DATE__ " " __TIME__ "): GLideN64 GX2 backend");
#else
	WHBLogConsoleInit();
	report_line("Our64 stage-2 replay (" __DATE__ " " __TIME__ "): GLideN64 null backend");
#endif

	// Its own thread for a larger stack; core 1, where the decoder will run.
	if (OSCreateThread(&g_thread, replay_thread, 0, nullptr, g_stack + sizeof(g_stack), sizeof(g_stack), 16,
	                   OS_THREAD_ATTRIB_AFFINITY_CPU1)) {
		OSResumeThread(&g_thread);
		while (!OSIsThreadTerminated(&g_thread)) {
			if (home_pressed()) g_quit = true;
			OSSleepTicks(OSMillisecondsToTicks(50));
		}
		int ret;
		OSJoinThread(&g_thread, &ret);
	}
	profile_shutdown();

	report_line(" ");
	report_line("Done. Press HOME to exit.");
	if (!report_save()) report_line("Results were not saved to SD.");
	// The system's HOME menu doesn't open in this app, so HOME (pressed now or
	// during the run) goes straight back to the Wii U Menu.
	bool leaving = false;
#ifdef OUR64_GX2_GFX
	our64_gx2_present = nullptr;
	EXIT_LOG("done screen\n");
	// Render WHBGfx frames as other GX2 apps do while waiting: the system
	// takes the screen back (HOME menu, exit) between them. Dark green = done.
	while (WHBProcIsRunning()) {
		if (!leaving && (g_quit || home_pressed())) {
			EXIT_LOG("HOME: launching the Wii U Menu\n");
			leaving = true;
			SYSLaunchMenu();
		}
		WHBGfxBeginRender();
		WHBGfxBeginRenderTV();
		WHBGfxClearColor(0.0f, 0.3f, 0.1f, 1.0f);
		WHBGfxFinishRenderTV();
		WHBGfxBeginRenderDRC();
		WHBGfxClearColor(0.0f, 0.3f, 0.1f, 1.0f);
		WHBGfxFinishRenderDRC();
		WHBGfxFinishRender();
	}
	EXIT_LOG("left the loop\n");
	WHBGfxShutdown();
#else
	while (WHBProcIsRunning()) {
		if (!leaving && (g_quit || home_pressed())) {
			leaving = true;
			SYSLaunchMenu();
		}
		WHBLogConsoleDraw();
		OSSleepTicks(OSMillisecondsToTicks(50));
	}
	WHBLogConsoleFree();
#endif
	WHBLogUdpDeinit();
	WHBLogCafeDeinit();
	WHBProcShutdown();
	return 0;
}
