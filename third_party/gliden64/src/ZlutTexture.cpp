#include "Graphics/Context.h"
#include "Graphics/Parameters.h"
#include "DepthBuffer.h"
#include "Config.h"
#include "Textures.h"
#include "ZlutTexture.h"

using namespace graphics;

ZlutTexture g_zlutTexture;

// RE2's shaders read the N64 depth table whenever they write depth. The GX2
// backend has no integer textures for palettes, but it can hold this one: without
// it every RE2 character pixel got depth 0, so characters drew over each other
// in draw order (arms through chests; Zach, 2026-10-26).
static bool zlutNeeded()
{
	if (Context::IntegerTextures)
		return true;
#ifdef __WIIU__
	return (config.generalEmulation.hacks & hack_RE2) != 0;
#else
	return false;
#endif
}

ZlutTexture::ZlutTexture()
: m_pTexture(nullptr)
{
}

void ZlutTexture::init()
{
	if (!zlutNeeded())
		return;

	const FramebufferTextureFormats & fbTexFormats = gfxContext.getFramebufferTextureFormats();

	std::vector<u32> vecZLUT(0x40000);
	const u16 * const zLUT16 = depthBufferList().getZLUT();
	for (u32 i = 0; i < 0x40000; ++i)
		vecZLUT[i] = zLUT16[i];

	m_pTexture = textureCache().addFrameBufferTexture(textureTarget::TEXTURE_2D);
	m_pTexture->format = G_IM_FMT_IA;
	m_pTexture->clampS = 1;
	m_pTexture->clampT = 1;
	m_pTexture->frameBufferTexture = CachedTexture::fbOneSample;
	m_pTexture->maskS = 0;
	m_pTexture->maskT = 0;
	m_pTexture->mirrorS = 0;
	m_pTexture->mirrorT = 0;
	m_pTexture->width = 512;
	m_pTexture->height = 512;
	m_pTexture->textureBytes = m_pTexture->width * m_pTexture->height * fbTexFormats.lutFormatBytes;

	Context::InitTextureParams initParams;
	initParams.handle = m_pTexture->name;
	initParams.width = m_pTexture->width;
	initParams.height = m_pTexture->height;
	initParams.internalFormat = fbTexFormats.lutInternalFormat;
	initParams.format = fbTexFormats.lutFormat;
	initParams.dataType = fbTexFormats.lutType;
	initParams.data = vecZLUT.data();
	gfxContext.init2DTexture(initParams);

	Context::TexParameters setParams;
	setParams.handle = m_pTexture->name;
	setParams.target = textureTarget::TEXTURE_2D;
	setParams.textureUnitIndex = textureIndices::ZLUTTex;
	setParams.minFilter = textureParameters::FILTER_NEAREST;
	setParams.magFilter = textureParameters::FILTER_NEAREST;
	setParams.wrapS = textureParameters::WRAP_CLAMP_TO_EDGE;
	setParams.wrapT = textureParameters::WRAP_CLAMP_TO_EDGE;
	gfxContext.setTextureParameters(setParams);
}

void ZlutTexture::destroy()
{
	if (m_pTexture == nullptr)
		return;

	const FramebufferTextureFormats & fbTexFormats = gfxContext.getFramebufferTextureFormats();

	textureCache().removeFrameBufferTexture(m_pTexture);
	m_pTexture = nullptr;
}
