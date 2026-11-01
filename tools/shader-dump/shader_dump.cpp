// Our64 shader-dump: writes the shader pair the console compiles for a
// GLideN64 combiner key, on the PC. GLideN64's GLSL generators run with the
// GX2 backend's fixed GLInfo (the shim headers in GLideN64's GX2Context), the
// pair gets the backend's text rewrites (gx2_ShaderText.h), and the Linux
// CafeGLSL compiles it in uniform-register mode as gx2_GL.cpp does, printing
// the R600 disassembly.
//
// Usage: shader-dump <out dir> <combiner key hex>...
// Writes <out dir>/<key>.vert, .frag, and .asm (the disassembly, stderr of
// the compile). Keys are the "mux=" values in gx2-log.txt or the .keys files
// in sd:/wiiu/our64/shaders.
// shader-dump --cache <sources dir> <cache dir>: compiles each <key>.vert and
// <key>.frag pair the GX2 replay saved (sd:/wiiu/our64/gx2-shader-sources)
// into <cache dir>/<first two hex digits>/<key>.bin, the console's shader-cache format
// (gx2_ShaderCache.cpp), so the GX2 replay runs in Cemu, which cannot compile.
// shader-dump --pack <cache dir> <out .pack> <.keys file>: a game's pack as the
// console's preload writes it (gx2_ShaderCache.cpp: "O64K", version 1, the
// number of pairs, each as its length and .bin bytes), one pair per combiner
// key in the .keys file. A pair already in <cache dir> (the console's layout)
// is taken from there, any other is compiled and added to it (with
// OUR64_PACK_NO_COMPILE set, left out: the PC's CafeGLSL makes different,
// untested code from the same sources). Set OUR64_ROM
// to the game's internal name, as the console names the .keys file's game.
// Settings come from GLideN64's defaults through tools/replay-pc/replay_core.so
// (build it first), with the player's overrides. OUR64_ROM=<internal ROM name>
// adds that game's GLideN64.custom.ini section (from GLideN64/ini);
// OUR64_CONFIG="Name=value,..." overrides more, e.g.
// EnableInaccurateTextureCoordinates=0 for the player's exact-texcoords.
#include <dirent.h>
#include <dlfcn.h>

#include <cstdio>
#include <sys/stat.h>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include <cafeglsl/CafeGLSLCompiler.h>

#include <Graphics/OpenGLContext/GLFunctions.h>
#include <Graphics/OpenGLContext/opengl_CachedFunctions.h>
#include <Graphics/OpenGLContext/opengl_GLInfo.h>
#include <Graphics/OpenGLContext/GLSL/glsl_CombinerProgramBuilderAccurate.h>
#include <Graphics/OpenGLContext/GLSL/glsl_CombinerProgramBuilderFast.h>
#include <Graphics/GX2Context/gx2_ShaderText.h>
#include <Combiner.h>
#include <CombinerKey.h>
#include <Config.h>
#include <GBI.h>
#include <N64.h>
#include <RSP.h>
#include <PluginAPI.h>

// Combiner.cpp's Combiner_GetStages, made reachable (shader_dump_combiner.cpp).
void our64_combiner_stages(const CombinerKey & _key, Combiner & _color, Combiner & _alpha);

namespace {

struct Shader { GLenum type; std::string source; };
std::map<GLuint, Shader> g_shaders;
std::map<GLuint, std::vector<GLuint>> g_attached;
GLuint g_next = 1;
std::string g_outBase;  // <out dir>/<key>, set per key
bool g_failed = false;
// --pack: the cache folder, and the pairs gathered for the pack in key order.
std::string g_packCache;
std::vector<std::pair<u64, std::string>> g_packPairs;
int g_packFound = 0, g_packCompiled = 0, g_packMissing = 0;

void writeFile(const std::string & _path, const std::string & _text)
{
	if (FILE * f = fopen(_path.c_str(), "w")) {
		fputs(_text.c_str(), f);
		fclose(f);
	}
}

// gx2_ShaderCache.cpp's shaderCacheKey as the console computes it (the mode
// word hashed big-endian): the pair's file name in sd:/wiiu/our64/shaders, so
// a file of that name there means the console compiled exactly these sources.
u64 consoleCacheKey(const std::string & _vs, const std::string & _ps)
{
	auto fnv = [](u64 _h, const void * _data, size_t _size) {
		const u8 * p = static_cast<const u8 *>(_data);
		for (size_t i = 0; i < _size; ++i)
			_h = (_h ^ p[i]) * 0x100000001B3ull;
		return _h;
	};
	u64 h = 0xCBF29CE484222325ull;
	const char * version = GetGLSLCompilerVersion();
	if (version != nullptr)
		h = fnv(h, version, strlen(version) + 1);
	const u8 mode[4] = {0, 0, 0, u8(GLSL_COMPILE_REGISTER)};
	h = fnv(h, mode, sizeof(mode));
	h = fnv(h, _vs.c_str(), _vs.size() + 1);
	return fnv(h, _ps.c_str(), _ps.size() + 1);
}

// gx2_ShaderCache.cpp's file, written as the console writes it: words
// big-endian, the shader programs as CafeGLSL made them (the same bytes on
// both hosts).
struct BigEndianWriter {
	FILE * f;
	void bytes(const void * _p, u32 _n) { if (_n > 0) fwrite(_p, 1, _n, f); }
	void word(u32 _v) { const u8 b[4] = {u8(_v >> 24), u8(_v >> 16), u8(_v >> 8), u8(_v)}; bytes(b, 4); }
	void words(const void * _p, u32 _n) { for (u32 i = 0; i < _n; ++i) word(static_cast<const u32 *>(_p)[i]); }
	void str(const char * _s) { const u32 n = _s != nullptr ? u32(strlen(_s)) : 0; word(n); bytes(_s, n); }
	template <typename S> void common(const S * _s) {
		word(_s->size);
		bytes(_s->program, _s->size);
		word(_s->mode);
		word(_s->uniformBlockCount);
		for (u32 i = 0; i < _s->uniformBlockCount; ++i) {
			str(_s->uniformBlocks[i].name); word(_s->uniformBlocks[i].offset); word(_s->uniformBlocks[i].size);
		}
		word(_s->uniformVarCount);
		for (u32 i = 0; i < _s->uniformVarCount; ++i) {
			str(_s->uniformVars[i].name); word(_s->uniformVars[i].type); word(_s->uniformVars[i].count);
			word(_s->uniformVars[i].offset); word(u32(_s->uniformVars[i].block));
		}
		word(_s->initialValueCount);
		words(_s->initialValues, _s->initialValueCount * sizeof(GX2UniformInitialValue) / 4);
		word(_s->loopVarCount);
		words(_s->loopVars, _s->loopVarCount * sizeof(GX2LoopVar) / 4);
		word(_s->samplerVarCount);
		for (u32 i = 0; i < _s->samplerVarCount; ++i) {
			str(_s->samplerVars[i].name); word(_s->samplerVars[i].type); word(_s->samplerVars[i].location);
		}
	}
};

bool writeConsoleCache(const std::string & _path, u64 _key, GX2ShaderMode _mode, const GX2VertexShader * _vs,
	const GX2PixelShader * _ps)
{
	FILE * f = fopen(_path.c_str(), "wb");
	if (f == nullptr)
		return false;
	BigEndianWriter w{f};
	const u32 magic = 0x4F363453;  // "O64S", version 1
	w.word(magic);
	w.word(1);
	w.word(u32(_key >> 32));
	w.word(u32(_key));
	w.word(_mode);
	w.words(&_vs->regs, sizeof(_vs->regs) / 4);
	w.common(_vs);
	w.word(_vs->attribVarCount);
	for (u32 i = 0; i < _vs->attribVarCount; ++i) {
		w.str(_vs->attribVars[i].name); w.word(_vs->attribVars[i].type); w.word(_vs->attribVars[i].count);
		w.word(_vs->attribVars[i].location);
	}
	w.word(_vs->ringItemsize);
	w.word(u32(_vs->hasStreamOut));
	w.words(_vs->streamOutStride, 4);
	w.words(&_ps->regs, sizeof(_ps->regs) / 4);
	w.common(_ps);
	w.word(magic);
	const bool ok = ferror(f) == 0;
	fclose(f);
	return ok;
}

std::string readFile(const std::string & _path)
{
	std::string text;
	if (FILE * f = fopen(_path.c_str(), "rb")) {
		char buf[4096];
		size_t n;
		while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
			text.append(buf, n);
		fclose(f);
	}
	return text;
}

// --cache: every saved pair in _sources compiled into _cache.
int buildCache(const std::string & _sources, const std::string & _cache)
{
	DIR * d = opendir(_sources.c_str());
	if (d == nullptr) {
		fprintf(stderr, "cannot open %s\n", _sources.c_str());
		return 1;
	}
	InitGLSLCompiler();
	int built = 0, failed = 0;
	while (dirent * e = readdir(d)) {
		const std::string name = e->d_name;
		if (name.size() != 21 || name.substr(16) != ".vert")
			continue;
		const std::string base = _sources + "/" + name.substr(0, 16);
		const std::string vs = readFile(base + ".vert"), ps = readFile(base + ".frag");
		const u64 key = strtoull(name.substr(0, 16).c_str(), nullptr, 16);
		if (consoleCacheKey(vs, ps) != key) {
			fprintf(stderr, "%s: the sources do not hash to their name (not saved by this compiler version?)\n", base.c_str());
			++failed;
			continue;
		}
		char log[8192] = {};
		GX2VertexShader * v = nullptr;
		GX2PixelShader * p = nullptr;
		const GX2ShaderMode mode = CompileShaderPair(vs.c_str(), ps.c_str(), GLSL_COMPILE_REGISTER, &v, &p,
			log, sizeof(log), GLSL_COMPILER_FLAG_NONE);
		if (v != nullptr && p != nullptr) {
			gx2::fixTransSlotWrites(v->program, v->size, v->regs.sq_pgm_resources_vs);
			gx2::fixTransSlotWrites(p->program, p->size, p->regs.sq_pgm_resources_ps);
		}
		if (v == nullptr || p == nullptr) {
			fprintf(stderr, "%s: compile failed: %s\n", base.c_str(), log);
			++failed;
		} else if (mkdir((_cache + "/" + name.substr(0, 2)).c_str(), 0777), writeConsoleCache(_cache + "/" + name.substr(0, 2) + "/" + name.substr(0, 16) + ".bin", key, mode, v, p)) {
			++built;
		} else {
			fprintf(stderr, "%s: could not write the cache file\n", base.c_str());
			++failed;
		}
		FreeVertexShader(v);
		FreePixelShader(p);
	}
	closedir(d);
	DestroyGLSLCompiler();
	printf("%d shader pairs written to %s, %d failed\n", built, _cache.c_str(), failed);
	return failed == 0 ? 0 : 1;
}

// What gx2_GL.cpp's glLinkProgram does with the sources (but its debug rewrites). OUR64_GX2_DEBUG=<words>
// applies the deferred-discard debug variants (gx2_ShaderText.h); "earlydiscard" leaves the discards in place.
void linkPair(const std::string & _vs, const std::string & _ps)
{
	const std::string vs = gx2::flipVertexShader(gx2::addMissingOutputs(_vs, _ps));
	const char * words = getenv("OUR64_GX2_DEBUG");
	std::string ps = gx2::fetchTexelFilter(gx2::allHighp(getenv("OUR64_ROM")) ? gx2::highpEverywhere(_ps) : _ps);
	if ((words == nullptr || strstr(words, "earlydiscard") == nullptr) && !gx2::keepDiscardsInPlace(getenv("OUR64_ROM")))
		ps = gx2::deferDiscards(ps);
	if (words != nullptr)
		ps = gx2::deferredDiscardVariant(ps, words);
	ps = gx2::zlutAsRGBA8(gx2::dualSourceAsTarget1(ps));
	if (!g_packCache.empty()) {
		const u64 key = consoleCacheKey(vs, ps);
		for (const auto & pp : g_packPairs)
			if (pp.first == key)
				return;
		char name[40];
		snprintf(name, sizeof(name), "%02x/%016llx.bin", unsigned(key >> 56), (unsigned long long)key);
		const std::string path = g_packCache + "/" + name;
		std::string bytes = readFile(path);
		if (bytes.empty() && getenv("OUR64_PACK_NO_COMPILE")) {
			++g_packMissing;
			return;
		}
		if (bytes.empty()) {
			char log[8192] = {};
			GX2VertexShader * v = nullptr;
			GX2PixelShader * p = nullptr;
			const GX2ShaderMode mode = CompileShaderPair(vs.c_str(), ps.c_str(), GLSL_COMPILE_REGISTER, &v, &p,
				log, sizeof(log), GLSL_COMPILER_FLAG_NONE);
			if (v != nullptr && p != nullptr) {
				gx2::fixTransSlotWrites(v->program, v->size, v->regs.sq_pgm_resources_vs);
				gx2::fixTransSlotWrites(p->program, p->size, p->regs.sq_pgm_resources_ps);
			}
			mkdir((g_packCache + "/" + std::string(name, 2)).c_str(), 0777);
			if (v == nullptr || p == nullptr || !writeConsoleCache(path, key, mode, v, p)) {
				fprintf(stderr, "%016llx: compile failed: %s\n", (unsigned long long)key, log);
				g_failed = true;
			}
			FreeVertexShader(v);
			FreePixelShader(p);
			bytes = readFile(path);
			if (!bytes.empty())
				++g_packCompiled;
		} else {
			++g_packFound;
		}
		if (!bytes.empty())
			g_packPairs.emplace_back(key, bytes);
		return;
	}
	writeFile(g_outBase + ".vert", vs);
	writeFile(g_outBase + ".frag", ps);
	// The disassembly goes to stderr: send it to the .asm file.
	fflush(stderr);
	FILE * asmFile = freopen((g_outBase + ".asm").c_str(), "w", stderr);
	char log[8192] = {};
	GX2VertexShader * v = nullptr;
	GX2PixelShader * p = nullptr;
	const GX2ShaderMode mode = CompileShaderPair(vs.c_str(), ps.c_str(), GLSL_COMPILE_REGISTER, &v, &p,
		log, sizeof(log), GLSL_COMPILER_FLAG_PRINT_DISASSEMBLY_TO_STDERR);
	// Then the pixel shader's registers and uniform offsets (in 32-bit
	// words; the disassembly's constant KC2[n] is word 4n).
	if (v != nullptr) {
		// Each vertex output's semantic: spi_vs_out_id holds four per word,
		// PARAM 4n..4n+3 from the low byte up.
		fprintf(stderr, "\n--- vertex shader outputs\nspi_vs_out_config %08x", v->regs.spi_vs_out_config);
		for (u32 i = 0; i < v->regs.num_spi_vs_out_id; ++i)
			fprintf(stderr, " spi_vs_out_id[%u] %08x", i, v->regs.spi_vs_out_id[i]);
		fprintf(stderr, "\n");
	}
	if (p != nullptr) {
		fprintf(stderr, "\n--- pixel shader registers\n");
		fprintf(stderr, "sq_pgm_resources_ps %08x sq_pgm_exports_ps %08x spi_ps_in_control_0 %08x spi_ps_in_control_1 %08x\n",
			p->regs.sq_pgm_resources_ps, p->regs.sq_pgm_exports_ps, p->regs.spi_ps_in_control_0, p->regs.spi_ps_in_control_1);
		fprintf(stderr, "cb_shader_mask %08x cb_shader_control %08x db_shader_control %08x spi_input_z %08x\n",
			p->regs.cb_shader_mask, p->regs.cb_shader_control, p->regs.db_shader_control, p->regs.spi_input_z);
		for (u32 i = 0; i < p->regs.num_spi_ps_input_cntl; ++i)
			fprintf(stderr, "spi_ps_input_cntl[%u] %08x\n", i, p->regs.spi_ps_input_cntls[i]);
		fprintf(stderr, "--- pixel uniforms\n");
		for (u32 i = 0; i < p->uniformVarCount; ++i)
			fprintf(stderr, "%-24s type %2u count %u offset %3u (KC2[%u])\n", p->uniformVars[i].name,
				unsigned(p->uniformVars[i].type), p->uniformVars[i].count, p->uniformVars[i].offset,
				p->uniformVars[i].offset / 4);
		for (u32 i = 0; i < p->samplerVarCount; ++i)
			fprintf(stderr, "sampler %-16s location %u\n", p->samplerVars[i].name, p->samplerVars[i].location);
	}
	fflush(stderr);
	(void)asmFile;
	freopen("/dev/tty", "w", stderr);
	if (v == nullptr || p == nullptr) {
		fprintf(stderr, "%s: compile failed: %s\n", g_outBase.c_str(), log);
		g_failed = true;
	} else {
		// The fix the console applies after each compile (the disassembly above is from before it).
		const u32 fixed = gx2::fixTransSlotWrites(v->program, v->size, v->regs.sq_pgm_resources_vs) +
			gx2::fixTransSlotWrites(p->program, p->size, p->regs.sq_pgm_resources_ps);
		printf("%s: mode %d, vertex %u bytes, pixel %u bytes, console cache file %016llx.bin%s\n", g_outBase.c_str(),
			int(mode), unsigned(v->size), unsigned(p->size), (unsigned long long)consoleCacheKey(vs, ps),
			fixed > 0 ? (", trans slot writes moved: " + std::to_string(fixed)).c_str() : "");
	}
	FreeVertexShader(v);
	FreePixelShader(p);
}

} // namespace

// ---- the GL calls GLideN64's GLSL generators make ----
extern "C" {
GLuint APIENTRY glCreateShader(GLenum type) { g_shaders[g_next] = Shader{type, ""}; return g_next++; }
void APIENTRY glShaderSource(GLuint shader, GLsizei count, const GLchar * const * string, const GLint * length)
{
	std::string & s = g_shaders[shader].source;
	s.clear();
	for (GLsizei i = 0; i < count; ++i)
		s.append(string[i], length != nullptr && length[i] >= 0 ? size_t(length[i]) : strlen(string[i]));
}
void APIENTRY glCompileShader(GLuint) {}
void APIENTRY glDeleteShader(GLuint) {}
void APIENTRY glGetShaderiv(GLuint, GLenum, GLint * params) { *params = GL_TRUE; }
void APIENTRY glGetShaderInfoLog(GLuint, GLsizei, GLsizei * length, GLchar * infoLog) { if (length) *length = 0; if (infoLog) *infoLog = 0; }
GLuint APIENTRY glCreateProgram(void) { return g_next++; }
void APIENTRY glAttachShader(GLuint program, GLuint shader) { g_attached[program].push_back(shader); }
void APIENTRY glBindAttribLocation(GLuint, GLuint, const GLchar *) {}
void APIENTRY glProgramParameteri(GLuint, GLenum, GLint) {}
void APIENTRY glLinkProgram(GLuint program)
{
	std::string vs, ps;
	for (GLuint s : g_attached[program])
		(g_shaders[s].type == GL_VERTEX_SHADER ? vs : ps) = g_shaders[s].source;
	if (!g_outBase.empty())
		linkPair(vs, ps);
}
void APIENTRY glGetProgramiv(GLuint, GLenum, GLint * params) { *params = GL_TRUE; }
void APIENTRY glGetProgramInfoLog(GLuint, GLsizei, GLsizei * length, GLchar * infoLog) { if (length) *length = 0; if (infoLog) *infoLog = 0; }
void APIENTRY glDeleteProgram(GLuint) {}
void APIENTRY glUseProgram(GLuint) {}
GLint APIENTRY glGetUniformLocation(GLuint, const GLchar *) { static GLint n; return n++; }
void APIENTRY glUniform1i(GLint, GLint) {}
void APIENTRY glUniform2i(GLint, GLint, GLint) {}
void APIENTRY glUniform4i(GLint, GLint, GLint, GLint, GLint) {}
void APIENTRY glUniform1f(GLint, GLfloat) {}
void APIENTRY glUniform2f(GLint, GLfloat, GLfloat) {}
void APIENTRY glUniform3fv(GLint, GLsizei, const GLfloat *) {}
void APIENTRY glUniform4fv(GLint, GLsizei, const GLfloat *) {}
const GLubyte * APIENTRY glGetString(GLenum) { return reinterpret_cast<const GLubyte *>(""); }
void APIENTRY glGetIntegerv(GLenum, GLint * data) { *data = 0; }
void APIENTRY glBlendFunc(GLenum, GLenum) {}
void APIENTRY glGetProgramBinary(GLuint, GLsizei, GLsizei * length, GLenum *, void *) { if (length) *length = 0; }
void APIENTRY glProgramBinary(GLuint, GLenum, const void *, GLsizei) {}
}

int main(int argc, char ** argv)
{
	if (argc == 4 && strcmp(argv[1], "--cache") == 0)
		return buildCache(argv[2], argv[3]);
	// --asm <.vert> <.frag>: compiles a pair as it is (no rewrites) and prints the disassembly.
	if (argc == 4 && strcmp(argv[1], "--asm") == 0) {
		InitGLSLCompiler();
		char log[8192] = {};
		GX2VertexShader * v = nullptr;
		GX2PixelShader * p = nullptr;
		CompileShaderPair(readFile(argv[2]).c_str(), readFile(argv[3]).c_str(), GLSL_COMPILE_REGISTER, &v, &p,
			log, sizeof(log), GLSL_COMPILER_FLAG_PRINT_DISASSEMBLY_TO_STDERR);
		if (v == nullptr || p == nullptr)
			fprintf(stderr, "compile failed: %s\n", log);
		FreeShaders(v, p);
		DestroyGLSLCompiler();
		return v != nullptr && p != nullptr ? 0 : 1;
	}
	std::vector<std::string> keys;
	std::string packOut;
	if (argc == 5 && strcmp(argv[1], "--pack") == 0) {
		g_packCache = argv[2];
		packOut = argv[3];
		std::string text = readFile(argv[4]);
		for (size_t at = 0; at < text.size();) {
			const size_t end = text.find('\n', at) == std::string::npos ? text.size() : text.find('\n', at);
			const std::string line = text.substr(at, end - at);
			if (!line.empty() && line[0] != '#')
				keys.push_back(line);
			at = end + 1;
		}
	} else if (argc < 3) {
		fprintf(stderr, "usage: %s <out dir> <combiner key hex>...\n       %s --cache <sources dir> <cache dir>\n"
			"       %s --pack <cache dir> <out .pack> <.keys file>\n", argv[0], argv[0], argv[0]);
		return 2;
	} else {
		for (int i = 2; i < argc; ++i)
			keys.push_back(argv[i]);
	}
	// The player's settings (player/source/video.cpp), then the backend's own
	// (gx2_ContextImpl.cpp, init).
	const std::string self = argv[0];
	const std::string corePath = self.substr(0, self.find_last_of('/') + 1) + "../replay-pc/replay_core.so";
	void * core = dlopen(corePath.c_str(), RTLD_NOW | RTLD_LOCAL);
	if (core == nullptr) {
		fprintf(stderr, "%s (make -C tools/replay-pc)\n", dlerror());
		return 1;
	}
	std::string overrides = "EnableCopyColorToRDRAM=0,AsyncShaderCompilation=1,EnableInaccurateTextureCoordinates=1";
	if (const char * more = getenv("OUR64_CONFIG"))
		overrides += std::string(",") + more;
	reinterpret_cast<int (*)(const char *)>(dlsym(core, "our64_config_override"))(overrides.c_str());
	const std::string shared = self.substr(0, self.find_last_of('/') + 1) + "../../third_party/gliden64/ini";
	reinterpret_cast<void (*)(const char *)>(dlsym(core, "our64_config_shared_dir"))(shared.c_str());
	const char * rom = getenv("OUR64_ROM");
	api().PluginStartup(core, nullptr, nullptr);
	if (rom != nullptr && rom[0] != '\0') {
		// RSP_Init, as the player's RomOpen runs it, from a header holding only
		// the name: it sets the per-game hacks (Majora's Mask's hack_ZeldaMM),
		// which change the shader text and so the cache keys.
		static u8 header[64];
		for (int i = 0; i < 20; ++i)
			header[(32 + i) ^ BYTE_ADDR_XOR] = i < int(strlen(rom)) ? rom[i] : ' ';
		HEADER = header;
		RSP_Init();
	}
	Config_LoadConfig();
	config.frameBufferEmulation.N64DepthCompare = Config::dcDisable;
	config.video.multisampling = 0;
	config.video.fxaa = 0;

	opengl::GLInfo info;
	info.majorVersion = 3;
	info.minorVersion = 3;
	info.dual_source_blending = true;
	info.parallelShaderCompile = true;
	info.drawElementsBaseVertex = true;
	info.depthTexture = true;
	opengl::CachedUseProgram useProgram;

	InitGLSLCompiler();
	std::unique_ptr<glsl::CombinerProgramBuilder> builder;
	if (config.generalEmulation.enableInaccurateTextureCoordinates)
		builder = std::make_unique<glsl::CombinerProgramBuilderFast>(info, &useProgram);
	else
		builder = std::make_unique<glsl::CombinerProgramBuilderAccurate>(info, &useProgram);

	for (const std::string & k : keys) {
		const u64 mux = strtoull(k.c_str(), nullptr, 16);
		const CombinerKey key(mux, false);
		GBI.setHWLSupported(key.isHWLSupported());
		Combiner color, alpha;
		our64_combiner_stages(key, color, alpha);
		char base[64];
		snprintf(base, sizeof(base), "/%016llx", (unsigned long long)mux);
		g_outBase = packOut.empty() ? std::string(argv[1]) + base : packOut;
		delete builder->buildCombinerProgram(color, alpha, key);
		g_outBase.clear();
	}
	DestroyGLSLCompiler();
	if (!packOut.empty()) {
		if (FILE * f = fopen(packOut.c_str(), "wb")) {
			BigEndianWriter w{f};
			w.word(0x4F36344B); // "O64K"
			w.word(1);
			w.word(u32(g_packPairs.size()));
			for (const auto & pp : g_packPairs) {
				w.word(u32(pp.second.size()));
				w.bytes(pp.second.data(), u32(pp.second.size()));
			}
			g_failed |= ferror(f) != 0;
			fclose(f);
		} else {
			g_failed = true;
		}
		printf("%s: %zu pairs for %zu keys (%d from the cache, %d compiled, %d not in the cache)\n", packOut.c_str(),
			g_packPairs.size(), keys.size(), g_packFound, g_packCompiled, g_packMissing);
	}
	return g_failed ? 1 : 0;
}
