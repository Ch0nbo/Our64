// Our64 ra-preview (PC): draws a frame through one of RetroArch's
// single-pass slang shaders (libretro's slang-shaders, read from where the
// system has them; none of their code is kept here), headless, as RetroArch's
// "viewport" scale does: the frame at the core's size in, the picture's size
// on the TV out. For comparing Our64's TV filters against them
// (compare_ra.py).
//
// Usage: ra-preview <shader.slang> <linear 0|1> <in.ppm> <out.ppm> <tv width>
//                   <tv height> <43 or 169> [NAME=value ...]
//   linear: the preset's filter_linear0. NAME=value sets a shader parameter
//   (otherwise its #pragma parameter default).
//
// The slang source is Vulkan GLSL; it is made GL 4.5 GLSL by text: the
// push-constant block becomes a uniform block, "set = N" goes, and the part
// before "#pragma stage" is given to both stages. Every block member is then
// set by name: MVP, SourceSize, OriginalSize, OutputSize, FrameCount, and
// the parameters.
#include <EGL/egl.h>
#include <EGL/eglext.h>
#define GL_GLEXT_PROTOTYPES
#include <GL/gl.h>
#include <GL/glext.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

namespace {

std::string readText(const std::string & path)
{
	std::ifstream f(path);
	if (!f) {
		fprintf(stderr, "cannot read %s\n", path.c_str());
		exit(1);
	}
	std::stringstream s;
	s << f.rdbuf();
	return s.str();
}

// #include "file", relative to the including file.
std::string expand(const std::string & path)
{
	const std::string dir = path.substr(0, path.find_last_of('/') + 1);
	std::istringstream in(readText(path));
	std::string out, line;
	static const std::regex inc("^\\s*#include\\s+\"([^\"]+)\"");
	std::smatch m;
	while (std::getline(in, line)) {
		if (std::regex_search(line, m, inc))
			out += expand(dir + m[1].str());
		else
			out += line + "\n";
	}
	return out;
}

GLuint compile(GLenum type, const std::string & src)
{
	const GLuint s = glCreateShader(type);
	const char * p = src.c_str();
	glShaderSource(s, 1, &p, nullptr);
	glCompileShader(s);
	GLint ok = 0;
	glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
	if (!ok) {
		char log[8192];
		glGetShaderInfoLog(s, sizeof(log), nullptr, log);
		fprintf(stderr, "compile failed:\n%s\n", log);
		exit(1);
	}
	return s;
}

} // namespace

int main(int argc, char ** argv)
{
	if (argc < 8) {
		fprintf(stderr, "usage: ra-preview <shader.slang> <linear 0|1> <in.ppm> <out.ppm> <tv w> <tv h> <43|169> [NAME=value ...]\n");
		return 1;
	}
	const bool linear = atoi(argv[2]) != 0;
	const int W = atoi(argv[5]), H = atoi(argv[6]);
	const bool picture_43 = strcmp(argv[7], "43") == 0;
	std::map<std::string, float> params;

	// The source: its parameters' defaults, and its two stages.
	std::string src = expand(argv[1]);
	src = std::regex_replace(src, std::regex("layout\\s*\\(\\s*push_constant\\s*\\)"), "layout(std140)");
	src = std::regex_replace(src, std::regex("set\\s*=\\s*\\d+\\s*,\\s*"), "");
	{
		static const std::regex par("#pragma\\s+parameter\\s+(\\w+)\\s+\"[^\"]*\"\\s+([-+0-9.eE]+)");
		for (std::sregex_iterator i(src.begin(), src.end(), par), e; i != e; ++i)
			params[(*i)[1].str()] = float(atof((*i)[2].str().c_str()));
	}
	for (int i = 8; i < argc; i++)
		if (const char * eq = strchr(argv[i], '='))
			params[std::string(argv[i], eq - argv[i])] = float(atof(eq + 1));
	const size_t vs_at = src.find("#pragma stage vertex"), fs_at = src.find("#pragma stage fragment");
	if (vs_at == std::string::npos || fs_at == std::string::npos || fs_at < vs_at) {
		fprintf(stderr, "no vertex and fragment stage\n");
		return 1;
	}
	const std::string common = src.substr(0, vs_at);
	const std::string vs = common + src.substr(vs_at, fs_at - vs_at), fs = common + src.substr(fs_at);

	int fw = 0, fh = 0, maxv = 0;
	FILE * f = fopen(argv[3], "rb");
	if (f == nullptr || fscanf(f, "P6 %d %d %d", &fw, &fh, &maxv) != 3 || maxv != 255) {
		fprintf(stderr, "cannot read %s (binary P6, maxval 255)\n", argv[3]);
		return 1;
	}
	fgetc(f);
	std::vector<unsigned char> frame(size_t(fw) * fh * 3);
	if (fread(frame.data(), 1, frame.size(), f) != frame.size())
		return 1;
	fclose(f);

	EGLDisplay dpy = EGL_NO_DISPLAY;
	auto getPlatformDisplay = (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
	if (getPlatformDisplay != nullptr)
		dpy = getPlatformDisplay(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, nullptr);
	if (dpy == EGL_NO_DISPLAY || !eglInitialize(dpy, nullptr, nullptr)) {
		fprintf(stderr, "no EGL surfaceless display\n");
		return 1;
	}
	eglBindAPI(EGL_OPENGL_API);
	const EGLint ctxAttr[] = {EGL_CONTEXT_MAJOR_VERSION, 4, EGL_CONTEXT_MINOR_VERSION, 5,
	                          EGL_CONTEXT_OPENGL_PROFILE_MASK, EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT, EGL_NONE};
	EGLContext ctx = eglCreateContext(dpy, EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT, ctxAttr);
	if (ctx == EGL_NO_CONTEXT || !eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx)) {
		fprintf(stderr, "no GL 4.5 core context\n");
		return 1;
	}

	const GLuint prog = glCreateProgram();
	glAttachShader(prog, compile(GL_VERTEX_SHADER, vs));
	glAttachShader(prog, compile(GL_FRAGMENT_SHADER, fs));
	glLinkProgram(prog);
	GLint linked = 0;
	glGetProgramiv(prog, GL_LINK_STATUS, &linked);
	if (!linked) {
		char log[4096];
		glGetProgramInfoLog(prog, sizeof(log), nullptr, log);
		fprintf(stderr, "link failed: %s\n", log);
		return 1;
	}
	glUseProgram(prog);

	uint32_t w = picture_43 ? (H * 4 / 3 + 1) & ~1u : W;
	if (w > uint32_t(W))
		w = W;
	const int x = (W - int(w)) / 2;

	// Every block: a buffer with its members set by name.
	GLint blocks = 0, uniforms = 0;
	glGetProgramiv(prog, GL_ACTIVE_UNIFORM_BLOCKS, &blocks);
	glGetProgramiv(prog, GL_ACTIVE_UNIFORMS, &uniforms);
	std::vector<std::vector<float>> data(blocks);
	for (GLint b = 0; b < blocks; b++) {
		GLint size = 0;
		glGetActiveUniformBlockiv(prog, b, GL_UNIFORM_BLOCK_DATA_SIZE, &size);
		data[b].assign(size / 4 + 4, 0.0f);
	}
	for (GLint u = 0; u < uniforms; u++) {
		char name[256];
		GLint n = 0, block = -1, offset = 0;
		GLenum type = 0;
		const GLuint idx = GLuint(u);
		glGetActiveUniform(prog, idx, sizeof(name), nullptr, &n, &type, name);
		glGetActiveUniformsiv(prog, 1, &idx, GL_UNIFORM_BLOCK_INDEX, &block);
		glGetActiveUniformsiv(prog, 1, &idx, GL_UNIFORM_OFFSET, &offset);
		if (block < 0)
			continue; // the sampler
		const char * dot = strrchr(name, '.');
		const std::string member = dot ? dot + 1 : name;
		float * at = data[block].data() + offset / 4;
		const float sw = float(fw), sh = float(fh), ow = float(w), oh = float(H);
		if (member == "MVP") { // Position 0..1, y = 0 the top, to clip space
			const float m[16] = {2, 0, 0, 0, 0, -2, 0, 0, 0, 0, 1, 0, -1, 1, 0, 1};
			memcpy(at, m, sizeof(m));
		} else if (member == "SourceSize" || member == "OriginalSize") {
			const float v[4] = {sw, sh, 1 / sw, 1 / sh};
			memcpy(at, v, sizeof(v));
		} else if (member == "OutputSize" || member == "FinalViewportSize") {
			const float v[4] = {ow, oh, 1 / ow, 1 / oh};
			memcpy(at, v, sizeof(v));
		} else if (member == "FrameCount" || member == "FrameDirection") {
			const uint32_t v = member == "FrameDirection" ? 1 : 0;
			memcpy(at, &v, 4);
		} else if (params.count(member)) {
			*at = params[member];
		} else {
			fprintf(stderr, "ra-preview: %s left at 0\n", name);
		}
	}
	for (GLint b = 0; b < blocks; b++) {
		GLuint buf;
		glGenBuffers(1, &buf);
		glBindBuffer(GL_UNIFORM_BUFFER, buf);
		glBufferData(GL_UNIFORM_BUFFER, data[b].size() * 4, data[b].data(), GL_STATIC_DRAW);
		glUniformBlockBinding(prog, b, 8 + b);
		glBindBufferBase(GL_UNIFORM_BUFFER, 8 + b, buf);
	}

	GLuint tex;
	glGenTextures(1, &tex);
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, tex);
	glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, fw, fh, 0, GL_RGB, GL_UNSIGNED_BYTE, frame.data());
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, linear ? GL_LINEAR : GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, linear ? GL_LINEAR : GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	const GLint source = glGetUniformLocation(prog, "Source");
	if (source >= 0)
		glUniform1i(source, 0);

	GLuint rb, fbo;
	glGenRenderbuffers(1, &rb);
	glBindRenderbuffer(GL_RENDERBUFFER, rb);
	glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, W, H);
	glGenFramebuffers(1, &fbo);
	glBindFramebuffer(GL_FRAMEBUFFER, fbo);
	glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, rb);

	// Position (location 0) and TexCoord (location 1): the same 0..1 corner.
	static const float quad[4][6] = {
		{0, 1, 0, 1, 0, 1}, {1, 1, 0, 1, 1, 1}, {0, 0, 0, 1, 0, 0}, {1, 0, 0, 1, 1, 0},
	};
	GLuint vao, vbo;
	glGenVertexArrays(1, &vao);
	glBindVertexArray(vao);
	glGenBuffers(1, &vbo);
	glBindBuffer(GL_ARRAY_BUFFER, vbo);
	glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);
	glEnableVertexAttribArray(0);
	glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, 24, (void *)0);
	glEnableVertexAttribArray(1);
	glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 24, (void *)16);

	glViewport(0, 0, W, H);
	glClearColor(0, 0, 0, 1);
	glClear(GL_COLOR_BUFFER_BIT);
	glViewport(x, 0, int(w), H);
	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
	glFinish();

	std::vector<unsigned char> out(size_t(W) * H * 4);
	glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, out.data());
	f = fopen(argv[4], "wb");
	if (f == nullptr)
		return 1;
	fprintf(f, "P6\n%d %d\n255\n", W, H);
	for (int y = H - 1; y >= 0; y--) // GL's rows are bottom first
		for (int xx = 0; xx < W; xx++)
			fwrite(&out[(size_t(y) * W + xx) * 4], 1, 3, f);
	fclose(f);
	return 0;
}
