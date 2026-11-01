#pragma once

// GLSL compiled on the console by CafeGLSL. Written from the RDP's documented
// combiner: (A - B) * C + D per cycle, color and alpha, with the hardware's mux
// codes. Nothing here comes from Nintendo's VC shaders.

static const char *kVertexShader = R"(#version 330
layout(location = 0) in vec4 aPos;
layout(location = 1) in vec4 aColor;
layout(location = 2) in vec2 aUV;
out vec4 vShade;
out vec2 vUV;
void main()
{
   gl_Position = aPos;
   vShade = aColor;
   vUV = aUV;
}
)";

static const char *kSimplePixelShader = R"(#version 330
uniform sampler2D uTex0;
in vec4 vShade;
in vec2 vUV;
out vec4 fragColor;
void main()
{
   fragColor = texture(uTex0, vUV) * vShade;
}
)";

// One shader for every combiner mode: the mux codes arrive as integer uniforms.
static const char *kUberPixelShader = R"(#version 330
uniform sampler2D uTex0;
uniform sampler2D uTex1;
uniform ivec4 uColor0;   // cycle 1 color mux: a, b, c, d
uniform ivec4 uAlpha0;   // cycle 1 alpha mux
uniform ivec4 uColor1;   // cycle 2 color mux
uniform ivec4 uAlpha1;   // cycle 2 alpha mux
uniform ivec4 uMode;     // x: cycles (1 or 2), y: 3-point filter, z: alpha compare, w: fog
uniform vec4 uPrim;
uniform vec4 uEnv;
uniform vec4 uKey;       // rgb: key centre, a: key scale
uniform vec4 uMisc;      // x: LOD fraction, y: prim LOD fraction, z: K4, w: K5
uniform vec4 uFog;       // rgb: fog color, a: alpha compare reference
uniform vec4 uTexSize;   // xy: tex0 size, zw: tex1 size
in vec4 vShade;
in vec2 vUV;
out vec4 fragColor;

vec4 comb;
vec4 t0;
vec4 t1;
float noiseV;

// The RDP's 3-point filter: three texels, split along the diagonal.
vec4 sample3(sampler2D s, vec2 uv, vec2 size)
{
   vec2 st = uv * size - 0.5;
   vec2 f = fract(st);
   vec2 i0 = floor(st);
   ivec2 a = ivec2(mod(i0, size));
   ivec2 b = ivec2(mod(i0 + vec2(1.0, 0.0), size));
   ivec2 c = ivec2(mod(i0 + vec2(0.0, 1.0), size));
   ivec2 d = ivec2(mod(i0 + vec2(1.0, 1.0), size));
   vec4 ta = texelFetch(s, a, 0);
   vec4 tb = texelFetch(s, b, 0);
   vec4 tc = texelFetch(s, c, 0);
   vec4 td = texelFetch(s, d, 0);
   if (f.x + f.y < 1.0)
      return ta + f.x * (tb - ta) + f.y * (tc - ta);
   return td + (1.0 - f.x) * (tc - td) + (1.0 - f.y) * (tb - td);
}

vec3 subA(int m)
{
   switch (m) {
   case 0: return comb.rgb;
   case 1: return t0.rgb;
   case 2: return t1.rgb;
   case 3: return uPrim.rgb;
   case 4: return vShade.rgb;
   case 5: return uEnv.rgb;
   case 6: return vec3(1.0);
   case 7: return vec3(noiseV);
   default: return vec3(0.0);
   }
}

vec3 subB(int m)
{
   switch (m) {
   case 0: return comb.rgb;
   case 1: return t0.rgb;
   case 2: return t1.rgb;
   case 3: return uPrim.rgb;
   case 4: return vShade.rgb;
   case 5: return uEnv.rgb;
   case 6: return uKey.rgb;
   case 7: return vec3(uMisc.z);
   default: return vec3(0.0);
   }
}

vec3 mulC(int m)
{
   switch (m) {
   case 0: return comb.rgb;
   case 1: return t0.rgb;
   case 2: return t1.rgb;
   case 3: return uPrim.rgb;
   case 4: return vShade.rgb;
   case 5: return uEnv.rgb;
   case 6: return vec3(uKey.a);
   case 7: return vec3(comb.a);
   case 8: return vec3(t0.a);
   case 9: return vec3(t1.a);
   case 10: return vec3(uPrim.a);
   case 11: return vec3(vShade.a);
   case 12: return vec3(uEnv.a);
   case 13: return vec3(uMisc.x);
   case 14: return vec3(uMisc.y);
   case 15: return vec3(uMisc.w);
   default: return vec3(0.0);
   }
}

vec3 addD(int m)
{
   switch (m) {
   case 0: return comb.rgb;
   case 1: return t0.rgb;
   case 2: return t1.rgb;
   case 3: return uPrim.rgb;
   case 4: return vShade.rgb;
   case 5: return uEnv.rgb;
   case 6: return vec3(1.0);
   default: return vec3(0.0);
   }
}

float alphaABD(int m)
{
   switch (m) {
   case 0: return comb.a;
   case 1: return t0.a;
   case 2: return t1.a;
   case 3: return uPrim.a;
   case 4: return vShade.a;
   case 5: return uEnv.a;
   case 6: return 1.0;
   default: return 0.0;
   }
}

float alphaC(int m)
{
   switch (m) {
   case 0: return uMisc.x;
   case 1: return t0.a;
   case 2: return t1.a;
   case 3: return uPrim.a;
   case 4: return vShade.a;
   case 5: return uEnv.a;
   case 6: return uMisc.y;
   default: return 0.0;
   }
}

vec4 cycle(ivec4 c, ivec4 a)
{
   vec3 rgb = (subA(c.x) - subB(c.y)) * mulC(c.z) + addD(c.w);
   float al = (alphaABD(a.x) - alphaABD(a.y)) * alphaC(a.z) + alphaABD(a.w);
   return clamp(vec4(rgb, al), 0.0, 1.0);
}

void main()
{
   noiseV = fract(sin(dot(gl_FragCoord.xy, vec2(12.9898, 78.233))) * 43758.5453);
   if (uMode.y != 0) {
      t0 = sample3(uTex0, vUV, uTexSize.xy);
      t1 = sample3(uTex1, vUV, uTexSize.zw);
   } else {
      t0 = texture(uTex0, vUV);
      t1 = texture(uTex1, vUV);
   }
   comb = vec4(0.0);
   comb = cycle(uColor0, uAlpha0);
   if (uMode.x == 2)
      comb = cycle(uColor1, uAlpha1);
   if (uMode.z != 0 && comb.a < uFog.a)
      discard;
   if (uMode.w != 0)
      comb.rgb = mix(comb.rgb, uFog.rgb, vShade.a);
   fragColor = comb;
}
)";

// Stand-in for one GLideN64-style generated shader: a fixed combiner plus the
// per-tile wrap/clamp/mirror, 3-point filtering, LOD, dithering, alpha compare,
// fog and N64 depth compare that such shaders carry. Only compiled, never drawn.
// VARIANT is replaced per compile so each one is new work. This is a printf
// format string: write a literal percent sign as %%.
static const char *kBigPixelShaderTemplate = R"(#version 330
#define VARIANT %d
uniform sampler2D uTex0;
uniform sampler2D uTex1;
uniform sampler2D uDepthTex;
uniform vec4 uPrim;
uniform vec4 uEnv;
uniform vec4 uFog;
uniform vec4 uTile0;     // xy: size, z: clamp s/t mask, w: mirror mask
uniform vec4 uTile1;
uniform vec4 uShift0;    // xy: shift scale, zw: offset
uniform vec4 uShift1;
uniform vec4 uLod;       // x: min level, y: prim LOD fraction, z: detail, w: sharpen
uniform vec4 uDepth;     // x: delta z, y: compare mode, z: polygon offset, w: scale
uniform vec4 uScreen;    // xy: framebuffer size, zw: scale
in vec4 vShade;
in vec2 vUV;
in vec2 vUV1;
out vec4 fragColor;

const float bayer[16] = float[16](0.0, 8.0, 2.0, 10.0, 12.0, 4.0, 14.0, 6.0,
                                  3.0, 11.0, 1.0, 9.0, 15.0, 7.0, 13.0, 5.0);

float wrapAxis(float c, float size, float clampOn, float mirrorOn)
{
   if (clampOn > 0.5)
      return clamp(c, 0.0, size - 1.0);
   float m = mod(c, 2.0 * size);
   if (mirrorOn > 0.5 && m >= size)
      return 2.0 * size - 1.0 - m;
   return mod(c, size);
}

ivec2 tileCoord(vec2 c, vec4 tile)
{
   float clampS = mod(tile.z, 2.0);
   float clampT = floor(tile.z / 2.0);
   float mirS = mod(tile.w, 2.0);
   float mirT = floor(tile.w / 2.0);
   return ivec2(wrapAxis(c.x, tile.x, clampS, mirS), wrapAxis(c.y, tile.y, clampT, mirT));
}

vec4 filter3(sampler2D s, vec2 uv, vec4 tile, vec4 shift)
{
   vec2 st = (uv * shift.xy + shift.zw) * tile.xy - 0.5;
   vec2 f = fract(st);
   vec2 b = floor(st);
   vec4 ta = texelFetch(s, tileCoord(b, tile), 0);
   vec4 tb = texelFetch(s, tileCoord(b + vec2(1.0, 0.0), tile), 0);
   vec4 tc = texelFetch(s, tileCoord(b + vec2(0.0, 1.0), tile), 0);
   vec4 td = texelFetch(s, tileCoord(b + vec2(1.0, 1.0), tile), 0);
   if (f.x + f.y < 1.0)
      return ta + f.x * (tb - ta) + f.y * (tc - ta);
   return td + (1.0 - f.x) * (tc - td) + (1.0 - f.y) * (tb - td);
}

float lodFraction(vec2 uv, vec2 size)
{
   vec2 dx = dFdx(uv * size);
   vec2 dy = dFdy(uv * size);
   float lod = max(length(dx), length(dy));
   float level = max(log2(max(lod, 1.0)), uLod.x);
   return fract(level);
}

vec3 dither(vec3 c)
{
   ivec2 p = ivec2(mod(gl_FragCoord.xy, 4.0));
   float d = (bayer[p.y * 4 + p.x] / 16.0 - 0.5) / 31.0;
   return floor((c + d) * 31.0 + 0.5) / 31.0;
}

bool depthCompare()
{
   vec2 uv = gl_FragCoord.xy / uScreen.xy;
   float stored = texture(uDepthTex, uv).r;
   float z = gl_FragCoord.z * uDepth.w + uDepth.z;
   if (uDepth.y < 0.5)
      return z <= stored + uDepth.x;
   if (uDepth.y < 1.5)
      return abs(z - stored) <= uDepth.x;
   return z < stored;
}

void main()
{
   if (!depthCompare())
      discard;
   vec4 t0 = filter3(uTex0, vUV, uTile0, uShift0);
   vec4 t1 = filter3(uTex1, vUV1, uTile1, uShift1);
   float lf = lodFraction(vUV, uTile0.xy);
   vec4 detail = mix(t0, t1, lf * uLod.z);
   vec3 c1 = (detail.rgb - uEnv.rgb) * vShade.rgb + uEnv.rgb * float(VARIANT %% 3) * 0.5;
   float a1 = (t0.a - uPrim.a) * vShade.a + uPrim.a;
   vec3 c2 = (c1 - uPrim.rgb) * uEnv.a + uPrim.rgb;
   float a2 = a1 * t1.a;
   vec4 comb = clamp(vec4(c2, a2), 0.0, 1.0);
   float noiseV = fract(sin(dot(gl_FragCoord.xy + float(VARIANT), vec2(12.9898, 78.233))) * 43758.5453);
   if (comb.a < max(uFog.a, noiseV * 0.01))
      discard;
   comb.rgb = mix(comb.rgb, uFog.rgb, vShade.a * uScreen.z);
   comb.rgb = dither(comb.rgb);
   comb.rgb = pow(comb.rgb, vec3(1.0 / 2.2));
   fragColor = comb;
}
)";

static const char *kBigVertexShader = R"(#version 330
layout(location = 0) in vec4 aPos;
layout(location = 1) in vec4 aColor;
layout(location = 2) in vec2 aUV;
out vec4 vShade;
out vec2 vUV;
out vec2 vUV1;
void main()
{
   gl_Position = aPos;
   vShade = aColor;
   vUV = aUV;
   vUV1 = aUV * 2.0;
}
)";
