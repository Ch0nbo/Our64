#pragma once
// Our64 GX2 backend: stands in for OpenGLContext/opengl_Attributes.h. The
// GLSL code binds these locations by name; gx2_GL.cpp maps the names to the
// GX2 vertex layout instead.
#include "GLFunctions.h"

namespace opengl {

	namespace triangleAttrib {
		const GLuint position = 0U;
		const GLuint color = 1U;
		const GLuint texcoord = 2U;
		const GLuint numlights = 3U;
		const GLuint modify = 4U;
		const GLuint barycoords = 8U;
	}

	namespace rectAttrib {
		const GLuint position = 5U;
		const GLuint texcoord0 = 6U;
		const GLuint texcoord1 = 7U;
		const GLuint barycoords = 9U;
	}

#define MaxAttribIndex 10
}
