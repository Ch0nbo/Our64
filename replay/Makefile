# Our64 stage-2 replay app for the console: GLideN64 with the null backend,
# playing the frames the bench captured. See source/main.cpp.
.SUFFIXES:
ifeq ($(strip $(DEVKITPRO)),)
$(error "Please set DEVKITPRO in your environment. export DEVKITPRO=<path to>/devkitpro")
endif

GFX		?=	null
ifeq ($(GFX),gx2)
# The GX2 flavor renders on the GPU (stage 3) and saves each frame as an image.
APP_NAME	:= Our64 GX2 Replay
APP_SHORTNAME	:= Our64 GX2 Replay
TARGET		:=	our64-replay-gx2
BUILD		:=	build-gx2
CAFEGLSL	:=	$(CURDIR)/../third_party/cafeglsl
GFX_INCLUDE	:=	-I$(CAFEGLSL)/include
GFX_LIBS	:=	-L$(CAFEGLSL)/lib -lcafeglsl
GFX_DEFINES	:=	-DREPORT_NO_CONSOLE
# The player's TV-only shader pass (gx2-tvfx.txt in source/main.cpp).
GFX_OFILES	:=	$(BUILD)/player/tv_pass.o
APP_ICON	:=	$(CURDIR)/meta/icon-gx2.png
else
APP_NAME	:= Our64 Replay
APP_SHORTNAME	:= Our64 Replay
TARGET		:=	our64-replay
BUILD		:=	build
endif
APP_AUTHOR	:= Our64

include $(DEVKITPRO)/wut/share/wut_rules
include gliden64.mk
BENCH		:=	$(CURDIR)/../bench/source

OFILES		:=	$(addprefix $(BUILD)/gliden64/,$(GLIDEN64_CORE:.cpp=.o) $(GLIDEN64_C:.c=.o)) \
			$(BUILD)/source/main.o $(BUILD)/source/core_wiiu.o $(BUILD)/source/profile.o $(BUILD)/bench/report.o \
			$(GFX_OFILES)

DEFINES		:=	-D__WIIU__ -D__WUT__ -DOS_LINUX $(GLIDEN64_DEFINES) $(GFX_DEFINES)
INCLUDE		:=	$(GLIDEN64_INCLUDE) $(GFX_INCLUDE) -I$(BENCH) $(foreach dir,$(PORTLIBS) $(WUT_ROOT),-I$(dir)/include)
LIBPATHS	:=	$(foreach dir,$(PORTLIBS) $(WUT_ROOT),-L$(dir)/lib)
# -ffp-contract=off: no fused multiply-add, so float results match the PC build.
COMMON		:=	-g -O2 -ffunction-sections -ffp-contract=off $(MACHDEP) $(DEFINES) $(INCLUDE)
CFLAGS		:=	$(COMMON) -std=gnu11 -w -DREPORT_FILE='"replay-results.txt"'
CXXFLAGS	:=	$(COMMON) -std=gnu++17 -w
LDFLAGS		:=	-g $(MACHDEP) $(RPXSPECS) -Wl,-Map,$(BUILD)/$(TARGET).map
LIBS		:=	$(GFX_LIBS) -lz -lwut -lm -lstdc++
export LD	:=	$(CXX)

.PHONY: all clean
all: $(TARGET).wuhb

$(TARGET).wuhb: $(TARGET).rpx
$(TARGET).rpx: $(BUILD)/$(TARGET).elf
	@elf2rpl $< $@
	@echo built ... $@

$(BUILD)/$(TARGET).elf: $(OFILES)
	@echo linking ... $(notdir $@)
	@$(LD) $(LDFLAGS) $(OFILES) $(LIBPATHS) $(LIBS) -o $@

$(BUILD)/gliden64/%.o: $(G)/%.cpp
	@mkdir -p $(dir $@)
	@echo $*.cpp
	@$(CXX) -MMD -MP $(CXXFLAGS) -c $< -o $@
$(BUILD)/gliden64/%.o: $(G)/%.c
	@mkdir -p $(dir $@)
	@echo $*.c
	@$(CC) -MMD -MP $(CFLAGS) -c $< -o $@
$(BUILD)/source/%.o: source/%.cpp
	@mkdir -p $(dir $@)
	@echo $*.cpp
	@$(CXX) -MMD -MP $(CXXFLAGS) -Wall -c $< -o $@
$(BUILD)/player/%.o: ../player/source/%.cpp
	@mkdir -p $(dir $@)
	@echo $*.cpp
	@$(CXX) -MMD -MP $(CXXFLAGS) -Wall -c $< -o $@
$(BUILD)/bench/%.o: $(BENCH)/%.c
	@mkdir -p $(dir $@)
	@echo $*.c
	@$(CC) -MMD -MP $(CFLAGS) -c $< -o $@

clean:
	@rm -rf $(BUILD) $(TARGET).wuhb $(TARGET).rpx

-include $(OFILES:.o=.d)
