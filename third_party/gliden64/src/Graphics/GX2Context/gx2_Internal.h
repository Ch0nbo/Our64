#pragma once
// Our64 GX2 backend: what gx2_GL.cpp (shader programs) and gx2_ContextImpl.cpp
// (state, textures, draws) share.
#include <string>
#include <vector>
#include <gx2/shaders.h>
#include <Types.h>

namespace gx2 {

// Vertex layouts the backend copies GLideN64's vertices into. The fetch
// shader of each program reads one of them, chosen by its attribute names.
struct TriangleVertex {
	f32 x, y, z, w;
	f32 r, g, b, a;
	f32 s, t;
	f32 numLights;
	u8 modify[4];   // MODIFY_XY, MODIFY_Z, MODIFY_ST, MODIFY_RGBA bytes, low byte first
};

struct RectVertex {
	f32 x, y, z, w;
	f32 s0, t0, s1, t1;
};

// A sampler of a program and the texture unit glUniform1i gave it.
struct SamplerBinding {
	u32 location;
	u32 unit;
	bool vertex;
};

// A linked program: CafeGLSL's shader pair, its fetch shader and the values
// of its loose uniforms, kept as images of the uniform registers.
struct Program {
	u32 name = 0;
	GX2VertexShader * vs = nullptr;
	GX2PixelShader * ps = nullptr;
	GX2ShaderMode mode = GX2_SHADER_MODE_UNIFORM_REGISTER;
	GX2FetchShader fetch = {};
	u8 * fetchProgram = nullptr;
	bool rectLayout = false;
	bool linked = false;
	bool cached = false;  // vs and ps came from the shader cache, not CafeGLSL
	std::string log;
	std::string psSource;  // kept for the "srclog=" debug variant only
	bool sourceLogged = false;
	u32 dbShaderControl = 0;  // the pixel shader's own DB_SHADER_CONTROL, before debug variants
	std::vector<u32> vsRegs, psRegs;
	bool vsDirty = true, psDirty = true;
	std::vector<SamplerBinding> samplers;
};

// Compiled shader pairs saved on the SD card (gx2_ShaderCache.cpp).
u64 shaderCacheKey(const std::string & _vs, const std::string & _ps, u32 _mode);
extern "C" volatile u64 our64_gx2_cache_read_ticks; // gx2_ShaderCache.cpp
bool shaderCacheLoad(u64 _key, GX2VertexShader ** _vs, GX2PixelShader ** _ps, GX2ShaderMode * _mode);
void shaderCacheStore(u64 _key, const GX2VertexShader * _vs, const GX2PixelShader * _ps, GX2ShaderMode _mode);
void freeCachedShaders(GX2VertexShader * _vs, GX2PixelShader * _ps);

// Combiner keys a game has used, kept beside the shader cache.
std::vector<u64> loadCombinerKeys(const char * _game);
void saveCombinerKeys(const char * _game, const std::vector<u64> & _keys);
// A game's pairs read from one file while its combiners load (between these
// two); the counts say where the pairs came from.
void shaderPackOpen(const char * _game);
void shaderPackClose(unsigned * _fromPack, unsigned * _fromFiles);

// How glLinkProgram treats a shader pair missing from the shader cache:
// compile it on the worker (Normal), leave the program unlinked (CacheOnly,
// counted by deferredLinks), or compile it on the worker after every job a
// draw is waiting for (Background).
enum class LinkMode { Normal, CacheOnly, Background };
void setLinkMode(LinkMode _mode);
u32 deferredLinks();

// The program glUseProgram selected, or null.
Program * currentProgram();

// Starts CafeGLSL once.
void initCompiler();

// Times compiling each .frag file in _dir on each core (log only).
void shaderBench(const char * _dir);

// Ends the shader compile worker (gx2_GL.cpp) once its queue is empty; the
// next link starts it again.
void stopCompileThread();

// Writes a message to the backend log (sd:/wiiu/our64/gx2-log.txt in the
// replay app).
void log(const char * _format, ...) __attribute__((format(printf, 1, 2)));

// glBlendFunc is the one state call the GLSL code makes itself.
extern void (*blendFuncHook)(u32 _sfactor, u32 _dfactor);

}
