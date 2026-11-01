#pragma once
// Our64 GX2 backend: stands in for OpenGLContext/opengl_CachedFunctions.h.
// The GLSL code only uses CachedUseProgram.
#include <Graphics/ObjectHandle.h>
#include "GLFunctions.h"

namespace opengl {

	class CachedUseProgram
	{
	public:
		void useProgram(graphics::ObjectHandle _program)
		{
			if (_program == m_cached)
				return;
			m_cached = _program;
			glUseProgram(GLuint(_program));
		}
		void reset() { m_cached = graphics::ObjectHandle(~0u); }

	private:
		graphics::ObjectHandle m_cached = graphics::ObjectHandle(~0u);
	};

}
