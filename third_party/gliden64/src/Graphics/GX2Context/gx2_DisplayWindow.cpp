// Our64 GX2 backend: the display window is an offscreen color buffer the
// size of the configured window (gx2_ContextImpl owns it). Each swap shows
// it through our64_gx2_present, if the app set one, and ends the frame.
#include <Config.h>
#include <DisplayWindow.h>
#include "gx2_ContextImpl.h"
#include "../../our64_at.h"

class DisplayWindowGX2 : public DisplayWindow
{
private:
	bool _start() override {
		m_bFullscreen = false;
		m_screenWidth = config.video.windowedWidth;
		m_screenHeight = config.video.windowedHeight;
		m_screenRefresh = 60;
		_setBufferSize();
		return true;
	}
	void _stop() override {}
	void _restart() override {}
	void _swapBuffers() override {
		OUR64_AT();
		if (our64_gx2_present != nullptr)
			our64_gx2_present(gx2::presentColorBuffer());
		OUR64_AT();
		gx2::endFrame();
		OUR64_AT();
	}
	void _saveScreenshot() override {}
	void _saveBufferContent(graphics::ObjectHandle, CachedTexture *) override {}
	bool _resizeWindow() override { _setBufferSize(); return true; } // Our64: the picture shape changed
	void _changeWindow() override {}
	void _readScreen(void **, long *, long *) override {}
	void _readScreen2(void * _dest, int * _width, int * _height, int) override {
		if (_width == nullptr || _height == nullptr) return;
		*_width = m_screenWidth;
		*_height = m_screenHeight;
		if (_dest != nullptr) gx2::readScreen(static_cast<u8*>(_dest), m_screenWidth, m_screenHeight);
	}
	graphics::ObjectHandle _getDefaultFramebuffer() override { return graphics::ObjectHandle::defaultFramebuffer; }
};

DisplayWindow & DisplayWindow::get()
{
	static DisplayWindowGX2 video;
	return video;
}

// Our64: a new picture shape (Config::aStretch or Config::a43), applied at
// the next VI through the resize path. Graphics core only.
extern "C" void our64_gx2_set_aspect(int aspect)
{
	if (config.frameBufferEmulation.aspect == u32(aspect))
		return;
	config.frameBufferEmulation.aspect = aspect;
	DisplayWindow::get().setWindowSize(1, 1); // any size: _resizeWindow sizes from the screen
}
