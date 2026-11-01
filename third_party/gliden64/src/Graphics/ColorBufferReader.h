#pragma once
#include <vector>
#include <Types.h>
#include <Textures.h>

namespace graphics {

class ColorBufferReader
{
public:
	ColorBufferReader(CachedTexture * _pTexture);
	virtual ~ColorBufferReader() = default;

	virtual const u8 * readPixels(s32 _x0, s32 _y0, u32 _width, u32 _height, u32 _size, bool _sync);
	virtual void cleanUp() = 0;
	// Our64: rows of N64 RGBA5551 pixels (host u16 values, _width per row),
	// converted by the GPU, with GLideN64's dithering; null when the backend
	// cannot, and the caller converts RGBA8 pixels itself.
	virtual const u16 * readPixels16(s32 _x0, s32 _y0, u32 _width, u32 _height, bool _sync) { return nullptr; }
	// Our64: the buffer's first _height rows as N64 16-bit pixels, copied by
	// the GPU into _dst (N64 memory, rows of _width pixels); lands later, see
	// Context::finishMemoryCopies. False when the backend cannot.
	virtual bool copyPixels16ToMemory(void * _dst, u32 _width, u32 _height) { return false; }

protected:
	struct ReadColorBufferParams {
		s32 x0;
		s32 y0;
		u32 width;
		u32 height;
		bool sync;
		ColorFormatParam colorFormat;
		DatatypeParam colorType;
		u32 colorFormatBytes;
	};

	CachedTexture * m_pTexture;
	std::vector<u8> m_pixelData;
	std::vector<u8> m_tempPixelData;

private:
	const u8* _convertFloatTextureBuffer(const u8* _gpuData, size_t _gpuDataSize, u32 _width, u32 _height, u32 _heightOffset, u32 _stride);
	const u8* _convertIntegerTextureBuffer(const u8* _gpuData, size_t _gpuDataSize, u32 _width, u32 _height,u32 _heightOffset, u32 _stride, u32 _colorsPerPixel);

	// _gpuDataSize must be set to the number of bytes readable through the
	// returned pointer. The conversion helpers clamp against it, because
	// _params.height comes from the caller and is not bounded by the texture.
	virtual const u8 * _readPixels(const ReadColorBufferParams& _params, u32& _heightOffset, u32& _stride, size_t& _gpuDataSize) = 0;
};

}
