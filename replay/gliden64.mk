# GLideN64 sources for Our64 builds with the null backend (no OpenGL), shared
# by the PC plugin build (Makefile.pc) and the console replay app (Makefile).
# CRC_OPT.cpp hashes textures with XXH3 (portable C); texture hashes are only
# cache keys, and the microcode CRC stays the exact CRC32.
# GLideN64, vendored (third_party/gliden64/VENDORED.md); GLIDEN64= builds another tree.
GLIDEN64 ?= $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/../third_party/gliden64)
G := $(GLIDEN64)/src

GLIDEN64_CORE := 3DMath.cpp Combiner.cpp CombinerKey.cpp CommonPluginAPI.cpp Config.cpp \
	convert.cpp CRC_OPT.cpp DebugDump.cpp Debugger.cpp DepthBuffer.cpp DisplayWindow.cpp \
	DisplayLoadProgress.cpp FrameBuffer.cpp FrameBufferInfo.cpp GBI.cpp gDP.cpp GLideN64.cpp \
	GraphicsDrawer.cpp gSP.cpp Log.cpp N64.cpp PaletteTexture.cpp Performance.cpp \
	PostProcessor.cpp RDP.cpp RSP.cpp RSP_LoadMatrix.cpp SoftwareRender.cpp TexrectDrawer.cpp \
	TextDrawerStub.cpp TextureFilterHandler.cpp Textures.cpp VI.cpp ZlutTexture.cpp \
	TxFilterStub.cpp \
	BufferCopy/BlueNoiseTexture.cpp BufferCopy/ColorBufferToRDRAM.cpp \
	BufferCopy/DepthBufferToRDRAM.cpp BufferCopy/RDRAMtoColorBuffer.cpp \
	DepthBufferRender/ClipPolygon.cpp DepthBufferRender/DepthBufferRender.cpp \
	common/CommonAPIImpl_common.cpp \
	Graphics/Context.cpp Graphics/ColorBufferReader.cpp Graphics/CombinerProgram.cpp \
	Graphics/ObjectHandle.cpp \
	Graphics/OpenGLContext/GLSL/glsl_CombinerInputs.cpp \
	Graphics/NullContext/null_ContextImpl.cpp Graphics/NullContext/null_Parameters.cpp \
	MupenPlusPluginAPI.cpp mupenplus/CommonAPIImpl_mupenplus.cpp \
	mupenplus/MemoryStatus_mupenplus.cpp mupenplus/MupenPlusAPIImpl.cpp \
	mupenplus/Config_mupenplus.cpp \
	$(addprefix uCodes/,F3D.cpp F3DBETA.cpp F3DDKR.cpp F3DEX.cpp F3DEX095.cpp F3DAM.cpp \
	F3DEX2.cpp F3DEX3.cpp F3DEX2ACCLAIM.cpp F3DEX2CBFD.cpp F3DZEX2.cpp F3DFLX2.cpp \
	F3DGOLDEN.cpp F3DTEXA.cpp F3DPD.cpp F3DSETA.cpp F5Indi_Naboo.cpp F5Rogue.cpp L3D.cpp \
	L3DEX2.cpp L3DEX.cpp S2DEX2.cpp S2DEX.cpp T3DUX.cpp Turbo3D.cpp ZSort.cpp ZSortBOSS.cpp)
GLIDEN64_C := osal/osal_files_unix.c osal/osal_keys_unix.c

GLIDEN64_DEFINES := -DMUPENPLUSAPI -DTXFILTER_LIB -DOUR64_CMD_TIMING -DNDEBUG
GLIDEN64_INCLUDE := -I$(G) -I$(G)/inc -I$(G)/osal -I$(abspath $(dir $(lastword $(MAKEFILE_LIST))))/inc

# Graphics backend: GFX=null (no output, the default) or GFX=gx2 (Wii U GPU,
# console only). The null backend's trace globals are linked in both.
GFX ?= null
ifeq ($(GFX),gx2)
GLIDEN64_CORE += $(addprefix Graphics/OpenGLContext/GLSL/,glsl_CombinerProgramBuilder.cpp \
	glsl_CombinerProgramBuilderAccurate.cpp glsl_CombinerProgramBuilderCommon.cpp \
	glsl_CombinerProgramBuilderFast.cpp glsl_CombinerProgramImpl.cpp \
	glsl_CombinerProgramUniformFactory.cpp glsl_CombinerProgramUniformFactoryAccurate.cpp \
	glsl_CombinerProgramUniformFactoryCommon.cpp glsl_CombinerProgramUniformFactoryFast.cpp \
	glsl_FXAA.cpp glsl_SpecialShadersFactory.cpp glsl_Utils.cpp) \
	Graphics/GX2Context/gx2_ContextImpl.cpp Graphics/GX2Context/gx2_DisplayWindow.cpp \
	Graphics/GX2Context/gx2_GL.cpp Graphics/GX2Context/gx2_ShaderCache.cpp
GLIDEN64_DEFINES += -DOUR64_GX2_GFX
# The shim headers stand in for the OpenGL ones, so they come first.
GLIDEN64_INCLUDE := -I$(G)/Graphics/GX2Context/shim $(GLIDEN64_INCLUDE)
else
GLIDEN64_CORE += Graphics/NullContext/null_DisplayWindow.cpp
GLIDEN64_DEFINES += -DOUR64_NULL_GFX
endif
