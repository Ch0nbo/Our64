// Our64: a graphics backend that draws nothing. The decoder runs in full
// (display lists, vertex transform, clipping, texture loading and conversion,
// frame-buffer tracking); every GPU call returns at once. Used to time the
// decoder's CPU cost on its own.
#include <cmath>
#include <cstdio>
#include <memory>
#include <vector>

#include <Graphics/ContextImpl.h>
#include <Graphics/ShaderProgram.h>
#include <Graphics/ColorBufferReader.h>
#include <Graphics/PixelBuffer.h>
#include <Graphics/FramebufferTextureFormats.h>
#include <Graphics/OpenGLContext/GLSL/glsl_CombinerInputs.h>
#include <Combiner.h>
#include <CombinerKey.h>
#include <DisplayWindow.h>
#include <GraphicsDrawer.h>
#include <gDP.h>
#include <gSP.h>

#include "null_ContextImpl.h"

namespace {

using namespace graphics;

// Any distinct non-zero values do: the core only passes them back.
struct NullFormats : FramebufferTextureFormats {
	NullFormats() {
		colorInternalFormat = InternalColorFormatParam(1); colorFormat = ColorFormatParam(1);
		colorType = DatatypeParam(1); colorFormatBytes = 4;
		monochromeInternalFormat = InternalColorFormatParam(2); monochromeFormat = ColorFormatParam(2);
		monochromeType = DatatypeParam(1); monochromeFormatBytes = 1;
		depthInternalFormat = InternalColorFormatParam(3); depthFormat = ColorFormatParam(3);
		depthType = DatatypeParam(2); depthFormatBytes = 4;
		depthImageInternalFormat = InternalColorFormatParam(4); depthImageFormat = ColorFormatParam(2);
		depthImageType = DatatypeParam(2); depthImageFormatBytes = 4;
		lutInternalFormat = InternalColorFormatParam(5); lutFormat = ColorFormatParam(4);
		lutType = DatatypeParam(3); lutFormatBytes = 4;
		fontInternalFormat = InternalColorFormatParam(2); fontFormat = ColorFormatParam(2);
		fontType = DatatypeParam(1); fontFormatBytes = 1;
	}
};

// Which inputs a combine mode reads decides whether the core loads textures,
// so this matches the GLSL builder's analysis.
glsl::CombinerInputs stageInputs(const CombinerStage & _stage)
{
	glsl::CombinerInputs inputs;
	for (u32 i = 0; i < _stage.numOps; ++i) {
		inputs.addInput(_stage.op[i].param1);
		if (_stage.op[i].op == INTER) {
			inputs.addInput(_stage.op[i].param2);
			inputs.addInput(_stage.op[i].param3);
		}
	}
	return inputs;
}

class NullCombinerProgram : public CombinerProgram {
public:
	NullCombinerProgram(Combiner & _color, Combiner & _alpha, const CombinerKey & _key) : m_key(_key) {
		m_inputs = stageInputs(_alpha.stage[0]);
		m_inputs += stageInputs(_color.stage[0]);
		if (_key.getCycleType() == G_CYC_2CYCLE) {
			if (_alpha.numStages == 2) m_inputs += stageInputs(_alpha.stage[1]);
			if (_color.numStages == 2) m_inputs += stageInputs(_color.stage[1]);
		}
	}
	void activate() override {}
	void update(bool) override {}
	const CombinerKey & getKey() const override { return m_key; }
	bool usesTexture() const override { return m_inputs.usesTexture(); }
	bool usesTile(u32 _t) const override { return m_inputs.usesTile(_t); }
	bool usesShade() const override { return m_inputs.usesShade(); }
	bool usesLOD() const override { return m_inputs.usesLOD(); }
	bool usesHwLighting() const override { return false; }
	bool getBinaryForm(std::vector<char> &) override { return false; }
private:
	CombinerKey m_key;
	glsl::CombinerInputs m_inputs;
};

class NullShader : public ShaderProgram {
public:
	void activate() override {}
};

class NullTexrectShader : public TexrectDrawerShaderProgram {
public:
	void activate() override {}
	void setTextureSize(u32, u32) override {}
	void setEnableAlphaTest(int) override {}
};

class NullTextShader : public TextDrawerShaderProgram {
public:
	void activate() override {}
	void setTextColor(float *) override {}
};

class NullPixelReadBuffer : public PixelReadBuffer {
public:
	explicit NullPixelReadBuffer(size_t _size) : m_data(_size) {}
	void readPixels(s32, s32, u32, u32, Parameter, Parameter) override {}
	void * getDataRange(u32 _offset, u32 _range) override {
		if (_offset + _range > m_data.size()) m_data.resize(_offset + _range);
		return m_data.data() + _offset;
	}
	void closeReadBuffer() override {}
	void bind() override {}
	void unbind() override {}
private:
	std::vector<u8> m_data;
};

// Readback returns black pixels of the requested size.
class NullColorBufferReader : public ColorBufferReader {
public:
	explicit NullColorBufferReader(CachedTexture * _pTexture) : ColorBufferReader(_pTexture) {}
	void cleanUp() override {}
private:
	const u8 * _readPixels(const ReadColorBufferParams & _params, u32 & _heightOffset, u32 & _stride,
	                       size_t & _gpuDataSize) override {
		const size_t size = size_t(_params.width) * _params.height * _params.colorFormatBytes;
		if (m_zeros.size() < size) m_zeros.assign(size, 0);
		_heightOffset = 0;
		_stride = _params.width;
		_gpuDataSize = size;
		return m_zeros.data();
	}
	std::vector<u8> m_zeros;
};


// ---- input trace: one line per backend call that carries decoder output ----
// Compared between the little-endian PC build and the big-endian console build
// to check the endian port. Values are hashed as numbers, not bytes, and
// floats are rounded to a fixed grid first, so both builds hash the same
// numbers when the decoder agrees.

// With our64_trace_verbose set, each hashed value is also printed, so two
// traces can be compared value by value.
struct Hash {
	Hash() { if (our64_trace_verbose) fputs("values", our64_trace); }
	u32 h = 2166136261u;
	void u(u32 _v) {
		if (our64_trace_verbose) fprintf(our64_trace, " %x", _v);
		for (int i = 0; i < 4; ++i) { h ^= (_v >> (8 * i)) & 0xFF; h *= 16777619u; }
	}
	void f(f32 _v, f32 _scale) { u(u32(s32(std::lrint(f64(_v) * _scale)))); }
};

void hashColor(Hash & _h, const gDPInfo::Color & _c)
{
	_h.f(_c.r, 255); _h.f(_c.g, 255); _h.f(_c.b, 255); _h.f(_c.a, 255);
}

// The RDP state a draw depends on that the null backend never sees as a call.
void hashState(Hash & _h)
{
	_h.u(gDP.otherMode.l); _h.u(gDP.otherMode.h);
	_h.u(gDP.combine.muxs0); _h.u(gDP.combine.muxs1);
	hashColor(_h, gDP.primColor); _h.f(gDP.primColor.l, 255); _h.f(gDP.primColor.m, 255);
	hashColor(_h, gDP.envColor);
	hashColor(_h, gDP.fogColor);
	hashColor(_h, gDP.blendColor);
	_h.u(gDP.fillColor.color);
	for (u32 t = 0; t < 2; ++t) {
		const gDPTile & tile = gDP.tiles[t];
		_h.u(tile.format); _h.u(tile.size); _h.u(tile.line); _h.u(tile.tmem); _h.u(tile.palette);
		_h.u(tile.cms); _h.u(tile.cmt); _h.u(tile.masks); _h.u(tile.maskt); _h.u(tile.shifts); _h.u(tile.shiftt);
		_h.u(tile.uls); _h.u(tile.ult); _h.u(tile.lrs); _h.u(tile.lrt);
	}
}

u32 texelBytes(const ColorFormatParam & _format, const DatatypeParam & _type)
{
	switch (u32(_format)) { // NullFormats: color, monochrome, depth, LUT
	case 1: return 4;
	case 2: return 1;
	case 3: return 4;
	case 4: return 4;
	}
	if (_type == datatype::UNSIGNED_BYTE) {
		if (_format == colorFormat::RGBA) return 4;
		if (_format == colorFormat::RED_GREEN_BLUE) return 3;
		if (_format == colorFormat::RG) return 2;
		return 1;
	}
	if (_type == datatype::UNSIGNED_INT || _type == datatype::FLOAT) return 4;
	return 2;
}

u32 hashTexels(const void * _data, u32 _width, u32 _height, u32 _bytes)
{
	const bool verbose = our64_trace_verbose;
	our64_trace_verbose = false;
	struct Restore { bool v; ~Restore() { our64_trace_verbose = v; } } restore{verbose};
	Hash h;
	if (_data == nullptr) return 0;
	const size_t n = size_t(_width) * _height;
	if (verbose) { // the first texels, to see how two traces differ
		fputs("texels", our64_trace);
		for (size_t i = 0; i < n && i < 24; ++i)
			fprintf(our64_trace, " %x", _bytes == 4 ? static_cast<const u32*>(_data)[i]
			                           : _bytes == 2 ? static_cast<const u16*>(_data)[i] : static_cast<const u8*>(_data)[i]);
	}
	if (_bytes == 4) for (size_t i = 0; i < n; ++i) h.u(static_cast<const u32*>(_data)[i]);
	else if (_bytes == 2) for (size_t i = 0; i < n; ++i) h.u(static_cast<const u16*>(_data)[i]);
	else for (size_t i = 0; i < n * _bytes; ++i) h.u(static_cast<const u8*>(_data)[i]);
	return h.h;
}

void hashVertex(Hash & _h, const SPVertex & _v, bool _flat)
{
	_h.f(_v.x, 64); _h.f(_v.y, 64); _h.f(_v.z, 64); _h.f(_v.w, 64);
	const f32 * c = _flat ? &_v.flat_r : &_v.r;
	for (int i = 0; i < 4; ++i) _h.f(c[i], 255);
	_h.f(_v.s, 32); _h.f(_v.t, 32);
	_h.u(_v.modify); _h.u(_v.clip);
}

class NullContextImpl : public ContextImpl {
public:
	void init() override {}
	void destroy() override {}
	void setClampMode(ClampMode _mode) override { m_clamp = _mode; }
	ClampMode getClampMode() override { return m_clamp; }
	void enable(EnableParam, bool) override {}
	u32 isEnabled(EnableParam) override { return 0; }
	void cullFace(CullModeParam) override {}
	void enableDepthWrite(bool) override {}
	void setDepthCompare(CompareParam) override {}
	void setViewport(s32, s32, s32, s32) override {}
	void setScissor(s32, s32, s32, s32) override {}
	void setBlending(BlendParam, BlendParam) override {}
	void setBlendingSeparate(BlendParam, BlendParam, BlendParam, BlendParam) override {}
	void setBlendColor(f32, f32, f32, f32) override {}
	void clearColorBuffer(f32 _r, f32 _g, f32 _b, f32 _a) override {
		if (our64_trace) {
			Hash h; h.f(_r, 255); h.f(_g, 255); h.f(_b, 255); h.f(_a, 255);
			fprintf(our64_trace, "clear fb%u %08x\n", m_fb, h.h);
		}
	}
	void clearDepthBuffer() override {}
	void setPolygonOffset(f32, f32) override {}
	ObjectHandle createTexture(Parameter) override { return ObjectHandle(++m_names); }
	void deleteTexture(ObjectHandle) override {}
	void init2DTexture(const Context::InitTextureParams & _p) override {
		if (our64_trace)
			fprintf(our64_trace, "teximage t%u %ux%u fmt%x type%x %08x\n", u32(_p.handle), _p.width, _p.height,
			        u32(_p.format), u32(_p.dataType),
			        hashTexels(_p.data, _p.width, _p.height, texelBytes(_p.format, _p.dataType)));
	}
	void update2DTexture(const Context::UpdateTextureDataParams & _p) override {
		if (our64_trace)
			fprintf(our64_trace, "texsub t%u %u,%u %ux%u fmt%x type%x %08x\n", u32(_p.handle), _p.x, _p.y,
			        _p.width, _p.height, u32(_p.format), u32(_p.dataType),
			        hashTexels(_p.data, _p.width, _p.height, texelBytes(_p.format, _p.dataType)));
	}
	void setTextureParameters(const Context::TexParameters & _p) override {
		const u32 unit = u32(_p.textureUnitIndex); // the core binds textures here
		if (unit < 8) m_bound[unit] = u32(_p.handle);
	}
	void bindTexture(const Context::BindTextureParameters & _p) override {
		const u32 unit = u32(_p.textureUnitIndex);
		if (unit < 8) m_bound[unit] = u32(_p.texture);
	}
	void setTextureUnpackAlignment(s32 _param) override { m_unpack = _param; }
	s32 getTextureUnpackAlignment() const override { return m_unpack; }
	s32 getMaxTextureSize() const override { return 8192; }
	f32 getMaxAnisotropy() const override { return 16.0f; }
	void bindImageTexture(const Context::BindImageTextureParameters &) override {}
	u32 convertInternalTextureFormat(u32 _format) const override { return _format; }
	void textureBarrier() override {}
	FramebufferTextureFormats * getFramebufferTextureFormats() override { return new NullFormats; }
	ObjectHandle createFramebuffer() override { return ObjectHandle(++m_names); }
	void deleteFramebuffer(ObjectHandle) override {}
	void bindFramebuffer(BufferTargetParam, ObjectHandle _name) override { m_fb = u32(_name); }
	void addFrameBufferRenderTarget(const Context::FrameBufferRenderTarget &) override {}
	ObjectHandle createRenderbuffer() override { return ObjectHandle(++m_names); }
	void initRenderbuffer(const Context::InitRenderbufferParams &) override {}
	bool blitFramebuffers(const Context::BlitFramebuffersParams & _p) override {
		if (our64_trace)
			fprintf(our64_trace, "blit fb%u %d,%d,%d,%d -> fb%u %d,%d,%d,%d\n", u32(_p.readBuffer), _p.srcX0, _p.srcY0,
			        _p.srcX1, _p.srcY1, u32(_p.drawBuffer), _p.dstX0, _p.dstY0, _p.dstX1, _p.dstY1);
		return true;
	}
	void setDrawBuffers(u32) override {}
	PixelReadBuffer * createPixelReadBuffer(size_t _sizeInBytes) override { return new NullPixelReadBuffer(_sizeInBytes); }
	ColorBufferReader * createColorBufferReader(CachedTexture * _pTexture) override { return new NullColorBufferReader(_pTexture); }
	bool isCombinerProgramBuilderObsolete() override { return false; }
	void resetCombinerProgramBuilder() override {}
	CombinerProgram * createCombinerProgram(Combiner & _color, Combiner & _alpha, const CombinerKey & _key) override {
		return new NullCombinerProgram(_color, _alpha, _key);
	}
	void createCombinerProgramAsync(Combiner &, Combiner &, const CombinerKey &) override {}
	void getCompiledCombinerPrograms(Combiners &, bool) override {}
	void dropPendingCombinerPrograms() override {}
	bool saveShadersStorage(const Combiners &) override { return false; }
	bool loadShadersStorage(Combiners &) override { return false; }
	ShaderProgram * createDepthFogShader() override { return new NullShader; }
	TexrectDrawerShaderProgram * createTexrectDrawerDrawShader() override { return new NullTexrectShader; }
	ShaderProgram * createTexrectDrawerClearShader() override { return new NullShader; }
	ShaderProgram * createTexrectUpscaleCopyShader() override { return new NullShader; }
	ShaderProgram * createTexrectColorAndDepthUpscaleCopyShader() override { return new NullShader; }
	ShaderProgram * createTexrectDownscaleCopyShader() override { return new NullShader; }
	ShaderProgram * createTexrectColorAndDepthDownscaleCopyShader() override { return new NullShader; }
	ShaderProgram * createGammaCorrectionShader() override { return new NullShader; }
	ShaderProgram * createFXAAShader() override { return new NullShader; }
	TextDrawerShaderProgram * createTextDrawerShader() override { return new NullTextShader; }
	void resetShaderProgram() override {}
	void drawTriangles(const Context::DrawTriangleParameters & _p) override {
		++our64_null_draws;
		if (!our64_trace) return;
		Hash h;
		hashState(h);
		if (_p.elements == nullptr)
			for (u32 i = 0; i < _p.verticesCount; ++i) hashVertex(h, _p.vertices[i], _p.flatColors);
		else
			for (u32 i = 0; i < _p.elementsCount; ++i) {
				// Always u16, whatever elementsType says (as in the GL backend).
				const u32 e = static_cast<const u16*>(_p.elements)[i];
				if (our64_trace_verbose) fprintf(our64_trace, " [%u]", e);
				hashVertex(h, _p.vertices[e], _p.flatColors);
			}
		fprintf(our64_trace, "tris fb%u t%u,t%u mode%x n%u/%u key%016llx %08x\n", m_fb, m_bound[0], m_bound[1],
		        u32(_p.mode), _p.verticesCount, _p.elementsCount, keyOf(_p.combiner), h.h);
	}
	void drawRects(const Context::DrawRectParameters & _p) override {
		++our64_null_draws;
		if (!our64_trace) return;
		Hash h;
		hashState(h);
		for (u32 i = 0; i < _p.verticesCount; ++i) {
			const RectVertex & v = _p.vertices[i];
			h.f(v.x, 64); h.f(v.y, 64); h.f(v.z, 64); h.f(v.w, 64);
			h.f(v.s0, 32); h.f(v.t0, 32); h.f(v.s1, 32); h.f(v.t1, 32);
		}
		fprintf(our64_trace, "rect fb%u t%u,t%u %s n%u key%016llx %08x\n", m_fb, m_bound[0], m_bound[1],
		        _p.texrect ? "tex" : "fill", _p.verticesCount, keyOf(_p.combiner), h.h);
	}
	void drawLine(f32 _width, SPVertex * _vertices) override {
		++our64_null_draws;
		if (!our64_trace) return;
		Hash h;
		hashState(h);
		h.f(_width, 64); hashVertex(h, _vertices[0], false); hashVertex(h, _vertices[1], false);
		fprintf(our64_trace, "line fb%u %08x\n", m_fb, h.h);
	}
	f32 getMaxLineWidth() override { return 1.0f; }
	// The GX2 backend's feature set (gx2_ContextImpl.cpp isSupported).
	bool isSupported(SpecialFeatures _feature) const override {
		switch (_feature) {
		case SpecialFeatures::BlitFramebuffer:
		case SpecialFeatures::DualSourceBlending:
		case SpecialFeatures::DepthFramebufferTextures:
		case SpecialFeatures::TextureBarrier:
			return true;
		default:
			return false;
		}
	}
	s32 getMaxMSAALevel() override { return 0; }
	bool isError() const override { return false; }
	bool isFramebufferError() const override { return false; }
private:
	static unsigned long long keyOf(const CombinerProgram * _c) {
		return _c == nullptr ? 0 : static_cast<unsigned long long>(_c->getKey().getMux());
	}
	ClampMode m_clamp = ClampMode::ClippingEnabled;
	u32 m_fb = 0;
	u32 m_bound[8] = {};
	s32 m_unpack = 4;
	u32 m_names = 0;
};

} // namespace

unsigned our64_null_draws;
FILE * our64_trace;
bool our64_trace_verbose;

graphics::ContextImpl * our64_createNullContext()
{
	return new NullContextImpl;
}
