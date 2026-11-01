#include "gpu_probe.h"
#include "report.h"
#include "shaders.h"

#include <cafeglsl/CafeGLSLCompiler.h>
#include <coreinit/cache.h>
#include <coreinit/memdefaultheap.h>
#include <gx2/clear.h>
#include <gx2/context.h>
#include <gx2/draw.h>
#include <gx2/event.h>
#include <gx2/mem.h>
#include <gx2/registers.h>
#include <gx2/sampler.h>
#include <gx2/shaders.h>
#include <gx2/state.h>
#include <gx2/surface.h>
#include <gx2/texture.h>
#include <gx2/utils.h>
#include <gx2r/buffer.h>
#include <gx2r/draw.h>
#include <whb/gfx.h>

#include <stdint.h>
#include <stdio.h>
#include <string.h>

namespace {

constexpr int kTris = 5000;           // a busy N64 frame
constexpr int kBatches = 50;          // state changes per frame
constexpr int kTrisPerBatch = kTris / kBatches;
constexpr int kFrames = 30;
constexpr float kTriSize = 0.07f;     // about 3x overdraw over the screen
constexpr uint32_t kVertexStride = 40; // vec4 pos, vec4 color, vec2 uv

char g_log[4096];

struct Program {
   GX2VertexShader *vs = nullptr;
   GX2PixelShader *ps = nullptr;
   WHBGfxShaderGroup group = {};
   GX2ShaderMode mode = GX2_SHADER_MODE_UNIFORM_REGISTER;
   bool ok = false;
};

struct Target {
   GX2ColorBuffer cb = {};
   GX2DepthBuffer db = {};
   uint32_t w = 0, h = 0;
};

GX2ContextState *g_ctx = nullptr;
GX2RBuffer g_vbo = {};
GX2Texture g_tex[2] = {};
GX2Sampler g_sampler = {};

uint32_t g_rng = 0x12345678;
uint32_t Rand()
{
   g_rng ^= g_rng << 13;
   g_rng ^= g_rng >> 17;
   g_rng ^= g_rng << 5;
   return g_rng;
}
float RandF() { return (Rand() & 0xFFFFFF) / 16777216.0f; }

void *Alloc(uint32_t size, uint32_t align)
{
   return MEMAllocFromDefaultHeapEx(size, align);
}

// Shows a solid color on TV and GamePad so the person watching knows which phase is running.
void ShowPhase(float r, float g, float b)
{
   WHBGfxBeginRender();
   WHBGfxBeginRenderTV();
   WHBGfxClearColor(r, g, b, 1.0f);
   WHBGfxFinishRenderTV();
   WHBGfxBeginRenderDRC();
   WHBGfxClearColor(r, g, b, 1.0f);
   WHBGfxFinishRenderDRC();
   WHBGfxFinishRender();
}

bool Compile(Program &p, const char *vsSrc, const char *psSrc, double *ms)
{
   double t0 = Report_NowUs();
   p.mode = CompileShaderPair(vsSrc, psSrc, GLSL_COMPILE_REGISTER, &p.vs, &p.ps, g_log, sizeof(g_log),
                              GLSL_COMPILER_FLAG_NONE);
   if (ms)
      *ms = (Report_NowUs() - t0) / 1000.0;
   if (p.mode == GLSL_SHADER_MODE_ERROR) {
      Report_Line("    compile error: %.300s", g_log);
      return false;
   }
   GX2Invalidate(GX2_INVALIDATE_MODE_CPU_SHADER, p.vs->program, p.vs->size);
   GX2Invalidate(GX2_INVALIDATE_MODE_CPU_SHADER, p.ps->program, p.ps->size);
   return true;
}

bool Link(Program &p)
{
   p.group.vertexShader = p.vs;
   p.group.pixelShader = p.ps;
   WHBGfxInitShaderAttribute(&p.group, "aPos", 0, 0, GX2_ATTRIB_FORMAT_FLOAT_32_32_32_32);
   WHBGfxInitShaderAttribute(&p.group, "aColor", 0, 16, GX2_ATTRIB_FORMAT_FLOAT_32_32_32_32);
   WHBGfxInitShaderAttribute(&p.group, "aUV", 0, 32, GX2_ATTRIB_FORMAT_FLOAT_32_32);
   p.ok = WHBGfxInitFetchShader(&p.group);
   return p.ok;
}

bool MakeGeometry()
{
   g_vbo.flags = (GX2RResourceFlags)(GX2R_RESOURCE_BIND_VERTEX_BUFFER | GX2R_RESOURCE_USAGE_CPU_WRITE |
                                     GX2R_RESOURCE_USAGE_GPU_READ);
   g_vbo.elemSize = kVertexStride;
   g_vbo.elemCount = kTris * 3;
   if (!GX2RCreateBuffer(&g_vbo))
      return false;
   float *v = (float *)GX2RLockBufferEx(&g_vbo, GX2R_RESOURCE_BIND_NONE);
   for (int t = 0; t < kTris; t++) {
      float cx = RandF() * 2.0f - 1.0f, cy = RandF() * 2.0f - 1.0f, z = 0.1f + RandF() * 0.8f;
      const float off[3][2] = {{0, 0}, {kTriSize, 0}, {0, kTriSize}};
      for (int k = 0; k < 3; k++) {
         float *p = v + (t * 3 + k) * (kVertexStride / 4);
         p[0] = cx + off[k][0]; p[1] = cy + off[k][1]; p[2] = z; p[3] = 1.0f;
         p[4] = RandF(); p[5] = RandF(); p[6] = RandF(); p[7] = RandF();
         p[8] = off[k][0] * 20.0f; p[9] = off[k][1] * 20.0f;
      }
   }
   GX2RUnlockBufferEx(&g_vbo, GX2R_RESOURCE_BIND_NONE);
   return true;
}

bool MakeTexture(GX2Texture &tex, uint32_t w, uint32_t h)
{
   tex.surface.dim = GX2_SURFACE_DIM_TEXTURE_2D;
   tex.surface.width = w;
   tex.surface.height = h;
   tex.surface.depth = 1;
   tex.surface.mipLevels = 1;
   tex.surface.format = GX2_SURFACE_FORMAT_UNORM_R8_G8_B8_A8;
   tex.surface.aa = GX2_AA_MODE1X;
   tex.surface.use = GX2_SURFACE_USE_TEXTURE;
   tex.surface.tileMode = GX2_TILE_MODE_DEFAULT;
   tex.viewNumMips = 1;
   tex.viewNumSlices = 1;
   tex.compMap = GX2_COMP_MAP(0, 1, 2, 3);
   GX2CalcSurfaceSizeAndAlignment(&tex.surface);
   GX2InitTextureRegs(&tex);
   tex.surface.image = Alloc(tex.surface.imageSize, tex.surface.alignment);
   if (!tex.surface.image)
      return false;
   uint32_t *px = (uint32_t *)tex.surface.image;
   for (uint32_t i = 0; i < tex.surface.imageSize / 4; i++)
      px[i] = Rand() | 0x000000FF;
   GX2Invalidate(GX2_INVALIDATE_MODE_CPU_TEXTURE, tex.surface.image, tex.surface.imageSize);
   return true;
}

bool MakeTarget(Target &t, uint32_t w, uint32_t h)
{
   t.w = w;
   t.h = h;
   GX2Surface &cs = t.cb.surface;
   cs.dim = GX2_SURFACE_DIM_TEXTURE_2D;
   cs.width = w;
   cs.height = h;
   cs.depth = 1;
   cs.mipLevels = 1;
   cs.format = GX2_SURFACE_FORMAT_UNORM_R8_G8_B8_A8;
   cs.aa = GX2_AA_MODE1X;
   cs.use = (GX2SurfaceUse)(GX2_SURFACE_USE_TEXTURE | GX2_SURFACE_USE_COLOR_BUFFER);
   cs.tileMode = GX2_TILE_MODE_DEFAULT;
   t.cb.viewNumSlices = 1;
   GX2CalcSurfaceSizeAndAlignment(&cs);
   GX2InitColorBufferRegs(&t.cb);
   cs.image = Alloc(cs.imageSize, cs.alignment);

   GX2Surface &ds = t.db.surface;
   ds.dim = GX2_SURFACE_DIM_TEXTURE_2D;
   ds.width = w;
   ds.height = h;
   ds.depth = 1;
   ds.mipLevels = 1;
   ds.format = GX2_SURFACE_FORMAT_FLOAT_R32;
   ds.aa = GX2_AA_MODE1X;
   ds.use = GX2_SURFACE_USE_DEPTH_BUFFER;
   ds.tileMode = GX2_TILE_MODE_DEFAULT;
   t.db.viewNumSlices = 1;
   t.db.depthClear = 1.0f;
   GX2CalcSurfaceSizeAndAlignment(&ds);
   GX2InitDepthBufferRegs(&t.db);
   ds.image = Alloc(ds.imageSize, ds.alignment);

   if (!cs.image || !ds.image)
      return false;
   GX2Invalidate(GX2_INVALIDATE_MODE_CPU, cs.image, cs.imageSize);
   GX2Invalidate(GX2_INVALIDATE_MODE_CPU, ds.image, ds.imageSize);
   return true;
}

void FreeTarget(Target &t)
{
   if (t.cb.surface.image)
      MEMFreeToDefaultHeap(t.cb.surface.image);
   if (t.db.surface.image)
      MEMFreeToDefaultHeap(t.db.surface.image);
   t = Target{};
}

void SetUniform(const Program &p, const char *name, const void *data)
{
   GX2UniformVar *var = GX2GetPixelUniformVar(p.ps, name);
   if (var)
      GX2SetPixelUniformReg(var->offset, 4, data);
}

void BindTexture(const Program &p, const char *name, const GX2Texture &tex)
{
   for (uint32_t i = 0; i < p.ps->samplerVarCount; i++) {
      const GX2SamplerVar &var = p.ps->samplerVars[i];
      if (strcmp(var.name, name) == 0) {
         GX2SetPixelTexture(&tex, var.location);
         GX2SetPixelSampler(&g_sampler, var.location);
         return;
      }
   }
}

// Eight common N64 combiner setups: color mux, alpha mux (cycle 1, then cycle 2), mode.
struct CombinerMode {
   int32_t c0[4], a0[4], c1[4], a1[4], mode[4];
};
const CombinerMode kModes[8] = {
   {{1, 15, 4, 7}, {7, 7, 7, 1}, {0, 0, 0, 0}, {0, 0, 0, 0}, {1, 0, 0, 0}},    // texel0 * shade
   {{1, 15, 4, 7}, {1, 7, 4, 7}, {0, 0, 0, 0}, {0, 0, 0, 0}, {1, 0, 0, 0}},    // modulate RGBA
   {{15, 15, 31, 1}, {7, 7, 7, 1}, {0, 0, 0, 0}, {0, 0, 0, 0}, {1, 0, 0, 0}},  // decal
   {{2, 1, 13, 1}, {2, 1, 0, 1}, {0, 15, 4, 7}, {0, 7, 4, 7}, {2, 0, 0, 0}},   // trilinear, then shade
   {{15, 15, 31, 3}, {7, 7, 7, 3}, {0, 0, 0, 0}, {0, 0, 0, 0}, {1, 0, 0, 0}},  // primitive color
   {{3, 5, 1, 5}, {7, 7, 7, 1}, {0, 0, 0, 0}, {0, 0, 0, 0}, {1, 1, 0, 0}},     // env blend, 3-point
   {{1, 15, 4, 7}, {1, 7, 4, 7}, {0, 0, 0, 0}, {0, 0, 0, 0}, {1, 0, 1, 1}},    // alpha compare + fog
   {{1, 15, 4, 7}, {1, 7, 4, 7}, {0, 15, 4, 7}, {0, 7, 4, 7}, {2, 1, 1, 1}},   // everything on
};

void BeginTarget(const Program &p, Target &t)
{
   GX2ClearColor(&t.cb, 0.1f, 0.0f, 0.0f, 1.0f);
   GX2ClearDepthStencilEx(&t.db, 1.0f, 0, GX2_CLEAR_FLAGS_DEPTH);
   GX2SetContextState(g_ctx);   // clears overwrite state
   GX2SetColorBuffer(&t.cb, GX2_RENDER_TARGET_0);
   GX2SetDepthBuffer(&t.db);
   GX2SetViewport(0, 0, (float)t.w, (float)t.h, 0.0f, 1.0f);
   GX2SetScissor(0, 0, t.w, t.h);
   GX2SetDepthOnlyControl(TRUE, TRUE, GX2_COMPARE_FUNC_LESS);
   if (!p.ok)
      return;
   GX2SetShaderMode(p.mode);
   GX2SetFetchShader(&p.group.fetchShader);
   GX2SetVertexShader(p.vs);
   GX2SetPixelShader(p.ps);
   GX2RSetAttributeBuffer(&g_vbo, 0, kVertexStride, 0);
   BindTexture(p, "uTex0", g_tex[0]);
   BindTexture(p, "uTex1", g_tex[1]);
}

void DrawFrame(const Program &p, Target &t, bool uber)
{
   BeginTarget(p, t);
   if (!p.ok)
      return;
   const float prim[4] = {0.8f, 0.6f, 0.4f, 0.9f}, env[4] = {0.2f, 0.3f, 0.9f, 0.5f};
   const float key[4] = {0.5f, 0.5f, 0.5f, 1.0f}, misc[4] = {0.3f, 0.6f, 0.1f, 0.2f};
   const float fog[4] = {0.7f, 0.7f, 0.8f, 0.05f}, size[4] = {64.0f, 32.0f, 64.0f, 32.0f};
   if (uber) {
      SetUniform(p, "uPrim", prim);
      SetUniform(p, "uEnv", env);
      SetUniform(p, "uKey", key);
      SetUniform(p, "uMisc", misc);
      SetUniform(p, "uFog", fog);
      SetUniform(p, "uTexSize", size);
   }
   for (int b = 0; b < kBatches; b++) {
      if (uber) {
         const CombinerMode &m = kModes[b % 8];
         SetUniform(p, "uColor0", m.c0);
         SetUniform(p, "uAlpha0", m.a0);
         SetUniform(p, "uColor1", m.c1);
         SetUniform(p, "uAlpha1", m.a1);
         SetUniform(p, "uMode", m.mode);
      }
      GX2DrawEx(GX2_PRIMITIVE_MODE_TRIANGLES, kTrisPerBatch * 3, b * kTrisPerBatch * 3, 1);
   }
}

// Milliseconds per frame, GPU work included: the timer stops after GX2DrawDone.
double TimeFrames(const Program &p, Target &t, bool uber)
{
   for (int i = 0; i < 3; i++)
      DrawFrame(p, t, uber);
   GX2DrawDone();
   double t0 = Report_NowUs();
   for (int i = 0; i < kFrames; i++) {
      DrawFrame(p, t, uber);
      GX2Flush();
   }
   GX2DrawDone();
   return (Report_NowUs() - t0) / 1000.0 / kFrames;
}

void TestCompile(Program &simple, Program &uber)
{
   Report_Line("[d] CafeGLSL %s", GetGLSLCompilerVersion());
   double ms = 0;
   if (Compile(simple, kVertexShader, kSimplePixelShader, &ms))
      Report_Line("[d] simple shader pair: %.1f ms", ms);
   if (Compile(uber, kVertexShader, kUberPixelShader, &ms))
      Report_Line("[d] uber-shader pair: %.1f ms", ms);

   static char src[16384];
   double total = 0, worst = 0;
   int done = 0;
   for (int v = 0; v < 5; v++) {
      snprintf(src, sizeof(src), kBigPixelShaderTemplate, v);
      Program big;
      if (!Compile(big, kBigVertexShader, src, &ms))
         break;
      FreeShaders(big.vs, big.ps);
      total += ms;
      worst = ms > worst ? ms : worst;
      done++;
   }
   if (done) {
      double avg = total / done;
      Report_Line("[d] GLideN64-sized shader: avg %.1f ms, worst %.1f ms over %d compiles", avg, worst, done);
      Report_Line("[d] verdict: %s (threshold 50 ms)",
                  avg < 50.0 ? "runtime compile usable, with an SD cache" : "uber-shader only, no runtime compile");
   } else {
      Report_Line("[d] verdict: GLideN64-sized shader did not compile; see error above");
   }
}

void TestDraw(Program &simple, Program &uber)
{
   const uint32_t res[3][2] = {{640, 480}, {1280, 720}, {1920, 1080}};
   const char *best = "none";
   Program none;   // ok == false: clears only
   for (auto &r : res) {
      Target t;
      if (!MakeTarget(t, r[0], r[1])) {
         Report_Line("[c] %ux%u: out of memory for target", r[0], r[1]);
         continue;
      }
      double clearMs = TimeFrames(none, t, false);
      double simpleMs = simple.ok ? TimeFrames(simple, t, false) : -1;
      double uberMs = uber.ok ? TimeFrames(uber, t, true) : -1;
      Report_Line("[c] %ux%u: clear %.2f ms, simple %.2f ms, uber %.2f ms per frame (%d tris, %d batches)",
                  r[0], r[1], clearMs, simpleMs, uberMs, kTris, kBatches);
      if (uberMs > 0 && uberMs < 16.7) {
         static char label[32];
         snprintf(label, sizeof(label), "%ux%u", r[0], r[1]);
         best = label;
      }
      FreeTarget(t);
   }
   Report_Line("[c] verdict: highest resolution with the uber-shader under 16.7 ms: %s", best);
}

void TestReadback(Program &uber)
{
   const uint32_t res[2][2] = {{320, 240}, {640, 480}};
   for (auto &r : res) {
      Target t;
      if (!MakeTarget(t, r[0], r[1]))
         continue;
      GX2Surface lin = {};
      lin.dim = GX2_SURFACE_DIM_TEXTURE_2D;
      lin.width = r[0];
      lin.height = r[1];
      lin.depth = 1;
      lin.mipLevels = 1;
      lin.format = GX2_SURFACE_FORMAT_UNORM_R8_G8_B8_A8;
      lin.aa = GX2_AA_MODE1X;
      lin.use = GX2_SURFACE_USE_TEXTURE;
      lin.tileMode = GX2_TILE_MODE_LINEAR_ALIGNED;
      GX2CalcSurfaceSizeAndAlignment(&lin);
      lin.image = Alloc(lin.imageSize, lin.alignment);
      static uint16_t rdram[640 * 480];

      double syncUs = 0, copyUs = 0, convUs = 0;
      uint32_t check = 0;
      const int N = 20;
      for (int i = 0; i < N; i++) {
         DrawFrame(uber, t, true);
         double t0 = Report_NowUs();
         GX2DrawDone();                       // the stall a mid-frame read forces
         double t1 = Report_NowUs();
         GX2CopySurface(&t.cb.surface, 0, 0, &lin, 0, 0);
         GX2DrawDone();
         DCInvalidateRange(lin.image, lin.imageSize);
         double t2 = Report_NowUs();
         // RGBA8 to the N64's RGBA5551, as a readback into RDRAM needs.
         const uint8_t *src = (const uint8_t *)lin.image;
         for (uint32_t y = 0; y < r[1]; y++) {
            const uint8_t *row = src + y * lin.pitch * 4;
            for (uint32_t x = 0; x < r[0]; x++) {
               const uint8_t *px = row + x * 4;
               rdram[y * r[0] + x] = (uint16_t)(((px[0] >> 3) << 11) | ((px[1] >> 3) << 6) | ((px[2] >> 3) << 1) |
                                                (px[3] >> 7));
            }
         }
         double t3 = Report_NowUs();
         check += rdram[(r[1] / 2) * r[0] + r[0] / 2];
         syncUs += t1 - t0;
         copyUs += t2 - t1;
         convUs += t3 - t2;
         GX2SetContextState(g_ctx);
      }
      Report_Line("[e] %ux%u: wait for GPU %.2f ms, copy to linear + read %.2f ms, convert to RGBA5551 %.2f ms "
                  "(check %u)",
                  r[0], r[1], syncUs / N / 1000.0, copyUs / N / 1000.0, convUs / N / 1000.0, (unsigned)(check & 0xFFFF));
      MEMFreeToDefaultHeap(lin.image);
      FreeTarget(t);
   }
   Report_Line("[e] verdict: per-readback cost is copy + convert; a game reading every frame pays it each frame");
}

}

void GpuProbe_Run()
{
   if (!WHBGfxInit()) {
      Report_Line("[c] FAIL: WHBGfxInit");
      return;
   }
   ShowPhase(0.6f, 0.0f, 0.0f);

   g_ctx = (GX2ContextState *)Alloc(sizeof(GX2ContextState), GX2_CONTEXT_STATE_ALIGNMENT);
   GX2SetupContextStateEx(g_ctx, TRUE);
   GX2InitSampler(&g_sampler, GX2_TEX_CLAMP_MODE_WRAP, GX2_TEX_XY_FILTER_MODE_LINEAR);

   double t0 = Report_NowUs();
   InitGLSLCompiler();
   Report_Line("[d] compiler init: %.1f ms", (Report_NowUs() - t0) / 1000.0);

   Program simple, uber;
   TestCompile(simple, uber);
   Report_Save();
   ShowPhase(0.8f, 0.6f, 0.0f);

   bool haveAssets = MakeGeometry() && MakeTexture(g_tex[0], 64, 32) && MakeTexture(g_tex[1], 64, 32);
   if (!haveAssets) {
      Report_Line("[c] FAIL: could not allocate geometry or textures");
   } else {
      if (simple.vs)
         Link(simple);
      if (uber.vs)
         Link(uber);
      TestDraw(simple, uber);
      Report_Save();
      ShowPhase(0.0f, 0.4f, 0.0f);
      if (uber.ok)
         TestReadback(uber);
      else
         Report_Line("[e] skipped: uber-shader did not compile");
   }
   Report_Save();

   DestroyGLSLCompiler();
   ShowPhase(0.0f, 0.0f, 0.0f);
   GX2DrawDone();
   WHBGfxShutdown();
}
