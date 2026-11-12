// Our64 player: Settings > Picture's filters (Scanlines, CRT, CRT Curved).
// The game's frame is drawn through one of tv_shaders.h's programs into a
// buffer at the TV's own size, which then goes to the TV in place of the
// frame (ui_wiiu.cpp's ui_gfx_scan), and into one at the GamePad's size for
// the GamePad (2026-11-12), the lines there locked to its rows (tv_mesh.h).
// On the graphics core, while a game runs.
//
// The programs go through the GX2 backend's GL layer (gx2_GL.cpp): it starts
// CafeGLSL, compiles on its worker under its compiler lock, keeps the result
// in the SD shader cache (sd:/wiiu/our64/shaders), and builds the fetch
// shader for the RectVertex layout. The draw itself is ours, in a GPU state
// of its own. Scanlines and CRT draw one quad; CRT Curved a grid that
// carries its bend (tv_mesh.h), in two draws: its inner cells through CRT's
// pixel shader, its edge cells through its own.
#include <cstdio>
#include <cstring>

#include <coreinit/cache.h>
#include <coreinit/memdefaultheap.h>
#include <coreinit/time.h>
#include <gx2/clear.h>
#include <gx2/context.h>
#include <gx2/draw.h>
#include <gx2/enum.h>
#include <gx2/event.h>
#include <gx2/mem.h>
#include <gx2/registers.h>
#include <gx2/sampler.h>
#include <gx2/shaders.h>
#include <gx2/state.h>
#include <gx2/texture.h>
#include <gx2/utils.h>

#include <Graphics/OpenGLContext/GLFunctions.h> // the GX2 backend's GL layer
#include <Graphics/GX2Context/gx2_Internal.h>
#include <VI.h> // GLideN64's view of the game's video mode (VI.PAL)

#include "prompt.h"
#include "tv_mesh.h"
#include "tv_shaders.h"
#include "video_ui.h"
extern "C" {
#include "report.h"
}

// In gx2.rpl, not declared by wut: GX2SampleBottomGPUCycle has the GPU write
// a 64-bit counter to the address once all earlier work is done (as the
// backend's "gpuprof" uses it): an EVENT_WRITE_EOP packet, bottom of the pipe,
// DATA_SEL 3 ("GPU clock counter"), written big-endian (ENDIAN_SWAP 8IN64),
// as decaf-emu's gx2_query.cpp builds it. That counter is not the shader
// clock: R6xx/R7xx GPUs run it at their reference crystal (Mesa's r600
// converts its timestamps with clock_crystal_freq), and GX2GetGPUSystemClock
// is the shader clock (549999755 Hz), so dividing by it gave 0.058 ms for a
// pass over 1.5 million pixels (2026-11-04). gx2.rpl's own conversion is
// GX2GPUTimeToCPUTime (counter to OSTime): the rate here comes from it.
extern "C" void GX2SampleBottomGPUCycle(uint64_t * sample);
extern "C" uint64_t GX2GPUTimeToCPUTime(uint64_t gpu_time);

namespace {

// The shader pairs: Scanlines, CRT, and CRT Curved's two (its inner cells,
// CRT's pixel shader with its vertex shader, and its edge cells); the
// GamePad's CRT Curved has its own two (kTvCurvedRowsVertexShader), its
// Scanlines and CRT are the TV's.
enum { PROG_SCANLINES, PROG_CRT, PROG_CURVED_INNER, PROG_CURVED_EDGE, PROG_PAD_CURVED_INNER, PROG_PAD_CURVED_EDGE, PROGS };
enum { TV, PAD, TARGETS };
struct TvProgram {
	GLuint name = 0;
	GLint size = -1; // uSize
	bool tried = false, ok = false;
	const gx2::Program * gx2 = nullptr; // once drawn: what it holds, for the log
};
TvProgram g_programs[PROGS];
GX2ContextState * g_ctx;
GX2Sampler g_sampler;
gx2::RectVertex * g_quad; // 8: the TV's quad, then the GamePad's
uint32_t g_pad_quad_h;    // the rows the GamePad's quad was made for, 0: not yet
bool g_setup_tried, g_setup_ok;

// CRT Curved's grid, one for the TV and one for the GamePad, each for the
// picture size it was built for; allocated the first time CRT Curved is
// drawn there, at the largest grid's size, and kept.
struct Mesh {
	TvMeshVertex * v;
	uint16_t * index;
	TvMesh info;
	uint32_t w, h; // 0: not built
	bool drawn;    // the GPU may still read it
};
Mesh g_meshes[TARGETS];

// The pass's GPU time, the TV's and the GamePad's each: two clock samples
// around it, read a frame or more later (never waited for), summed and
// logged for the first kTimeLogs 10 s windows of each game and of each
// shader chosen during one: a whole game's worth of lines would fill
// report.c's repeat filter.
constexpr unsigned kTimeLogs = 3;
struct Timing {
	unsigned logs;
	int shader = -1;
	uint64_t * stamps;
	bool armed;
	unsigned armed_frames;
	uint64_t gpu_sum;
	unsigned gpu_n;
	OSTime log_at;
};
Timing g_timing[TARGETS];

bool g_order_known, g_swapped; // the counter as written, or byte-swapped
double g_rate;                  // counts a second (measure_rate); 0 until the first timed pass

// prompt.c's shader_name and shader_is_filter, here so the GX2 Replay app can
// link this file alone (its gx2-tvfx.txt test switch).
bool tv_only(int shader) { return shader >= SHADER_SCANLINES && shader < SHADERS; }
const char * tv_name(int shader)
{
	return shader == SHADER_SCANLINES ? "Scanlines" : shader == SHADER_CRT ? "CRT" : "CRT Curved";
}

const char * pixel_source(int prog)
{
	return prog == PROG_SCANLINES ? kTvScanlinesShader
	     : prog == PROG_CURVED_EDGE || prog == PROG_PAD_CURVED_EDGE ? kTvCurvedShader : kTvCrtShader;
}

const char * vertex_source(int prog)
{
	return prog == PROG_PAD_CURVED_INNER || prog == PROG_PAD_CURVED_EDGE ? kTvCurvedRowsVertexShader
	     : prog == PROG_CURVED_INNER || prog == PROG_CURVED_EDGE ? kTvCurvedVertexShader : kTvVertexShader;
}

// A shader's first and last program on a screen.
int first_program(int shader, int target)
{
	return shader == SHADER_SCANLINES ? PROG_SCANLINES : shader == SHADER_CRT ? PROG_CRT
	     : target == PAD ? PROG_PAD_CURVED_INNER : PROG_CURVED_INNER;
}
int last_program(int shader, int target) { return first_program(shader, target) + (shader == SHADER_CURVED ? 1 : 0); }

bool setup()
{
	if (g_setup_tried)
		return g_setup_ok;
	g_setup_tried = true;
	g_ctx = static_cast<GX2ContextState *>(MEMAllocFromDefaultHeapEx(sizeof(GX2ContextState), GX2_CONTEXT_STATE_ALIGNMENT));
	g_quad = static_cast<gx2::RectVertex *>(MEMAllocFromDefaultHeapEx(8 * sizeof(gx2::RectVertex), GX2_VERTEX_BUFFER_ALIGNMENT));
	for (Timing & t : g_timing)
		t.stamps = static_cast<uint64_t *>(MEMAllocFromDefaultHeapEx(64, 64));
	if (g_ctx == nullptr || g_quad == nullptr || g_timing[TV].stamps == nullptr || g_timing[PAD].stamps == nullptr) {
		report_line("TV SHADER: no memory for its GPU state");
		return false;
	}
	GX2SetupContextStateEx(g_ctx, TRUE);
	// The whole quad; the viewport places it. The frame's rows are top
	// first, so its first row (t = 0) goes to the top (y = 1).
	// The frame less its overscan (tv_mesh.h).
	const float lo = tv_overscan(0.0), hi = tv_overscan(1.0);
	const gx2::RectVertex quad[4] = {
		{-1.0f, -1.0f, 0.0f, 1.0f, lo, hi, 0.0f, 0.0f},
		{ 1.0f, -1.0f, 0.0f, 1.0f, hi, hi, 0.0f, 0.0f},
		{-1.0f,  1.0f, 0.0f, 1.0f, lo, lo, 0.0f, 0.0f},
		{ 1.0f,  1.0f, 0.0f, 1.0f, hi, lo, 0.0f, 0.0f},
	};
	memcpy(g_quad, quad, sizeof(quad));
	memcpy(g_quad + 4, quad, sizeof(quad)); // the GamePad's, until pad_quad
	GX2Invalidate(GX2_INVALIDATE_MODE_CPU_ATTRIBUTE_BUFFER, g_quad, 2 * sizeof(quad));
	GX2InitSampler(&g_sampler, GX2_TEX_CLAMP_MODE_CLAMP, GX2_TEX_XY_FILTER_MODE_LINEAR);
	g_setup_ok = true;
	return true;
}

// The GamePad's quad for a screen of h rows: the frame less the overscan
// that locks the lines to its rows up and down (tv_mesh.h). Made once (the
// GamePad's rows never change).
void pad_quad(uint32_t h)
{
	if (g_pad_quad_h == h)
		return;
	if (g_pad_quad_h != 0)
		GX2DrawDone(); // the last draw may still read it
	const double o = tv_rows_overscan(h);
	const float lo = tv_overscan(0.0), hi = tv_overscan(1.0), top = tv_overscan(0.0, o), bottom = tv_overscan(1.0, o);
	const gx2::RectVertex quad[4] = {
		{-1.0f, -1.0f, 0.0f, 1.0f, lo, bottom, 0.0f, 0.0f},
		{ 1.0f, -1.0f, 0.0f, 1.0f, hi, bottom, 0.0f, 0.0f},
		{-1.0f,  1.0f, 0.0f, 1.0f, lo, top, 0.0f, 0.0f},
		{ 1.0f,  1.0f, 0.0f, 1.0f, hi, top, 0.0f, 0.0f},
	};
	memcpy(g_quad + 4, quad, sizeof(quad));
	GX2Invalidate(GX2_INVALIDATE_MODE_CPU_ATTRIBUTE_BUFFER, g_quad + 4, sizeof(quad));
	g_pad_quad_h = h;
}

// Compiled the first time the shader is used (CafeGLSL, or the SD cache on
// later runs); a shader that fails is not tried again this session.
bool build_program(int shader, int prog)
{
	TvProgram & t = g_programs[prog];
	if (t.tried)
		return t.ok;
	t.tried = true;
	gx2::initCompiler();
	const char * vs = vertex_source(prog);
	const char * ps = pixel_source(prog);
	const GLuint v = glCreateShader(GL_VERTEX_SHADER);
	glShaderSource(v, 1, &vs, nullptr);
	const GLuint f = glCreateShader(GL_FRAGMENT_SHADER);
	glShaderSource(f, 1, &ps, nullptr);
	const GLuint program = glCreateProgram();
	glAttachShader(program, v);
	glAttachShader(program, f);
	glLinkProgram(program);
	glDeleteShader(v);
	glDeleteShader(f);
	GLint linked = GL_FALSE;
	glGetProgramiv(program, GL_LINK_STATUS, &linked); // waits for the compile
	if (linked != GL_TRUE) {
		char log[1024];
		glGetProgramInfoLog(program, sizeof(log), nullptr, log);
		report_line("TV SHADER: %s%s did not compile, the plain picture is shown: %s", prog >= PROG_PAD_CURVED_INNER ? "GamePad " : "", tv_name(shader), log);
		glDeleteProgram(program);
		return false;
	}
	t.name = program;
	t.size = glGetUniformLocation(program, "uSize");
	t.ok = true;
	return true;
}

bool build(int shader, int target)
{
	const int first = first_program(shader, target), last = last_program(shader, target);
	if (g_programs[last].tried)
		return g_programs[first].ok && g_programs[last].ok;
	const OSTime start = OSGetSystemTime();
	bool ok = true;
	for (int prog = first; prog <= last && ok; prog++)
		ok = build_program(shader, prog);
	g_programs[last].tried = true; // tried once, even if the first failed
	if (ok)
		report_line("TV SHADER: %s%s ready in %u ms", first >= PROG_PAD_CURVED_INNER ? "GamePad " : "", tv_name(shader),
		            unsigned(OSTicksToMilliseconds(OSGetSystemTime() - start)));
	return ok;
}

// CRT Curved's grid for a w x h picture: built when the picture's size
// changes (its shape, or the TV's size), after the GPU is done with the last
// one (a moment's wait, then). overscan_v: tv_mesh_build's.
bool build_mesh(Mesh & m, uint32_t w, uint32_t h, double overscan_v)
{
	if (m.v == nullptr) {
		m.v = static_cast<TvMeshVertex *>(MEMAllocFromDefaultHeapEx(kTvMeshMaxVertices * sizeof(TvMeshVertex), GX2_VERTEX_BUFFER_ALIGNMENT));
		m.index = static_cast<uint16_t *>(MEMAllocFromDefaultHeapEx(kTvMeshMaxIndices * sizeof(uint16_t), GX2_INDEX_BUFFER_ALIGNMENT));
		if (m.v == nullptr || m.index == nullptr) {
			report_line("TV SHADER: no memory for CRT Curved's grid");
			if (m.v != nullptr)
				MEMFreeToDefaultHeap(m.v);
			if (m.index != nullptr)
				MEMFreeToDefaultHeap(m.index);
			m.v = nullptr;
			m.index = nullptr;
			return false;
		}
	}
	if (w == m.w && h == m.h)
		return true;
	if (m.drawn)
		GX2DrawDone();
	m.info = tv_mesh_build(m.v, m.index, w, h, kTvMeshBend, overscan_v);
	GX2Invalidate(GX2_INVALIDATE_MODE_CPU_ATTRIBUTE_BUFFER, m.v, m.info.vertices * sizeof(TvMeshVertex));
	GX2Invalidate(GX2_INVALIDATE_MODE_CPU_ATTRIBUTE_BUFFER, m.index, (m.info.inner + m.info.edge) * sizeof(uint16_t));
	m.w = w;
	m.h = h;
	return true;
}

// A program's shaders, its uniform registers and the frame's texture, for
// the next draw.
void bind(const gx2::Program * p, const GX2Texture * tex)
{
	GX2SetShaderMode(p->mode);
	GX2SetFetchShader(&p->fetch);
	GX2SetVertexShader(p->vs);
	GX2SetPixelShader(p->ps);
	if (!p->vsRegs.empty())
		GX2SetVertexUniformReg(0, p->vsRegs.size(), p->vsRegs.data());
	if (!p->psRegs.empty())
		GX2SetPixelUniformReg(0, p->psRegs.size(), p->psRegs.data());
	for (const gx2::SamplerBinding & s : p->samplers)
		if (!s.vertex) {
			GX2SetPixelTexture(tex, s.location);
			GX2SetPixelSampler(&g_sampler, s.location);
		}
}

// A sample the GPU has written: not the 0 put there first, nor the all-ones
// "no value" GX2 (and Cemu, which never writes one) leaves.
bool written(uint64_t v) { return v != 0 && v != ~0ull; }

// The counter's rate, from gx2.rpl's own conversion (GX2GPUTimeToCPUTime:
// counter to OSTime) over a span of a billion counts, logged once. It said
// 27 MHz, and two console runs measured 26.97 and 27.10 MHz against the
// system clock (2026-11-04); a third measured 14.3 MHz, its first sample
// seen seconds after the GPU wrote it (a pause between), so the rate is no
// longer measured.
void measure_rate()
{
	if (g_rate > 0.0)
		return;
	const uint64_t span = 1000000000ull;
	const uint64_t c0 = GX2GPUTimeToCPUTime(span), c1 = GX2GPUTimeToCPUTime(2 * span);
	g_rate = c1 > c0 ? double(span) / (double(c1 - c0) / double(OSTimerClockSpeed)) : 27000000.0;
	report_line("TV SHADER: GPU timestamps tick at %.0f Hz (GX2GPUTimeToCPUTime)", g_rate);
}

// The last samples, if the GPU has written them; then the log line every 10 s.
void collect_time(Timing & g, int target, int shader, const GX2ColorBuffer * out)
{
	if (shader != g.shader) {
		g.shader = shader;
		g.logs = 0;
		g.gpu_sum = 0;
		g.gpu_n = 0;
		g.log_at = 0;
	}
	const OSTime now = OSGetSystemTime();
	if (g.armed) {
		DCInvalidateRange(g.stamps, 64);
		uint64_t a = g.stamps[0], b = g.stamps[1];
		if (written(a) && written(b)) {
			// Written big-endian, so as read; should it come byte-swapped,
			// that counts up by far less: decided on the first pair, kept.
			if (!g_order_known && b != a) {
				g_swapped = __builtin_bswap64(b) - __builtin_bswap64(a) < b - a;
				g_order_known = true;
			}
			if (g_swapped) {
				a = __builtin_bswap64(a);
				b = __builtin_bswap64(b);
			}
			if (b > a && b - a < 1000000000ull) {
				g.gpu_sum += b - a;
				g.gpu_n++;
				measure_rate();
			}
			g.armed = false;
		} else if (++g.armed_frames > 120) {
			g.armed = false; // never written (Cemu does not): try again
		}
	}
	if (g.log_at == 0)
		g.log_at = now;
	if (now - g.log_at < OSSecondsToTicks(10))
		return;
	g.log_at = now;
	if (g.gpu_n == 0 || g.logs >= kTimeLogs)
		return;
	g.logs++;
	const double counts = double(g.gpu_sum) / g.gpu_n;
	report_line("TV SHADER: %s%s at %ux%u, %.3f ms GPU a frame (%u frames timed, %.0f counts at %.0f Hz)",
	            target == PAD ? "GamePad " : "", tv_name(shader), unsigned(out->surface.width),
	            unsigned(out->surface.height), counts * 1000.0 / g_rate, g.gpu_n, counts, g_rate);
	g.gpu_sum = 0;
	g.gpu_n = 0;
}

} // namespace

unsigned tv_pass_held_bytes()
{
	unsigned n = 0;
	if (g_ctx != nullptr)
		n += sizeof(GX2ContextState);
	if (g_quad != nullptr)
		n += 8 * sizeof(gx2::RectVertex);
	for (const Timing & t : g_timing)
		if (t.stamps != nullptr)
			n += 64;
	for (const Mesh & m : g_meshes)
		if (m.v != nullptr)
			n += kTvMeshMaxVertices * sizeof(TvMeshVertex) + kTvMeshMaxIndices * sizeof(uint16_t);
	// a compiled pair as the GL layer keeps it: the shaders' headers and
	// code, the fetch shader and the uniform register images
	for (const TvProgram & t : g_programs)
		if (const gx2::Program * p = t.gx2) {
			n += sizeof(gx2::Program) + p->fetch.size + (p->vsRegs.size() + p->psRegs.size()) * sizeof(uint32_t);
			if (p->vs != nullptr)
				n += sizeof(GX2VertexShader) + p->vs->size;
			if (p->ps != nullptr)
				n += sizeof(GX2PixelShader) + p->ps->size;
		}
	return n;
}

void tv_pass_game_stopped()
{
	for (Timing & t : g_timing)
		t.shader = -1; // the next game logs its first windows again
}

bool tv_pass_draw(GX2ColorBuffer * frame, GX2ColorBuffer * out, int shader, bool picture_43, bool clear, bool gamepad)
{
	const int target = gamepad ? PAD : TV;
	if (!tv_only(shader) || !setup() || !build(shader, target))
		return false;
	const uint32_t W = out->surface.width, H = out->surface.height;
	uint32_t w = picture_43 ? (H * 4 / 3 + 1) & ~1u : W;
	if (w > W)
		w = W;
	const uint32_t x = (W - w) / 2;
	const bool curved = shader == SHADER_CURVED;
	Mesh & mesh = g_meshes[target];
	Timing & timing = g_timing[target];
	if (curved && !build_mesh(mesh, w, H, gamepad ? tv_rows_overscan(H) : double(kTvOverscan)))
		return false;
	if (gamepad)
		pad_quad(H);
	collect_time(timing, target, shader, out);
	const int first = first_program(shader, target), last = last_program(shader, target);

	// The game's lines: a TV field's, 288 on PAL, 240 on NTSC (GLideN64 puts
	// a field's lines on the frame's rows, 2 rows a line, whatever the VI's
	// window). A game in an interlaced 480-line mode (VI.real_height, the
	// lines it shows, 474 to 480 or up to 576, against about 240 or 288 for
	// the others) has twice that: each of its rows is a line, which the
	// shaders draw without gaps.
	const float field = VI.PAL ? 288.0f : 240.0f;
	// On the GamePad the lines are groups of its own rows, not the game's
	// (tv_mesh.h's tv_rows_lines and kTvPadRows); for an interlaced game
	// every row is a line, which draws no gaps there either.
	const bool interlaced = VI.real_height * 2 > unsigned(field * 3);
	const float pad_rows = interlaced ? 1.0f : float(kTvPadRows);
	const float lines = gamepad ? float(tv_rows_lines(H)) * (kTvPadRows / pad_rows) : field * (interlaced ? 2.0f : 1.0f);
	const GLfloat size[4] = {float(w), float(H), lines, gamepad ? 1.0f / pad_rows : 0.0f};
	// The uniform goes in through the GL layer, which keeps it in the
	// program's register image; GLideN64's own program is current again after.
	gx2::Program * was = gx2::currentProgram();
	for (int prog = first; prog <= last; prog++) {
		TvProgram & t = g_programs[prog];
		glUseProgram(t.name);
		glUniform4fv(t.size, 1, size);
		t.gx2 = gx2::currentProgram();
	}

	// The frame read as a texture: the same memory, layout and swizzle. All
	// frames that reach here were made with GX2_SURFACE_USE_TEXTURE
	// (GLideN64's present buffer, the queue's pictures, the notices' copy).
	// The frame is read, and the TV's buffer written, through sRGB views of
	// the same RGBA8 memory (the same layout, only the number type differs):
	// the texture unit turns the frame's values into light before it filters,
	// the color buffer turns the shader's light back into values, and the
	// shaders mix light without an instruction for it (tv_shaders.h).
	GX2Texture tex;
	memset(&tex, 0, sizeof(tex));
	tex.surface = frame->surface;
	if (tex.surface.format == GX2_SURFACE_FORMAT_UNORM_R8_G8_B8_A8)
		tex.surface.format = GX2_SURFACE_FORMAT_SRGB_R8_G8_B8_A8;
	tex.viewFirstMip = 0;
	tex.viewNumMips = 1;
	tex.viewFirstSlice = 0;
	tex.viewNumSlices = 1;
	tex.compMap = GX2_COMP_MAP(GX2_SQ_SEL_R, GX2_SQ_SEL_G, GX2_SQ_SEL_B, GX2_SQ_SEL_A);
	GX2InitTextureRegs(&tex);

	// The frame was just drawn or copied by the GPU as a color buffer, in
	// this same command stream: its last pixels may still sit in the color
	// buffer cache, and the texture cache may hold the frame before. The GX2
	// backend does the same before it samples its own frame buffers
	// (finishRenderTarget). The commands run in order, so no wait is needed.
	GX2Invalidate((GX2InvalidateMode)(GX2_INVALIDATE_MODE_COLOR_BUFFER | GX2_INVALIDATE_MODE_TEXTURE),
	              frame->surface.image, frame->surface.imageSize);

	const bool timed = !timing.armed && timing.logs < kTimeLogs;
	if (timed) {
		memset(timing.stamps, 0, 64);
		DCFlushRange(timing.stamps, 64);
		GX2SampleBottomGPUCycle(&timing.stamps[0]);
		DCFlushRange(timing.stamps, 64); // what GX2 put there for "no value" is in memory before the GPU writes
	}
	GX2SetContextState(g_ctx);
	if (clear) { // black around the picture: drawn once, the pass writes only the picture's rect
		GX2ClearColor(out, 0.0f, 0.0f, 0.0f, 1.0f);
		GX2SetContextState(g_ctx);
	}
	GX2ColorBuffer light = *out;
	if (light.surface.format == GX2_SURFACE_FORMAT_UNORM_R8_G8_B8_A8) {
		light.surface.format = GX2_SURFACE_FORMAT_SRGB_R8_G8_B8_A8;
		GX2InitColorBufferRegs(&light);
	}
	GX2SetColorBuffer(&light, GX2_RENDER_TARGET_0);
	GX2SetViewport(float(x), 0.0f, float(w), float(H), 0.0f, 1.0f);
	GX2SetScissor(x, 0, w, H);
	GX2SetDepthOnlyControl(FALSE, FALSE, GX2_COMPARE_FUNC_ALWAYS);
	GX2SetColorControl(GX2_LOGIC_OP_COPY, 0, FALSE, TRUE); // no blending
	GX2SetCullOnlyControl(GX2_FRONT_FACE_CCW, FALSE, FALSE);
	GX2SetTargetChannelMasks(GX2_CHANNEL_MASK_RGBA, (GX2ChannelMask)0, (GX2ChannelMask)0, (GX2ChannelMask)0,
	                         (GX2ChannelMask)0, (GX2ChannelMask)0, (GX2ChannelMask)0, (GX2ChannelMask)0);
	bind(g_programs[first].gx2, &tex);
	if (curved) {
		GX2SetAttribBuffer(0, mesh.info.vertices * sizeof(TvMeshVertex), sizeof(TvMeshVertex), mesh.v);
		if (mesh.info.inner != 0)
			GX2DrawIndexedEx(GX2_PRIMITIVE_MODE_TRIANGLES, mesh.info.inner, GX2_INDEX_TYPE_U16, mesh.index, 0, 1);
		bind(g_programs[last].gx2, &tex);
		GX2DrawIndexedEx(GX2_PRIMITIVE_MODE_TRIANGLES, mesh.info.edge, GX2_INDEX_TYPE_U16,
		                 mesh.index + mesh.info.inner, 0, 1); // never empty: the outer cells reach the edge
		mesh.drawn = true;
	} else {
		GX2SetAttribBuffer(0, 4 * sizeof(gx2::RectVertex), sizeof(gx2::RectVertex), g_quad + (gamepad ? 4 : 0));
		GX2DrawEx(GX2_PRIMITIVE_MODE_TRIANGLE_STRIP, 4, 0, 1);
	}
	// Out of the color buffer cache before the copy to the TV's scan buffer reads it.
	GX2Invalidate(GX2_INVALIDATE_MODE_COLOR_BUFFER, out->surface.image, out->surface.imageSize);
	if (timed) {
		GX2SampleBottomGPUCycle(&timing.stamps[1]);
		DCFlushRange(timing.stamps, 64); // never written back over the GPU's samples later
		timing.armed = true;
		timing.armed_frames = 0;
	}
	glUseProgram(was != nullptr ? was->name : 0);
	return true;
}
