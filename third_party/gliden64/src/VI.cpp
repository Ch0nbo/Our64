#include "GLideN64.h"
#include "our64_at.h"
#include <math.h>
#include "Types.h"
#include "VI.h"
#include "N64.h"
#include "gSP.h"
#include "gDP.h"
#include "RSP.h"
#include "FrameBuffer.h"
#include "DepthBuffer.h"
#include "FrameBufferInfo.h"
#include "Config.h"
#include "Performance.h"
#include "Debugger.h"
#include "DebugDump.h"
#include "osal_keys.h"
#include "DisplayWindow.h"
#include "TextureFilterHandler.h"
#include "GLideNHQ/TxFilterExport.h"
#include <Graphics/Context.h>

using namespace std;

VIInfo VI;

u16 VI_GetMaxBufferHeight(u16 _width)
{
	if (_width > 320 || VI.interlaced)
		return VI.PAL ? 580 : 480;

	return VI.PAL ? 290 : 240;
}

volatile const char *our64_at_file = "";
volatile int our64_at_line;
volatile unsigned our64_vi_w, our64_vi_h;

void VI_UpdateSize()
{
	const f32 xScale = _FIXED2FLOAT( _SHIFTR( *REG.VI_X_SCALE, 0, 12 ), 10 );
//	f32 xOffset = _FIXED2FLOAT( _SHIFTR( *REG.VI_X_SCALE, 16, 12 ), 10 );

	const u32 vScale = _SHIFTR(*REG.VI_Y_SCALE, 0, 12);
//	f32 yOffset = _FIXED2FLOAT( _SHIFTR( *REG.VI_Y_SCALE, 16, 12 ), 10 );

//	const u32 hEnd = _SHIFTR( *REG.VI_H_START, 0, 10 );
//	const u32 hStart = _SHIFTR( *REG.VI_H_START, 16, 10 );

	VI.PAL = (*REG.VI_V_SYNC & 0x3ff) > 550;

	// These are in half-lines, so shift an extra bit
	const u32 vStart = _SHIFTR( *REG.VI_V_START, 16, 10 );
	u32 vEnd = _SHIFTR(*REG.VI_V_START, 0, 10);
	if (vEnd < vStart)
		vEnd = VI.PAL ? 44 + 576 : 34 + 480;

	const bool interlacedPrev = VI.interlaced;
	if (VI.width > 0)
		VI.widthPrev = VI.width;

	VI.real_height = vEnd > vStart ? (((vEnd - vStart) >> 1) * vScale) >> 10 : 0;
	VI.width = *REG.VI_WIDTH;
	VI.interlaced = (*REG.VI_STATUS & VI_STATUS_SERRATE_ENABLED) != 0;

	if (VI.interlaced) {
		f32 fullWidth = 640.0f;
		if ((*REG.VI_X_SCALE) % 512 == 0)
			fullWidth *= xScale;
		if (*REG.VI_WIDTH > fullWidth) {
			const u32 scale = (u32)floorf(*REG.VI_WIDTH / fullWidth + 0.5f);
			VI.width /= scale;
			VI.real_height *= scale;
		}
		if (VI.real_height % 2 == 1)
			--VI.real_height;
	} //else if (hEnd != 0 && *REG.VI_WIDTH != 0)
		//VI.width = min((u32)floorf((hEnd - hStart)*xScale + 0.5f), *REG.VI_WIDTH);

	if (VI.PAL && (vEnd - vStart) > 478) {
		VI.height = (u32)(VI.real_height*1.0041841f);
		if (VI.height > 576)
			VI.height = VI.real_height = 576;
	} else {
		VI.height = (u32)(VI.real_height*1.0126582f);
		if (VI.height > 480)
			VI.height = VI.real_height = 480;
	}
	if (VI.height % 2 == 1)
		--VI.height;

//	const int fsaa = ((*REG.VI_STATUS) >> 8) & 3;
//	const int divot = ((*REG.VI_STATUS) >> 4) & 1;
	FrameBufferList & fbList = frameBufferList();
	FrameBuffer * pBuffer = fbList.findBuffer(VI.lastOrigin & 0xffffff);
	DepthBuffer * pDepthBuffer = pBuffer != nullptr ? pBuffer->m_pDepthBuffer : nullptr;
	if (config.frameBufferEmulation.enable &&
		((interlacedPrev != VI.interlaced) ||
		(VI.width > 0 && VI.width != VI.widthPrev) ||
		(!VI.interlaced && pDepthBuffer != nullptr && pDepthBuffer->m_width != VI.width))) {
		fbList.removeBuffers(VI.widthPrev);
		// Our64: Resident Evil 2 draws a room's first frame at the new width
		// before the VI shows it: removing the new width's buffers threw that
		// frame away, and the screen came from RDRAM, the background's source
		// data, a wall of green and blue (results/frames-re2-v32201, 2026-10-26).
		if ((config.generalEmulation.hacks & hack_RE2) == 0)
			fbList.removeBuffers(VI.width);
		depthBufferList().destroy();
		depthBufferList().init();
	}

	VI.rwidth = VI.width != 0 ? 1.0f / VI.width : 0.0f;
	VI.rheight = VI.height != 0 ? 1.0f / VI.height : 0.0f;
	our64_vi_w = VI.width;
	our64_vi_h = VI.height;
}

static void checkHotkeys()
{
	osal_keys_update_state();

	if (osal_is_key_pressed(KEY_G, 0x0001)) {
		SwitchDump(config.debug.dumpMode);
	}

	if (osal_is_key_pressed(config.hotkeys.enabledKeys[Config::hkHdTexToggle], 0x0001)) {
		if (config.textureFilter.txHiresEnable == 0)
			dwnd().getDrawer().showMessage("Enable HD textures\n", Milliseconds(750));
		else
			dwnd().getDrawer().showMessage("Disable HD textures\n", Milliseconds(750));
		config.textureFilter.txHiresEnable = !config.textureFilter.txHiresEnable;
		textureCache().clear();
	}

	if (config.textureFilter.txHiresEnable != 0) {
		/* Force reload hi-res textures. Useful for texture artists */
		if (osal_is_key_pressed(config.hotkeys.enabledKeys[Config::hkHdTexReload], 0x0001)) {
			dwnd().getDrawer().showMessage("Reload HD textures\n", Milliseconds(750));
			if (txfilter_reloadhirestex()) {
				textureCache().clear();
			}
		}

		/* Turn on texture dump */
		if (osal_is_key_pressed(config.hotkeys.enabledKeys[Config::hkTexDump], 0x0001))
			textureCache().toggleDumpTex();

		if (osal_is_key_pressed(config.hotkeys.enabledKeys[Config::hkStrongCRC], 0x0001)) {
			if (config.textureFilter.txStrongCRC == 0)
				dwnd().getDrawer().showMessage("Enable strong CRC for textures dump\n", Milliseconds(750));
			else
				dwnd().getDrawer().showMessage("Disable strong CRC for textures dump\n", Milliseconds(750));
			config.textureFilter.txStrongCRC = !config.textureFilter.txStrongCRC;
		}
	}

	if (osal_is_key_pressed(config.hotkeys.enabledKeys[Config::hkTexCoordBounds], 0x0001)) {
		if (config.graphics2D.enableTexCoordBounds == 0)
			dwnd().getDrawer().showMessage("Bound texrect texture coordinates on\n", Milliseconds(1000));
		else
			dwnd().getDrawer().showMessage("Bound texrect texture coordinates off\n", Milliseconds(1000));
		config.graphics2D.enableTexCoordBounds = !config.graphics2D.enableTexCoordBounds;
	}

	if (osal_is_key_pressed(config.hotkeys.enabledKeys[Config::hkNativeResTexrects], 0x0001)) {
		static u32 s_nativeResTexrects = Config::NativeResTexrectsMode::ntOptimized;
		if (config.graphics2D.enableNativeResTexrects != Config::NativeResTexrectsMode::ntDisable) {
			s_nativeResTexrects = config.graphics2D.enableNativeResTexrects;
			config.graphics2D.enableNativeResTexrects = Config::NativeResTexrectsMode::ntDisable;
		}
		else
			config.graphics2D.enableNativeResTexrects = s_nativeResTexrects;
		if (config.graphics2D.enableNativeResTexrects == Config::NativeResTexrectsMode::ntDisable)
			dwnd().getDrawer().showMessage("Disable 2D texrects in native resolution\n", Milliseconds(1000));
		else
			dwnd().getDrawer().showMessage("Enable 2D texrects in native resolution\n", Milliseconds(1000));
	}

	if (osal_is_key_pressed(config.hotkeys.enabledKeys[Config::hkVsync], 0x0001)) {
		config.video.verticalSync = !config.video.verticalSync;
		dwnd().stop();
		dwnd().start();
		if (config.video.verticalSync == 0)
			dwnd().getDrawer().showMessage("Disable vertical sync\n", Milliseconds(1000));
		else
			dwnd().getDrawer().showMessage("Enable vertical sync\n", Milliseconds(1000));
	}


	if (osal_is_key_pressed(config.hotkeys.enabledKeys[Config::hkFBEmulation], 0x0001)) {
		config.frameBufferEmulation.enable = !config.frameBufferEmulation.enable;
		dwnd().stop();
		dwnd().start();
		if (config.frameBufferEmulation.enable == 0)
			dwnd().getDrawer().showMessage("Disable frame buffer emulation\n", Milliseconds(2000));
		else
			dwnd().getDrawer().showMessage("Enable frame buffer emulation\n", Milliseconds(1000));
	}

	if (config.frameBufferEmulation.enable != 0 &&
		osal_is_key_pressed(config.hotkeys.enabledKeys[Config::hkN64DepthCompare], 0x0001)) {
		static u32 N64DepthCompare = Config::N64DepthCompareMode::dcCompatible;
		if (config.frameBufferEmulation.N64DepthCompare != Config::N64DepthCompareMode::dcDisable) {
			N64DepthCompare = config.frameBufferEmulation.N64DepthCompare;
			config.frameBufferEmulation.N64DepthCompare = Config::N64DepthCompareMode::dcDisable;
		} else
			config.frameBufferEmulation.N64DepthCompare = N64DepthCompare;
		dwnd().stop();
		dwnd().start();
		if (config.frameBufferEmulation.N64DepthCompare == Config::N64DepthCompareMode::dcDisable)
			dwnd().getDrawer().showMessage("Disable N64 depth compare\n", Milliseconds(1000));
		else
			dwnd().getDrawer().showMessage("Enable N64 depth compare\n", Milliseconds(1000));
	}

	if (osal_is_key_pressed(config.hotkeys.enabledKeys[Config::hkOsdVis], 0x0001)) {
		config.onScreenDisplay.vis = !config.onScreenDisplay.vis;
	}

	if (osal_is_key_pressed(config.hotkeys.enabledKeys[Config::hkOsdFps], 0x0001)) {
		config.onScreenDisplay.fps = !config.onScreenDisplay.fps;
	}

	if (osal_is_key_pressed(config.hotkeys.enabledKeys[Config::hkOsdPercent], 0x0001)) {
		config.onScreenDisplay.percent = !config.onScreenDisplay.percent;
	}

	if (osal_is_key_pressed(config.hotkeys.enabledKeys[Config::hkOsdInternalResolution], 0x0001)) {
		config.onScreenDisplay.internalResolution = !config.onScreenDisplay.internalResolution;
	}

	if (osal_is_key_pressed(config.hotkeys.enabledKeys[Config::hkOsdRenderingResolution], 0x0001)) {
		config.onScreenDisplay.renderingResolution = !config.onScreenDisplay.renderingResolution;
	}

	if (osal_is_key_pressed(config.hotkeys.enabledKeys[Config::hkForceGammaCorrection], 0x0001)) {
		if (config.gammaCorrection.force == 0)
			dwnd().getDrawer().showMessage("Force gamma correction on\n", Milliseconds(750));
		else
			dwnd().getDrawer().showMessage("Force gamma correction off\n", Milliseconds(750));
		config.gammaCorrection.force = !config.gammaCorrection.force;
	}

	if (osal_is_key_pressed(config.hotkeys.enabledKeys[Config::hkInaccurateTexCords], 0x0001)) {
		config.generalEmulation.enableInaccurateTextureCoordinates = !config.generalEmulation.enableInaccurateTextureCoordinates;
		dwnd().stop();
		dwnd().start();
		if (config.generalEmulation.enableInaccurateTextureCoordinates == 0)
			dwnd().getDrawer().showMessage("Disable inaccurate texture coordinates\n", Milliseconds(1000));
		else
			dwnd().getDrawer().showMessage("Enable inaccurate texture coordinates\n", Milliseconds(1000));
	}
}

// Our64: for the player's log, VIs that kept the last frame (Resident Evil 2's
// width change, below) and screens taken from RDRAM (no buffer at the origin).
extern "C" { unsigned our64_vi_holds, our64_vi_from_rdram, our64_vi_found_start, our64_vi_found_width, our64_vi_found_valid,
	our64_vi_cfb, our64_vi_swaps; extern unsigned our64_cfb_calls, our64_cfb_nobuffer, our64_cfb_noheight, our64_cfb_zero,
	our64_cfb_drawn, our64_cfb_alpha, our64_dlists, our64_rdplists; }

void VI_UpdateScreen()
{
	if (VI.lastOrigin == -1) // Workaround for Mupen64Plus issue with initialization
		gfxContext.isError();

	DebugMsg(DEBUG_DETAIL, "VI_UpdateScreen Origin: %08x, Old origin: %08x, width: %d\n", *REG.VI_ORIGIN, VI.lastOrigin, *REG.VI_WIDTH);

	if (ConfigOpen)
		return;

	OUR64_AT();
	perf.increaseVICount();
	DisplayWindow & wnd = dwnd();
	if (wnd.changeWindow())
		return;
	if (wnd.resizeWindow())
		return;
	wnd.saveScreenshot();
	g_debugger.checkDebugState();

	checkHotkeys();
	OUR64_AT();

	bool bVIUpdated = false;
	if (*REG.VI_ORIGIN != VI.lastOrigin) {
		VI_UpdateSize();
		bVIUpdated = true;
		wnd.updateScale();
		perf.increaseFramesCount();
	}

	if (config.frameBufferEmulation.enable) {

		OUR64_AT();
		FrameBuffer * pBuffer = frameBufferList().findBuffer(*REG.VI_ORIGIN & 0xffffff);
		our64_vi_found_start = pBuffer ? pBuffer->m_startAddress : 0;
		our64_vi_found_width = pBuffer ? pBuffer->m_width : 0;
		our64_vi_found_valid = pBuffer ? (unsigned)pBuffer->isValid(true) : 2;
		{ static const bool our64Log = getenv("OUR64_ADDRLOG") != nullptr;
		  if (our64Log) fprintf(stderr, "VI origin %06X width %u: buffer %06X w %u valid %d\n", *REG.VI_ORIGIN & 0xffffff, *REG.VI_WIDTH,
			pBuffer ? pBuffer->m_startAddress : 0, pBuffer ? pBuffer->m_width : 0, pBuffer ? (int)pBuffer->isValid(true) : -1);
		  if (our64Log && pBuffer == nullptr) frameBufferList().our64DumpList(); }
		// Our64: Resident Evil 2 changes its 16-bit screen's width with each room
		// (448 to 436 wide, for one). hack_RE2 reads the 512-wide color images as
		// VI_WIDTH wide, which on the new room's first frame still holds the old
		// width: that frame's background goes to a buffer of the old width, thrown
		// away when the next color image has the new one. The first VIs of the new
		// width then find no buffer and showed RDRAM, which holds the background's
		// source data there: a wall of green and blue (results/frames-re2-v32201,
		// 36914-36917; 2026-10-26). For 30 VIs after a width change, a 16-bit
		// screen with no buffer keeps the last frame shown instead.
		if ((config.generalEmulation.hacks & hack_RE2) != 0) {
			static u32 viCount, lastWidth, widthChangedAt;
			++viCount;
			if (*REG.VI_WIDTH != lastWidth) {
				lastWidth = *REG.VI_WIDTH;
				widthChangedAt = viCount;
			}
			if (pBuffer == nullptr && (*REG.VI_STATUS & VI_STATUS_TYPE_32) == VI_STATUS_TYPE_16 &&
				viCount - widthChangedAt < 30) {
				VI.lastOrigin = *REG.VI_ORIGIN;
				++our64_vi_holds;
				return;
			}
		}
		if (pBuffer == nullptr) {
			++our64_vi_from_rdram;
			gDP.changed |= CHANGED_CPU_FB_WRITE;
		} else if (!FBInfo::fbInfo.isSupported() &&
				 // Our64: a screen taken from RDRAM is read again when RDRAM
				 // changed, Resident Evil 2's too: its door animations (264 wide,
				 // 32-bit) are decoded into RDRAM, and the first frame was taken
				 // before the decode wrote it, so the source data stayed up for
				 // the whole animation, a wall of green and blue (2026-10-26).
				 ((config.generalEmulation.hacks & hack_RE2) == 0 || pBuffer->m_cfb) &&
				 !pBuffer->isValid(true)) {
			gDP.changed |= CHANGED_CPU_FB_WRITE;
			if (config.frameBufferEmulation.copyToRDRAM == 0 && (config.generalEmulation.hacks & hack_subscreen) == 0) {
				OUR64_AT();
				pBuffer->copyRdram();
			}
		}

		const bool bCFB = (gDP.changed&CHANGED_CPU_FB_WRITE) == CHANGED_CPU_FB_WRITE;
		our64_vi_cfb += bCFB;
		{ static const bool our64Cfb = getenv("OUR64_CFBLOG") != nullptr; static u32 n;
		  if (our64Cfb && ++n % 600 == 0)
			fprintf(stderr, "SCREEN: %u display lists, %u RDP lists; %u CPU-written; copies %u: no buffer %u, no height %u, all zero %u, drawn %u (%u with alpha)\n",
				our64_dlists, our64_rdplists, our64_vi_cfb, our64_cfb_calls, our64_cfb_nobuffer, our64_cfb_noheight, our64_cfb_zero, our64_cfb_drawn, our64_cfb_alpha); }
		bool bNeedSwap = false;
		switch (config.frameBufferEmulation.bufferSwapMode) {
		case Config::bsOnVerticalInterrupt:
			bNeedSwap = true;
			break;
		case Config::bsOnVIOriginChange:
			bNeedSwap = bCFB ? true : (*REG.VI_ORIGIN != VI.lastOrigin);
			break;
		case Config::bsOnColorImageChange:
			bNeedSwap = bCFB ? true : (gDP.colorImage.changed != 0);
			break;
		}

		if (bNeedSwap) {
			++our64_vi_swaps;
			if (bCFB) {
				if (pBuffer == nullptr || pBuffer->m_width != VI.width) {
					if (!bVIUpdated) {
						VI_UpdateSize();
						wnd.updateScale();
						bVIUpdated = true;
					}
					const u32 size = *REG.VI_STATUS & VI_STATUS_TYPE_32;
					if (VI.height > 0 && size > G_IM_SIZ_8b  && VI.width > 0)
						(OUR64_AT(), frameBufferList().saveBuffer(*REG.VI_ORIGIN & 0xffffff, G_IM_FMT_RGBA, size, VI.width, true));
				}
			}
//			if ((((*REG.VI_STATUS) & 3) > 0) && (gDP.colorImage.changed || bCFB)) { // Does not work in release build!!!
			if (((*REG.VI_STATUS) & VI_STATUS_TYPE_32) > 0) {
				if (!bVIUpdated) {
					VI_UpdateSize();
					bVIUpdated = true;
				}
				OUR64_AT();
				FrameBuffer_CopyFromRDRAM(*REG.VI_ORIGIN & 0xffffff, bCFB);
			}
			OUR64_AT();
			frameBufferList().renderBuffer();
			OUR64_AT();
			frameBufferList().clearBuffersChanged();
			VI.lastOrigin = *REG.VI_ORIGIN;
		}
	} else {
		bool bNeedRender = false;
		switch (config.frameBufferEmulation.bufferSwapMode) {
		case Config::bsOnVerticalInterrupt:
			bNeedRender = true;
			break;
		case Config::bsOnVIOriginChange:
			bNeedRender = *REG.VI_ORIGIN != VI.lastOrigin;
			break;
		case Config::bsOnColorImageChange:
			bNeedRender = (gDP.changed & CHANGED_COLORBUFFER) != 0;
			break;
		}
		if (bNeedRender)
			frameBufferList().renderBuffer();

		gDP.changed &= ~CHANGED_COLORBUFFER;
		VI.lastOrigin = *REG.VI_ORIGIN;
	}

	if (VI.lastOrigin == -1) { // Workaround for Mupen64Plus issue with initialization
		gfxContext.clearColorBuffer(0.0f, 0.0f, 0.0f, 0.0f);
	}
}
