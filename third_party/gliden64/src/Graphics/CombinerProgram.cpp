#include <vector>
#include "CombinerProgram.h"
#include <Config.h>

namespace graphics {

	u32 CombinerProgram::getShaderCombinerOptionsBits()
	{
		// WARNING: Shader Storage format version must be increased after any change in this function.
		std::vector<u32> vecOptions;
		vecOptions.push_back(config.video.multisampling > 0 ? 1 : 0);
		vecOptions.push_back(config.texture.bilinearMode);
		vecOptions.push_back(config.texture.enableHalosRemoval);
		vecOptions.push_back(config.generalEmulation.enableHWLighting);
		vecOptions.push_back(config.generalEmulation.enableHiresNoiseDithering);
		vecOptions.push_back(config.generalEmulation.enableDitheringPattern);
		vecOptions.push_back(config.generalEmulation.enableDitheringQuantization);
		vecOptions.push_back(config.generalEmulation.enableLOD);
		vecOptions.push_back(config.generalEmulation.enableCoverage);
		vecOptions.push_back(config.frameBufferEmulation.N64DepthCompare == Config::dcFast ? 1 : 0);
		vecOptions.push_back(config.frameBufferEmulation.N64DepthCompare == Config::dcCompatible ? 1 : 0);
		vecOptions.push_back(config.generalEmulation.enableLegacyBlending);
		vecOptions.push_back(config.generalEmulation.enableFragmentDepthWrite);
		vecOptions.push_back(config.generalEmulation.enableInaccurateTextureCoordinates);
		// Our64: changes the simple shaders' text, for Fighting Force only; every
		// other game keeps the same bits, so the storage version stays.
		vecOptions.push_back((config.generalEmulation.hacks & hack_fastLodSharpen) != 0 ? 1 : 0);
		u32 optionsSet = 0;
		for (u32 i = 0; i < vecOptions.size(); ++i)
			optionsSet |= vecOptions[i] << i;
		return optionsSet;
	}

}
