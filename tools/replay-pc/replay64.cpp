// Our64 replay64: plays captured frames (.o64f, from the console bench) through
// a mupen64plus video plugin on the PC, and saves what it draws.
//
// replay_core.so provides the core's config and video-extension functions,
// which the plugin finds through dlsym. The replay itself is
// replay/replay_engine.h, shared with the console replay app.
//
// Usage: replay64 <plugin.so> <out dir> <frame.o64f>...
// Frames are grouped by game (the file name before the last '-') and the games
// play one after another in name order, in one plugin session, like the
// console replay app. Writes <out dir>/<frame name>.ppm and prints the decode
// time of each frame.
// REPLAY_PASSES=n plays each game's frames n times; images come from the last pass.
// REPLAY_TRACE=<dir> writes the null backend's input trace (our64_trace) for
// each game to <dir>/<game>.trace, for comparing this build with the console's;
// REPLAY_TRACE_VERBOSE=1 adds every hashed value.
// OUR64_SHARED_DIR=<dir> loads GLideN64.custom.ini (per-game settings) from there,
// as the console apps do from sd:/wiiu/our64 (GLideN64/ini has the file).
// REPLAY_CONFIG="Name=value,..." overrides plugin settings (e.g.
// EnableCopyColorToRDRAM=0).
// REPLAY_BACK=1 reads the back buffer (each image one frame late): Xvfb's front
// buffer reads back black.
// REPLAY_AHEAD=1 decodes each frame with the next frame's RDRAM: what a decoder
// on its own core reads if the CPU runs on to the next graphics task. Compare
// its trace with a normal run's.
#include <dlfcn.h>

#include <chrono>
#include <cstdlib>
#include <algorithm>
#include <map>

#define M64P_CORE_PROTOTYPES 1
#include "m64p_common.h"
#include "m64p_config.h"
#include "m64p_vidext.h"

#include "../../replay/replay_engine.h"

static void write_ppm(const std::string &path, const uint8_t *rgb, int w, int h)
{
	FILE *fp = fopen(path.c_str(), "wb");
	if (!fp) return;
	fprintf(fp, "P6\n%d %d\n255\n", w, h);
	for (int y = h - 1; y >= 0; y--) // ReadScreen2 returns rows bottom-up
		fwrite(rgb + (size_t)y * w * 3, 3, w, fp);
	fclose(fp);
}

static double now_ms()
{
	return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

int main(int argc, char **argv)
{
	if (argc < 4) {
		fprintf(stderr, "usage: %s <plugin.so> <out dir> <frame.o64f>...\n", argv[0]);
		return 2;
	}
#ifdef REPLAY64_STATIC
	void *plugin = dlopen(nullptr, RTLD_NOW); // the decoder is linked in (gprof build)
#else
	void *plugin = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
#endif
	if (!plugin) { fprintf(stderr, "%s\n", dlerror()); return 1; }
	replay::Plugin p;
#define SYM(name) p.name = (ptr_##name)dlsym(plugin, #name)
	SYM(PluginStartup); SYM(InitiateGFX); SYM(RomOpen); SYM(RomClosed); SYM(ProcessDList);
	SYM(UpdateScreen); SYM(ViStatusChanged); SYM(ViWidthChanged); SYM(ReadScreen2); SYM(PluginShutdown);
	if (!p.PluginStartup || !p.InitiateGFX || !p.ProcessDList || !p.ReadScreen2) { fprintf(stderr, "not a video plugin\n"); return 1; }

	FILE **trace = (FILE **)dlsym(plugin, "our64_trace");
	const char *trace_dir = getenv("REPLAY_TRACE");
	if (trace_dir) {
		if (!trace) { fprintf(stderr, "REPLAY_TRACE needs the null-backend plugin\n"); return 1; }
		if (getenv("REPLAY_TRACE_VERBOSE"))
			*(bool *)dlsym(plugin, "our64_trace_verbose") = true;
	}

	std::map<std::string, std::vector<std::string>> games;
	for (int i = 3; i < argc; i++) {
		std::string n = argv[i];
		n = n.substr(n.find_last_of('/') + 1);
		games[n.substr(0, n.find_last_of('-'))].push_back(argv[i]);
	}

	std::string core_path = std::string(argv[0]).substr(0, std::string(argv[0]).find_last_of('/') + 1) + "replay_core.so";
	void *core = dlopen(core_path.c_str(), RTLD_NOW | RTLD_LOCAL);
	if (!core) { fprintf(stderr, "%s\n", dlerror()); return 1; }
	if (const char *cfg = getenv("REPLAY_CONFIG")) {
		auto set = (int (*)(const char *))dlsym(core, "our64_config_override");
		fprintf(stderr, "config overrides: %d\n", set(cfg));
	}
	static replay::Host host;
	const char *passes_env = getenv("REPLAY_PASSES");
	const int passes = passes_env ? atoi(passes_env) : 1;
	const bool ahead = getenv("REPLAY_AHEAD") != nullptr;
	const int front = getenv("REPLAY_BACK") ? 0 : 1;
	std::vector<uint8_t> rgb;

	for (auto &game : games) {
	std::sort(game.second.begin(), game.second.end());
	std::vector<replay::Frame> frames(game.second.size());
	for (size_t i = 0; i < frames.size(); i++)
		if (!replay::load(game.second[i].c_str(), frames[i])) { fprintf(stderr, "bad capture: %s\n", game.second[i].c_str()); return 1; }

	FILE *trace_fp = nullptr;
	if (trace_dir) {
		const std::string path = std::string(trace_dir) + "/" + game.first + ".trace";
		if (!(trace_fp = fopen(path.c_str(), "w"))) { perror(path.c_str()); return 1; }
		*trace = trace_fp;
	}
	if (!host.start(p, core, frames[0], [](void *, int level, const char *msg) {
		    if (level <= M64MSG_WARNING) fprintf(stderr, "plugin: %s\n", msg);
	    })) { fprintf(stderr, "%s\n", host.error()); return 1; }

	for (int pass = 0; pass < passes; pass++) {
		double dl_total = 0;
		for (size_t i = 0; i < frames.size(); i++) {
			const replay::Frame &f = frames[i];
			host.apply(f);
			if (ahead && i + 1 < frames.size())
				host.apply_rdram(frames[i + 1]);
			if (trace_fp) fprintf(trace_fp, "frame %s\n", f.name.c_str());
			const double t0 = now_ms();
			p.ProcessDList();
			const double t1 = now_ms();
			p.UpdateScreen();
			const double t2 = now_ms();
			// Front buffer: after the swap the back buffer still holds the
			// previous frame, which made every image one frame late.
			int w = 0, hgt = 0;
			p.ReadScreen2(nullptr, &w, &hgt, front);
			rgb.resize((size_t)w * hgt * 3);
			p.ReadScreen2(rgb.data(), &w, &hgt, front);
			write_ppm(std::string(argv[2]) + "/" + f.name + ".ppm", rgb.data(), w, hgt);
			printf("%s: VI %u, task %u, origin 0x%06X, ProcessDList %.2f ms, UpdateScreen %.2f ms, %dx%d\n",
			       f.name.c_str(), f.h.vi_count, f.h.task_index, f.h.vi[1], t1 - t0, t2 - t1, w, hgt);
			dl_total += t1 - t0;
		}
		printf("%s pass %d: ProcessDList average %.3f ms over %zu frames\n", game.first.c_str(), pass + 1,
		       dl_total / frames.size(), frames.size());
		if (trace_fp) { // like the console app, only the first pass is traced
			fclose(trace_fp);
			trace_fp = nullptr;
			*trace = nullptr;
		}
	}
	host.stop();
	}
	return 0;
}
