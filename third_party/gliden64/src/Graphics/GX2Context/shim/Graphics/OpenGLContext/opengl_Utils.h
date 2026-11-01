#pragma once
// Our64 GX2 backend: stands in for OpenGLContext/opengl_Utils.h.
#include "opengl_GLInfo.h"

namespace opengl {

	struct Utils
	{
		static bool isGLError() { return false; }
	};

}
