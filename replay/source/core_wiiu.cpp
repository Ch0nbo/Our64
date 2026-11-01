// Our64 replay app: the mupen64plus core functions GLideN64 looks up by name.
// GLideN64 is linked in, so our64_dlsym (its DLSYM on __WIIU__) finds them in
// this table. The video extension does nothing: the null backend never draws.
#include <cstring>

// The config store, with its functions renamed: GLideN64 keeps global function
// pointers with the plain names.
#define CORE_FN(name) our64core_##name
#include "../core_config.cpp"

#include "m64p_vidext.h"

extern "C" {

static m64p_error vidOk() { return M64ERR_SUCCESS; }
static m64p_error vidModes(m64p_2d_size *, int *n) { *n = 0; return M64ERR_SUCCESS; }
static m64p_error vidRates(m64p_2d_size, int *n, int *) { *n = 0; return M64ERR_SUCCESS; }
static m64p_error vidMode(int, int, int, m64p_video_mode, m64p_video_flags) { return M64ERR_SUCCESS; }
static m64p_error vidModeRate(int, int, int, int, m64p_video_mode, m64p_video_flags) { return M64ERR_SUCCESS; }
static m64p_error vidCaption(const char *) { return M64ERR_SUCCESS; }
static m64p_error vidResize(int, int) { return M64ERR_SUCCESS; }
static m64p_function vidProc(const char *) { return nullptr; }
static m64p_error vidSetAttr(m64p_GLattr, int) { return M64ERR_SUCCESS; }
static m64p_error vidGetAttr(m64p_GLattr, int *v) { *v = 0; return M64ERR_SUCCESS; }
static uint32_t vidDefaultFb() { return 0; }

void * our64_dlsym(const char * name)
{
	static const struct { const char * name; void * fn; } table[] = {
#define F(n) {#n, (void *)&our64core_##n}
		F(ConfigOpenSection), F(ConfigDeleteSection), F(ConfigSaveSection), F(ConfigSaveFile),
		F(ConfigSetDefaultInt), F(ConfigSetDefaultFloat), F(ConfigSetDefaultBool), F(ConfigSetDefaultString),
		F(ConfigGetParamInt), F(ConfigGetParamFloat), F(ConfigGetParamBool), F(ConfigGetParamString),
		F(ConfigGetSharedDataFilepath), F(ConfigGetUserConfigPath), F(ConfigGetUserDataPath),
		F(ConfigGetUserCachePath), F(ConfigExternalOpen), F(ConfigExternalClose),
		F(ConfigExternalGetParameter), F(PluginGetVersion),
#undef F
		{"VidExt_Init", (void *)&vidOk}, {"VidExt_Quit", (void *)&vidOk},
		{"VidExt_ListFullscreenModes", (void *)&vidModes}, {"VidExt_ListFullscreenRates", (void *)&vidRates},
		{"VidExt_SetVideoMode", (void *)&vidMode}, {"VidExt_SetVideoModeWithRate", (void *)&vidModeRate},
		{"VidExt_SetCaption", (void *)&vidCaption}, {"VidExt_ToggleFullScreen", (void *)&vidOk},
		{"VidExt_ResizeWindow", (void *)&vidResize}, {"VidExt_GL_GetProcAddress", (void *)&vidProc},
		{"VidExt_GL_SetAttribute", (void *)&vidSetAttr}, {"VidExt_GL_GetAttribute", (void *)&vidGetAttr},
		{"VidExt_GL_SwapBuffers", (void *)&vidOk}, {"VidExt_GL_GetDefaultFramebuffer", (void *)&vidDefaultFb},
	};
	for (const auto & e : table)
		if (strcmp(e.name, name) == 0) return e.fn;
	return nullptr;
}

} // extern "C"
