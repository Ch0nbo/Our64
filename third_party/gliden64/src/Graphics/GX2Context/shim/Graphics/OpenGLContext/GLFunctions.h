#pragma once
// Our64 GX2 backend: stands in for OpenGLContext/GLFunctions.h when the GLSL
// shader generators (OpenGLContext/GLSL) are built for the Wii U. The include
// path puts GX2Context/shim first. The GL functions declared here are the
// ones those files call; GX2Context/gx2_GL.cpp implements them on CafeGLSL
// and GX2.
#define GL_GLEXT_PROTOTYPES 1
#include <cstring>  // the real header brings it in through the platform headers
#include <Types.h>
#include <GL/glcorearb.h>

#ifndef GL_COMPLETION_STATUS_ARB
#define GL_COMPLETION_STATUS_ARB 0x91B1
#endif
#ifndef GL_TEXTURE_MAX_ANISOTROPY_EXT
#define GL_TEXTURE_MAX_ANISOTROPY_EXT 0x84FE
#endif

// Optional GL entry points: none are present.
#define IS_GL_FUNCTION_VALID(proc_name) false
