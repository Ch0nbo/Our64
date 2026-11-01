// Our64 replay64: the mupen64plus core functions a video plugin looks up with
// dlsym: the video extension here, the config store in replay/core_config.cpp.
// Built as its own library and loaded privately: exported from the program
// itself, these names would clash with the plugin's global function pointers
// of the same names.
#include <SDL2/SDL.h>
#include <cstdio>

#define M64P_CORE_PROTOTYPES 1
#include "m64p_types.h"
#include "m64p_common.h"
#include "m64p_plugin.h"
#include "m64p_config.h"
#include "m64p_vidext.h"

extern "C" {

// ---- video extension: one SDL window with an OpenGL 3.3 core context ----

static SDL_Window *g_window;
static SDL_GLContext g_gl;

EXPORT m64p_error CALL VidExt_Init(void) { return SDL_InitSubSystem(SDL_INIT_VIDEO) ? M64ERR_SYSTEM_FAIL : M64ERR_SUCCESS; }
EXPORT m64p_error CALL VidExt_Quit(void)
{
	if (g_gl) SDL_GL_DeleteContext(g_gl);
	if (g_window) SDL_DestroyWindow(g_window);
	g_gl = nullptr;
	g_window = nullptr;
	return M64ERR_SUCCESS;
}
EXPORT m64p_error CALL VidExt_ListFullscreenModes(m64p_2d_size *, int *n) { *n = 0; return M64ERR_SUCCESS; }
EXPORT m64p_error CALL VidExt_ListFullscreenRates(m64p_2d_size, int *n, int *) { *n = 0; return M64ERR_SUCCESS; }
EXPORT m64p_error CALL VidExt_SetVideoMode(int w, int h, int, m64p_video_mode, m64p_video_flags)
{
	if (!g_window) {
		g_window = SDL_CreateWindow("replay64", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, w, h,
		                            SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN);
		if (!g_window) { fprintf(stderr, "SDL window: %s\n", SDL_GetError()); return M64ERR_SYSTEM_FAIL; }
		g_gl = SDL_GL_CreateContext(g_window);
		if (!g_gl) { fprintf(stderr, "GL context: %s\n", SDL_GetError()); return M64ERR_SYSTEM_FAIL; }
	} else {
		SDL_SetWindowSize(g_window, w, h);
	}
	return M64ERR_SUCCESS;
}
EXPORT m64p_error CALL VidExt_SetVideoModeWithRate(int w, int h, int, int bpp, m64p_video_mode m, m64p_video_flags f)
{
	return VidExt_SetVideoMode(w, h, bpp, m, f);
}
EXPORT m64p_error CALL VidExt_SetCaption(const char *) { return M64ERR_SUCCESS; }
EXPORT m64p_error CALL VidExt_ToggleFullScreen(void) { return M64ERR_SUCCESS; }
EXPORT m64p_error CALL VidExt_ResizeWindow(int w, int h) { SDL_SetWindowSize(g_window, w, h); return M64ERR_SUCCESS; }
EXPORT m64p_function CALL VidExt_GL_GetProcAddress(const char *name) { return (m64p_function)SDL_GL_GetProcAddress(name); }
EXPORT m64p_error CALL VidExt_GL_SetAttribute(m64p_GLattr a, int v)
{
	switch (a) {
	case M64P_GL_DOUBLEBUFFER: SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, v); break;
	case M64P_GL_BUFFER_SIZE: SDL_GL_SetAttribute(SDL_GL_BUFFER_SIZE, v); break;
	case M64P_GL_DEPTH_SIZE: SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, v); break;
	case M64P_GL_RED_SIZE: SDL_GL_SetAttribute(SDL_GL_RED_SIZE, v); break;
	case M64P_GL_GREEN_SIZE: SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, v); break;
	case M64P_GL_BLUE_SIZE: SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE, v); break;
	case M64P_GL_ALPHA_SIZE: SDL_GL_SetAttribute(SDL_GL_ALPHA_SIZE, v); break;
	case M64P_GL_MULTISAMPLEBUFFERS: SDL_GL_SetAttribute(SDL_GL_MULTISAMPLEBUFFERS, v); break;
	case M64P_GL_MULTISAMPLESAMPLES: SDL_GL_SetAttribute(SDL_GL_MULTISAMPLESAMPLES, v); break;
	case M64P_GL_CONTEXT_MAJOR_VERSION: SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, v); break;
	case M64P_GL_CONTEXT_MINOR_VERSION: SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, v); break;
	case M64P_GL_CONTEXT_PROFILE_MASK:
		SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK,
		                    v == M64P_GL_CONTEXT_PROFILE_CORE ? SDL_GL_CONTEXT_PROFILE_CORE
		                    : v == M64P_GL_CONTEXT_PROFILE_ES ? SDL_GL_CONTEXT_PROFILE_ES
		                                                      : SDL_GL_CONTEXT_PROFILE_COMPATIBILITY);
		break;
	case M64P_GL_SWAP_CONTROL: break; // set after the context exists
	default: break;
	}
	return M64ERR_SUCCESS;
}
EXPORT m64p_error CALL VidExt_GL_GetAttribute(m64p_GLattr a, int *v)
{
	SDL_GLattr s;
	switch (a) {
	case M64P_GL_DOUBLEBUFFER: s = SDL_GL_DOUBLEBUFFER; break;
	case M64P_GL_DEPTH_SIZE: s = SDL_GL_DEPTH_SIZE; break;
	case M64P_GL_CONTEXT_MAJOR_VERSION: s = SDL_GL_CONTEXT_MAJOR_VERSION; break;
	case M64P_GL_CONTEXT_MINOR_VERSION: s = SDL_GL_CONTEXT_MINOR_VERSION; break;
	case M64P_GL_CONTEXT_PROFILE_MASK: *v = M64P_GL_CONTEXT_PROFILE_CORE; return M64ERR_SUCCESS;
	case M64P_GL_SWAP_CONTROL: *v = 0; return M64ERR_SUCCESS;
	default: *v = 0; return M64ERR_SUCCESS;
	}
	return SDL_GL_GetAttribute(s, v) ? M64ERR_SYSTEM_FAIL : M64ERR_SUCCESS;
}
EXPORT m64p_error CALL VidExt_GL_SwapBuffers(void) { SDL_GL_SwapWindow(g_window); return M64ERR_SUCCESS; }
EXPORT uint32_t CALL VidExt_GL_GetDefaultFramebuffer(void) { return 0; }

} // extern "C"
