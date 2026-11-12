// Our64 player: Settings > Picture's TV-only shaders, GLSL that CafeGLSL
// compiles on the console (tv_pass.cpp). Our own code.
//
// Written for CafeGLSL: texture() through a linear sampler, no texelFetch,
// and no branches around texture reads (inside Cemu, CafeGLSL hangs on a
// texelFetch inside an if). The one uniform is uSize: the picture's size on
// the TV (its 4:3 or 16:9 rect), the game's lines (240 NTSC, 288 PAL, twice
// that in an interlaced 480-line mode); its w is unused (CRT Curved's
// curvature is in its grid, tv_mesh.h).
//
// They work in linear light without spending an instruction on it:
// tv_pass.cpp reads the frame through an sRGB view (the texture unit turns
// its values into light before it blends them) and writes the TV's buffer
// through one (the color buffer turns light back into values), so the lines
// mix light as a tube does instead of darkening the picture. The views are
// the same RGBA8 memory with the same channel order (GX2 format 0x41A against
// 0x01A: only the number type differs), so they cannot move a hue; on flat
// colors Off and Scanlines average to the same light (tools/crt-preview).
//
// No phosphor mask (2026-11-04): an RGB aperture grille, 3 TV pixels a
// stripe group, drawn at the TV's own pixels showed color shifts on Zach's
// 1080p TV (a TV that scales or overscans the picture beats against any
// fine pattern, and processing in 4:2:2 smears colored stripes into hue
// shifts), and its brightness make-up clipped bright colors channel by
// channel (230,200,90 drifted toward green). CRT reads as a tube by its
// beams, their sideways softness and the glow.
//
// Cost: on the console's GPU (an R700-class Latte) a pixel shader is a
// series of ALU groups, up to 5 operations each, and every group costs the
// same, full or not, so the time is the number of groups: the longest chain
// of operations that wait on each other, or the operation count over 5.
// Measured on the console at 1080p (2026-11-04, GPU timestamps): Scanlines
// 1.19 ms a frame (11 groups, 2 texture reads), CRT 1.46 (14, 4), CRT Curved
// with its bend worked out per pixel 1.79 (20, 4): about 0.055 ms a group
// and 0.05 ms a read a pixel, over about 0.5 ms that does not depend on the
// shader. CafeGLSL's linux glslcompiler -v prints the groups
// (tools/crt-preview). Whatever depends only on the uniform, or changes
// linearly across the picture, is worked out in the vertex shader for the
// corners and reaches each pixel interpolated, for nothing:
// vU: the frame's u, and u half a beam's softness either side.
// vLine: the frame's v in game lines (minus half a line), v, v one line
//   further, and one line in v.
// vBeam: beam()'s constants (see there): a dark line's inverse half-width,
//   what full brightness adds to it, the glow, and CRT's light scale (the
//   bump's 15/16, less the glow's light; CRT Curved's falloff toward the
//   corners folded in).
// vZEdge: CRT Curved's edge (see kTvCurvedShader). CafeGLSL numbers a
// shader's varyings in name order and links the two shaders of a pair by
// that number, not by name: CRT Curved's inner cells run kTvCrtShader, which
// has no edge, so the edge's name must sort after vBeam, vLine and vU, or it
// takes vLine's slot (the middle of the picture drawn from the edge's values,
// 2026-11-05).
// CRT Curved's bend is in its geometry (tv_mesh.h): a grid whose vertices
// carry the bent texture coordinates, so its pixels run CRT's own code and,
// in the cells along the edges, the edge term (2026-11-04; the bend per
// pixel had cost 6 groups more than CRT).
#pragma once

// The vertex shaders: a quad of the GX2 backend's RectVertex layout
// (aRectPosition, aTexCoord0), whose fetch shader the backend builds from
// these names. The backend negates gl_Position.y in every vertex shader;
// this negates it first, so y = 1 is the top of the TV and the frame's first
// row (v = 0) goes there. Scanlines and CRT share kTvVertexShader, CRT
// Curved has its own (its grid's vertices carry more).
// TV_SOFT: half the distance between CRT's two reads across a line, in u
// (0.0007 since 2026-11-12: at 0.00125 an edge was 5.2 TV pixels wide at 1080p,
// softer than RetroArch's crt-lottes 3.9 and crt-geom 3.2; now 3.6):
// 0.22 of a game pixel when the game is 320 pixels wide (most are; a 640-wide
// one gets the same softness on the glass, 0.45 of its pixels).
#define TV_VERTEX_COMMON \
	"#version 330\n" \
	"uniform vec4 uSize;\n" \
	"in vec4 aRectPosition;\n" \
	"in vec2 aTexCoord0;\n" \
	"out vec4 vBeam;\n" \
	"void corner()\n" \
	"{\n" \
	"  gl_Position = vec4(aRectPosition.x, -aRectPosition.y, 0.0, 1.0);\n" \
	"  float hi = step(360.0, uSize.z);\n" \
	"  vBeam = vec4(mix(1.0 / 0.6, 1.0, hi), mix(1.0 / 0.85 - 1.0 / 0.6, 0.0, hi), 0.05, (15.0 / 16.0) / 1.1);\n" \
	"}\n"
#define TV_SOFT "0.0007"

static const char kTvVertexShader[] =
	TV_VERTEX_COMMON
	"out vec3 vU;\n"
	"out vec4 vLine;\n"
	"void main()\n"
	"{\n"
	"  corner();\n"
	"  vU = aTexCoord0.x + vec3(0.0, -" TV_SOFT ", " TV_SOFT ");\n"
	"  float line = 1.0 / uSize.z;\n"
	"  vLine = vec4(aTexCoord0.y * uSize.z - 0.5, aTexCoord0.y, aTexCoord0.y + line, line);\n"
	"}\n";

// CRT Curved's grid (tv_mesh.h): aTexCoord0 the bent uv, the rest as
// kTvVertexShader makes from it; aRectPosition.z the falloff toward the
// corners (into vBeam.w, the light scale), aTexCoord1 and aRectPosition.w
// the edge (vZEdge, see kTvCurvedShader). All of it is linear across a cell.
static const char kTvCurvedVertexShader[] =
	TV_VERTEX_COMMON
	"in vec2 aTexCoord1;\n"
	"out vec3 vU;\n"
	"out vec4 vLine;\n"
	"out vec4 vZEdge;\n"
	"void main()\n"
	"{\n"
	"  corner();\n"
	"  vBeam.w *= aRectPosition.z;\n"
	"  vU = aTexCoord0.x + vec3(0.0, -" TV_SOFT ", " TV_SOFT ");\n"
	"  float line = 1.0 / uSize.z;\n"
	"  vLine = vec4(aTexCoord0.y * uSize.z - 0.5, aTexCoord0.y, aTexCoord0.y + line, line);\n"
	"  vZEdge = vec4(aTexCoord1, aRectPosition.w, 0.0);\n"
	"}\n";

// What the three share. beam(): the picture as a CRT's beams draw it. Each
// game line is one beam that follows the game's own lines (vLine), not the
// TV's rows: a pixel takes the two lines around it, a and b, f of a line
// below a's center, each read through the linear sampler at the line's
// center (the frame's two rows of that line, averaged, and its columns
// blended). A beam is a smooth bump, (1 - t^2)^2, as wide as the line is
// bright (its brightest channel): a dark line is thin (half-width 0.6 of a
// line, dark gaps between lines), a bright one wide (0.85, filling most of
// the gap), as an electron beam spreads with its current. Each bump is
// scaled to keep its light (narrow means taller: times its inverse width and
// 15/16), so the lines change the picture's texture, not its brightness. The
// same weight goes to the three channels: the beams shade the picture
// without tinting it. A game in an interlaced 480-line mode (uSize.z 480 or
// 576) has its lines twice as close as a tube's beams are wide: there every
// line has the width of two (half-width 1) and the gaps close, as a TV's two
// fields fill each other's.
// glow: CRT's (vBeam.z), none for Scanlines: it lifts both lines' weights
// (the halation of light spread in the tube's glass, which also fills the
// gaps); scale takes its light back out (vBeam.w: the 15/16 over 1 + 2 glow),
// so CRT is as bright as Off; CRT Curved's falloff and edge scale it too.
#define TV_PIXEL_COMMON \
	"#version 330\n" \
	"uniform sampler2D uTex0;\n" \
	"uniform vec4 uSize;\n" \
	"in vec4 vBeam;\n" \
	"out vec4 fragColor;\n" \
	"vec3 beam(vec3 a, vec3 b, float f, float glow, float scale)\n" \
	"{\n" \
	"  vec2 d = vec2(f, 1.0 - f);\n" \
	"  vec2 d0 = d * vBeam.x;\n" \
	"  vec2 d1 = d * vBeam.y;\n" \
	"  vec2 m = vec2(max(max(a.r, a.g), a.b), max(max(b.r, b.g), b.b));\n" \
	"  vec2 t = m * d1 + d0;\n" \
	"  vec2 i = (m * vBeam.y + vBeam.x) * scale;\n" \
	"  vec2 u = clamp(1.0 - t * t, 0.0, 1.0);\n" \
	"  vec2 w = u * (u * i) + glow * scale;\n" \
	"  return a * w.x + b * w.y;\n" \
	"}\n"

// Lines only, as sharp across as the frame.
static const char kTvScanlinesShader[] =
	TV_PIXEL_COMMON
	"in vec3 vU;\n"
	"in vec4 vLine;\n"
	"void main()\n"
	"{\n"
	"  float f = fract(vLine.x);\n"
	"  vec3 a = texture(uTex0, vec2(vU.x, vLine.y - f * vLine.w)).rgb;\n"
	"  vec3 b = texture(uTex0, vec2(vU.x, vLine.z - f * vLine.w)).rgb;\n"
	"  fragColor = vec4(beam(a, b, f, 0.0, 15.0 / 16.0), 1.0);\n"
	"}\n";

// The lines, softer across (each line read twice, TV_SOFT either side, as a
// beam's spot is round, not a pixel wide), and the glow.
static const char kTvCrtShader[] =
	TV_PIXEL_COMMON
	"in vec3 vU;\n"
	"in vec4 vLine;\n"
	"void main()\n"
	"{\n"
	"  float f = fract(vLine.x);\n"
	"  float ya = vLine.y - f * vLine.w;\n"
	"  float yb = vLine.z - f * vLine.w;\n"
	"  vec3 a = texture(uTex0, vec2(vU.y, ya)).rgb + texture(uTex0, vec2(vU.z, ya)).rgb;\n"
	"  vec3 b = texture(uTex0, vec2(vU.y, yb)).rgb + texture(uTex0, vec2(vU.z, yb)).rgb;\n"
	"  fragColor = vec4(beam(a * 0.5, b * 0.5, f, vBeam.z, vBeam.w), 1.0);\n"
	"}\n";

// CRT Curved's cells along the edges (tv_mesh.h; the others run kTvCrtShader
// with kTvCurvedVertexShader): CRT, and the edge. Past the bent picture's
// edges it is black: a rounded rectangle (the tube's corners, radius 0.08
// of the half height), faded over about a TV pixel so it is not jagged.
// vZEdge.xy: how far the bent point is past the rect the corners round off
// from, scaled so their squares' sum is R^2 - r^2 in TV pixels near the
// edge; vZEdge.z: where that fades out (see tv_mesh.h). The light scales the
// beams' weights (through beam()'s scale, as the falloff does).
static const char kTvCurvedShader[] =
	TV_PIXEL_COMMON
	"in vec3 vU;\n"
	"in vec4 vLine;\n"
	"in vec4 vZEdge;\n"
	"void main()\n"
	"{\n"
	"  float f = fract(vLine.x);\n"
	"  float ya = vLine.y - f * vLine.w;\n"
	"  float yb = vLine.z - f * vLine.w;\n"
	"  vec3 a = texture(uTex0, vec2(vU.y, ya)).rgb + texture(uTex0, vec2(vU.z, ya)).rgb;\n"
	"  vec3 b = texture(uTex0, vec2(vU.y, yb)).rgb + texture(uTex0, vec2(vU.z, yb)).rgb;\n"
	"  vec2 e = max(vZEdge.xy, 0.0);\n"
	"  float lit = clamp(vZEdge.z - e.x * e.x - e.y * e.y, 0.0, 1.0);\n"
	"  fragColor = vec4(beam(a * 0.5, b * 0.5, f, vBeam.z, vBeam.w * lit), 1.0);\n"
	"}\n";

#undef TV_PIXEL_COMMON
#undef TV_SOFT
#undef TV_VERTEX_COMMON
