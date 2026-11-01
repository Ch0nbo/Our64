// Our64: a window that does not exist, for the null backend. Sizes come from
// the config as in the mupen64plus window; nothing is shown or read back.
#include <cstring>
#include <Config.h>
#include <DisplayWindow.h>

class DisplayWindowNull : public DisplayWindow
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
	void _swapBuffers() override {}
	void _saveScreenshot() override {}
	void _saveBufferContent(graphics::ObjectHandle, CachedTexture *) override {}
	bool _resizeWindow() override { return true; }
	void _changeWindow() override {}
	void _readScreen(void **, long *, long *) override {}
	void _readScreen2(void * _dest, int * _width, int * _height, int) override {
		if (_width == nullptr || _height == nullptr) return;
		*_width = m_screenWidth;
		*_height = m_screenHeight;
		if (_dest != nullptr) memset(_dest, 0, size_t(m_screenWidth) * m_screenHeight * 3);
	}
	graphics::ObjectHandle _getDefaultFramebuffer() override { return graphics::ObjectHandle::null; }
};

DisplayWindow & DisplayWindow::get()
{
	static DisplayWindowNull video;
	return video;
}
