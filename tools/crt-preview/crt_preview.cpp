// Our64 crt-preview (PC): draws a 640x480 frame through the player's TV-only
// shaders (player/source/tv_shaders.h) as tv_pass.cpp does on the console:
// a buffer at the TV's size, black around a 4:3 or 16:9 picture, the same
// uSize uniform. Runs headless on an EGL surfaceless context (Mesa), so it
// needs no display.
//
// Usage: crt-preview <shader 0-3> <in.ppm> <out.ppm> <tv width> <tv height>
//                    <43 or 169> [lines] [rows]
//   rows: the GamePad's picture (854 480): the lines locked to the screen's
//         rows (tv_mesh.h's tv_rows_lines), not the game's
//   shader: 0 Off (plain bilinear stretch), 1 Scanlines, 2 CRT, 3 CRT Curved
//   lines: the game's line count as tv_pass.cpp sends it (default 240;
//          288 PAL, 480 or 576 for a game in an interlaced 480-line mode)
// tools/crt-preview/flat_check.py: Off against each shader on flat colors.
// CRT Curved is drawn as tv_pass.cpp draws it: tv_mesh.h's grid, its inner
// cells through CRT's pixel shader and its edge cells through CRT Curved's,
// both with CRT Curved's vertex shader.
// crt-preview --dump <dir>: writes the shaders' GLSL (tv.vert, curved.vert,
// scanlines.frag, crt.frag, curved.frag) for CafeGLSL's glslcompiler (CRT
// Curved is curved.vert with crt.frag, and with curved.frag).
// crt-preview --mesh <picture w> <picture h>: the grid against the bend
// worked out per pixel: the largest error in TV pixels, the share of the
// picture in edge cells and the 2x2 blocks shaded twice.
#include <EGL/egl.h>
#include <EGL/eglext.h>
#define GL_GLEXT_PROTOTYPES
#include <GL/gl.h>
#include <GL/glext.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "../../player/source/tv_mesh.h"
#include "../../player/source/tv_shaders.h"

namespace {

// A plain stretch, for "Off".
const char kPlainShader[] =
	"#version 330\n"
	"uniform sampler2D uTex0;\n"
	"uniform vec4 uSize;\n"
	"in vec3 vU;\n"
	"in vec4 vLine;\n"
	"out vec4 fragColor;\n"
	"void main() { fragColor = vec4(texture(uTex0, vec2(vU.x, vLine.y)).rgb, 1.0); }\n";

bool readPpm(const char * path, int & w, int & h, std::vector<unsigned char> & rgb)
{
	FILE * f = fopen(path, "rb");
	if (f == nullptr)
		return false;
	int maxv = 0;
	if (fscanf(f, "P6 %d %d %d", &w, &h, &maxv) != 3 || maxv != 255) {
		fclose(f);
		return false;
	}
	fgetc(f);
	rgb.resize(size_t(w) * h * 3);
	const bool ok = fread(rgb.data(), 1, rgb.size(), f) == rgb.size();
	fclose(f);
	return ok;
}

void writeFile(const std::string & path, const char * text)
{
	if (FILE * f = fopen(path.c_str(), "w")) {
		fputs(text, f);
		fclose(f);
	}
}

GLuint compile(GLenum type, const char * src)
{
	const GLuint s = glCreateShader(type);
	glShaderSource(s, 1, &src, nullptr);
	glCompileShader(s);
	GLint ok = 0;
	glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
	if (!ok) {
		char log[4096];
		glGetShaderInfoLog(s, sizeof(log), nullptr, log);
		fprintf(stderr, "compile failed:\n%s\n", log);
		exit(1);
	}
	return s;
}

GLuint link(const char * vs, const char * ps)
{
	const GLuint prog = glCreateProgram();
	glAttachShader(prog, compile(GL_VERTEX_SHADER, vs));
	glAttachShader(prog, compile(GL_FRAGMENT_SHADER, ps));
	glBindAttribLocation(prog, 0, "aRectPosition");
	glBindAttribLocation(prog, 1, "aTexCoord0");
	glBindAttribLocation(prog, 2, "aTexCoord1");
	glLinkProgram(prog);
	GLint linked = 0;
	glGetProgramiv(prog, GL_LINK_STATUS, &linked);
	if (!linked) {
		fprintf(stderr, "link failed\n");
		exit(1);
	}
	return prog;
}

// The grid's triangles rasterized on the CPU at the picture's pixel centers
// (edges counted on both sides; a pixel is measured once): the interpolated
// bent uv against tv_mesh_bend's, in TV pixels.
int meshCheck(unsigned w, unsigned h)
{
	std::vector<TvMeshVertex> v(kTvMeshMaxVertices);
	std::vector<uint16_t> idx(kTvMeshMaxIndices);
	const TvMesh m = tv_mesh_build(v.data(), idx.data(), w, h, kTvMeshBend);
	const double as = double(w) / h, k = tv_mesh_k(as, kTvMeshBend);
	std::vector<unsigned char> seen(size_t(w) * h);
	std::vector<int> owner(size_t(w / 2) * (h / 2), -1); // the last triangle that shaded each 2x2 block
	double worst = 0.0;
	size_t edge_pixels = 0, blocks = 0;
	for (unsigned t = 0; t < (m.inner + m.edge) / 3; t++) {
		const TvMeshVertex & a = v[idx[t * 3]], & b = v[idx[t * 3 + 1]], & c = v[idx[t * 3 + 2]];
		// in pixels, y down
		const double ax = (a.x + 1) * w / 2, ay = (1 - a.y) * h / 2, bx = (b.x + 1) * w / 2, by = (1 - b.y) * h / 2;
		const double cx = (c.x + 1) * w / 2, cy = (1 - c.y) * h / 2;
		const double den = (by - cy) * (ax - cx) + (cx - bx) * (ay - cy);
		const int x0 = int(std::min({ax, bx, cx})), x1 = int(std::max({ax, bx, cx}));
		const int y0 = int(std::min({ay, by, cy})), y1 = int(std::max({ay, by, cy}));
		for (int y = y0; y <= y1 && y < int(h); y++)
			for (int x = x0; x <= x1 && x < int(w); x++) {
				const double px = x + 0.5, py = y + 0.5;
				const double l0 = ((by - cy) * (px - cx) + (cx - bx) * (py - cy)) / den;
				const double l1 = ((cy - ay) * (px - cx) + (ax - cx) * (py - cy)) / den;
				const double l2 = 1.0 - l0 - l1;
				if (l0 < -1e-9 || l1 < -1e-9 || l2 < -1e-9)
					continue;
				const size_t q = size_t(y / 2) * (w / 2) + x / 2;
				if (owner[q] != int(t)) {
					owner[q] = int(t);
					blocks++;
				}
				if (seen[size_t(y) * w + x])
					continue;
				seen[size_t(y) * w + x] = 1;
				if (t * 3 >= m.inner)
					edge_pixels++;
				double gx, gy;
				tv_mesh_bend(px / w * 2 - 1, py / h * 2 - 1, as, k, gx, gy);
				const double s = l0 * a.s0 + l1 * b.s0 + l2 * c.s0, tt = l0 * a.t0 + l1 * b.t0 + l2 * c.t0;
				const double dx = (s - tv_overscan(gx * 0.5 + 0.5)) * w, dy = (tt - tv_overscan(gy * 0.5 + 0.5)) * h;
				worst = std::max(worst, std::sqrt(dx * dx + dy * dy));
			}
	}
	printf("%ux%u picture: %ux%u cells, largest error %.3f TV pixels, %.1f%% of the picture in edge cells, "
	       "%.2f%% more 2x2 blocks shaded\n", w, h, m.columns, kTvMeshRows, worst, 100.0 * edge_pixels / (double(w) * h),
	       100.0 * (double(blocks) / (double(w / 2) * (h / 2)) - 1.0));
	return 0;
}

} // namespace

int main(int argc, char ** argv)
{
	if (argc == 3 && strcmp(argv[1], "--dump") == 0) {
		const std::string d = argv[2];
		writeFile(d + "/tv.vert", kTvVertexShader);
		writeFile(d + "/curved.vert", kTvCurvedVertexShader);
		writeFile(d + "/curved-rows.vert", kTvCurvedRowsVertexShader);
		writeFile(d + "/scanlines.frag", kTvScanlinesShader);
		writeFile(d + "/crt.frag", kTvCrtShader);
		writeFile(d + "/curved.frag", kTvCurvedShader);
		return 0;
	}
	if (argc == 4 && strcmp(argv[1], "--mesh") == 0)
		return meshCheck(unsigned(atoi(argv[2])), unsigned(atoi(argv[3])));
	if (argc < 7) {
		fprintf(stderr, "usage: crt-preview <shader 0-3> <in.ppm> <out.ppm> <tv w> <tv h> <43|169> [lines]\n");
		return 2;
	}
	const int shader = atoi(argv[1]);
	const int W = atoi(argv[4]), H = atoi(argv[5]);
	const bool picture_43 = strcmp(argv[6], "43") == 0;
	float lines = argc > 7 ? float(atof(argv[7])) : 240.0f;
	// "rows": as tv_pass.cpp draws the GamePad's picture, the lines locked to the screen's rows (tv_mesh.h)
	const bool rows = argc > 8 && strcmp(argv[8], "rows") == 0;
	const bool interlaced = lines >= 360.0f;
	const double over_v = rows ? tv_rows_overscan(unsigned(H)) : double(kTvOverscan);
	if (rows)
		lines = float(tv_rows_lines(unsigned(H)) * (interlaced ? double(kTvPadRows) : 1.0));
	int fw, fh;
	std::vector<unsigned char> frame;
	if (!readPpm(argv[2], fw, fh, frame)) {
		fprintf(stderr, "cannot read %s (binary P6, maxval 255)\n", argv[2]);
		return 1;
	}

	EGLDisplay dpy = EGL_NO_DISPLAY;
	auto getPlatformDisplay = (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
	if (getPlatformDisplay != nullptr)
		dpy = getPlatformDisplay(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, nullptr);
	if (dpy == EGL_NO_DISPLAY || !eglInitialize(dpy, nullptr, nullptr)) {
		fprintf(stderr, "no EGL surfaceless display\n");
		return 1;
	}
	eglBindAPI(EGL_OPENGL_API);
	const EGLint ctxAttr[] = {EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_MINOR_VERSION, 3,
	                          EGL_CONTEXT_OPENGL_PROFILE_MASK, EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT, EGL_NONE};
	EGLContext ctx = eglCreateContext(dpy, EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT, ctxAttr);
	if (ctx == EGL_NO_CONTEXT || !eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx)) {
		fprintf(stderr, "no GL 3.3 core context\n");
		return 1;
	}

	const char * ps = shader == 1 ? kTvScanlinesShader : shader == 2 || shader == 3 ? kTvCrtShader : kPlainShader;
	const char * curved_vs = rows ? kTvCurvedRowsVertexShader : kTvCurvedVertexShader;
	const GLuint prog = link(shader == 3 ? curved_vs : kTvVertexShader, ps);
	const GLuint edge = shader == 3 ? link(curved_vs, kTvCurvedShader) : 0;

	// The frame: rows top first, as the console's (v = 0 is the top row).
	GLuint tex;
	glGenTextures(1, &tex);
	glBindTexture(GL_TEXTURE_2D, tex);
	glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
	// tv_pass.cpp reads the frame and writes the TV's buffer through sRGB
	// views (light in the shader); "Off" is the plain frame.
	const bool srgb = shader != 0;
	glTexImage2D(GL_TEXTURE_2D, 0, srgb ? GL_SRGB8_ALPHA8 : GL_RGBA8, fw, fh, 0, GL_RGB, GL_UNSIGNED_BYTE, frame.data());
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

	GLuint rb, fbo;
	glGenRenderbuffers(1, &rb);
	glBindRenderbuffer(GL_RENDERBUFFER, rb);
	glRenderbufferStorage(GL_RENDERBUFFER, srgb ? GL_SRGB8_ALPHA8 : GL_RGBA8, W, H);
	glGenFramebuffers(1, &fbo);
	glBindFramebuffer(GL_FRAMEBUFFER, fbo);
	glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, rb);

	// tv_pass.cpp's quad (RectVertex: x y z w, s t, unused).
	// Off shows the whole frame; the filters the frame less its overscan (tv_mesh.h).
	const float lo = shader == 0 ? 0.0f : tv_overscan(0.0), hi = shader == 0 ? 1.0f : tv_overscan(1.0);
	const float top = shader == 0 ? 0.0f : tv_overscan(0.0, over_v), bottom = shader == 0 ? 1.0f : tv_overscan(1.0, over_v);
	const float quad[4][8] = {
		{-1.0f, -1.0f, 0.0f, 1.0f, lo, bottom, 0.0f, 0.0f},
		{ 1.0f, -1.0f, 0.0f, 1.0f, hi, bottom, 0.0f, 0.0f},
		{-1.0f,  1.0f, 0.0f, 1.0f, lo, top, 0.0f, 0.0f},
		{ 1.0f,  1.0f, 0.0f, 1.0f, hi, top, 0.0f, 0.0f},
	};
	// tv_pass_draw's picture rect and uniform.
	int w = picture_43 ? ((H * 4 / 3 + 1) & ~1) : W;
	if (w > W)
		w = W;
	const int x = (W - w) / 2;

	// CRT Curved: tv_pass.cpp's grid for the picture.
	std::vector<TvMeshVertex> mesh(kTvMeshMaxVertices);
	std::vector<uint16_t> index(kTvMeshMaxIndices);
	const TvMesh m = tv_mesh_build(mesh.data(), index.data(), unsigned(w), unsigned(H), kTvMeshBend, over_v);
	GLuint vao, vbo, ibo;
	glGenVertexArrays(1, &vao);
	glBindVertexArray(vao);
	glGenBuffers(1, &vbo);
	glBindBuffer(GL_ARRAY_BUFFER, vbo);
	if (shader == 3)
		glBufferData(GL_ARRAY_BUFFER, m.vertices * sizeof(TvMeshVertex), mesh.data(), GL_STATIC_DRAW);
	else
		glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);
	glGenBuffers(1, &ibo);
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ibo);
	glBufferData(GL_ELEMENT_ARRAY_BUFFER, (m.inner + m.edge) * sizeof(uint16_t), index.data(), GL_STATIC_DRAW);
	glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, 32, nullptr);
	glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 32, reinterpret_cast<void *>(16));
	glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, 32, reinterpret_cast<void *>(24));
	glEnableVertexAttribArray(0);
	glEnableVertexAttribArray(1);
	glEnableVertexAttribArray(2);
	if (srgb)
		glEnable(GL_FRAMEBUFFER_SRGB);
	glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
	glClear(GL_COLOR_BUFFER_BIT);
	glViewport(x, 0, w, H);
	for (GLuint p : {prog, edge}) {
		if (p == 0)
			continue;
		glUseProgram(p);
		glUniform1i(glGetUniformLocation(p, "uTex0"), 0);
		const GLint loc = glGetUniformLocation(p, "uSize");
		if (loc >= 0)
			glUniform4f(loc, float(w), float(H), lines, rows ? (interlaced ? 1.0f : 1.0f / kTvPadRows) : 0.0f); // as tv_pass_draw sends it
	}
	if (shader == 3) {
		glUseProgram(prog);
		glDrawElements(GL_TRIANGLES, GLsizei(m.inner), GL_UNSIGNED_SHORT, nullptr);
		glUseProgram(edge);
		glDrawElements(GL_TRIANGLES, GLsizei(m.edge), GL_UNSIGNED_SHORT, reinterpret_cast<void *>(m.inner * sizeof(uint16_t)));
	} else {
		glUseProgram(prog);
		glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
	}

	// GL's row 0 is the bottom; the vertex shader puts v = 0 there (the
	// console's backend flips it to the top), so row 0 read first is the top.
	std::vector<unsigned char> out(size_t(W) * H * 3);
	glPixelStorei(GL_PACK_ALIGNMENT, 1);
	glReadPixels(0, 0, W, H, GL_RGB, GL_UNSIGNED_BYTE, out.data());
	FILE * f = fopen(argv[3], "wb");
	if (f == nullptr)
		return 1;
	fprintf(f, "P6\n%d %d\n255\n", W, H);
	fwrite(out.data(), 1, out.size(), f);
	fclose(f);
	return 0;
}
