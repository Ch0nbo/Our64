# Our64 shader-dump (PC): the shader pair the console compiles for a GLideN64
# combiner key, and CafeGLSL's R600 disassembly of it. See shader_dump.cpp.
# CAFEGLSL is the Linux x86-64 release of CafeGLSL (the same version as
# third_party/cafeglsl): https://github.com/Exzap/CafeGLSL/releases
include ../../replay/gliden64.mk
CAFEGLSL ?= $(error set CAFEGLSL to the unpacked cafeglsl-<version>-linux-x86_64 directory)
BUILD := build
GLSL := $(addprefix Graphics/OpenGLContext/GLSL/,glsl_CombinerProgramBuilder.cpp \
	glsl_CombinerProgramBuilderAccurate.cpp glsl_CombinerProgramBuilderCommon.cpp \
	glsl_CombinerProgramBuilderFast.cpp glsl_CombinerProgramImpl.cpp \
	glsl_CombinerProgramUniformFactory.cpp glsl_CombinerProgramUniformFactoryAccurate.cpp \
	glsl_CombinerProgramUniformFactoryCommon.cpp glsl_CombinerProgramUniformFactoryFast.cpp \
	glsl_Utils.cpp)
# The core as the null backend builds it (Combiner.cpp comes in through
# shader_dump_combiner.cpp), plus the GLSL generators, with the GX2 shim
# headers first as the console build has them.
SRCS := $(filter-out Combiner.cpp,$(GLIDEN64_CORE)) Graphics/NullContext/null_DisplayWindow.cpp $(GLSL)
INCLUDE := -I$(G)/Graphics/GX2Context/shim $(GLIDEN64_INCLUDE) -I$(CAFEGLSL)/include
CXXFLAGS := -O1 -g -std=c++17 -w -DOS_LINUX -DOUR64_NULL_GFX $(GLIDEN64_DEFINES) $(INCLUDE)
CFLAGS := -O1 -g -w -DOS_LINUX $(GLIDEN64_INCLUDE)
OBJS := $(addprefix $(BUILD)/,$(SRCS:.cpp=.o) $(GLIDEN64_C:.c=.o))

shader-dump: $(OBJS) $(BUILD)/shader_dump.o $(BUILD)/shader_dump_combiner.o
	$(CXX) -o $@ $^ $(CAFEGLSL)/lib/libcafeglsl.a -lpthread -ldl -lz
$(BUILD)/shader_dump.o $(BUILD)/shader_dump_combiner.o: $(BUILD)/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) -MMD -MP $(CXXFLAGS) -I$(G) -c $< -o $@
$(BUILD)/%.o: $(G)/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) -MMD -MP $(CXXFLAGS) -c $< -o $@
$(BUILD)/%.o: $(G)/%.c
	@mkdir -p $(dir $@)
	$(CC) -MMD -MP $(CFLAGS) -c $< -o $@
clean:
	rm -rf $(BUILD) shader-dump
-include $(OBJS:.o=.d)
