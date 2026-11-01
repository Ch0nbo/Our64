#-------------------------------------------------------------------------------
# Our64 stage-0 hardware probe (Aroma .wuhb)
#-------------------------------------------------------------------------------
.SUFFIXES:

ifeq ($(strip $(DEVKITPRO)),)
$(error "Please set DEVKITPRO in your environment. export DEVKITPRO=<path to>/devkitpro")
endif

TOPDIR ?= $(CURDIR)

APP_NAME	:= Our64 Probe
APP_SHORTNAME	:= Our64 Probe
APP_AUTHOR	:= Our64

include $(DEVKITPRO)/wut/share/wut_rules

TARGET		:=	our64-probe
BUILD		:=	build
SOURCES		:=	source
DATA		:=
INCLUDES	:=	source
CONTENT		:=
ICON		:=
TV_SPLASH	:=
DRC_SPLASH	:=

CAFEGLSL	:=	$(TOPDIR)/../third_party/cafeglsl

CFLAGS	:=	-g -Wall -O2 -ffunction-sections $(MACHDEP)
CFLAGS	+=	$(INCLUDE) -D__WIIU__ -D__WUT__
CXXFLAGS	:= $(CFLAGS) -std=gnu++17 -fno-exceptions -fno-rtti
ASFLAGS	:=	-g $(ARCH)
LDFLAGS	=	-g $(ARCH) $(RPXSPECS) -Wl,-Map,$(notdir $*.map)

LIBS	:= -lcafeglsl -lwut -lm -lstdc++

LIBDIRS	:= $(PORTLIBS) $(WUT_ROOT) $(CAFEGLSL)

ifneq ($(BUILD),$(notdir $(CURDIR)))

export OUTPUT	:=	$(CURDIR)/$(TARGET)
export TOPDIR	:=	$(CURDIR)
export VPATH	:=	$(foreach dir,$(SOURCES),$(CURDIR)/$(dir))
export DEPSDIR	:=	$(CURDIR)/$(BUILD)

CFILES		:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.c)))
CPPFILES	:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.cpp)))
SFILES		:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.s)))

export LD	:=	$(CXX)

export OFILES	:=	$(CPPFILES:.cpp=.o) $(CFILES:.c=.o) $(SFILES:.s=.o)
export INCLUDE	:=	$(foreach dir,$(INCLUDES),-I$(CURDIR)/$(dir)) \
			$(foreach dir,$(LIBDIRS),-I$(dir)/include) \
			-I$(CURDIR)/$(BUILD)
export LIBPATHS	:=	$(foreach dir,$(LIBDIRS),-L$(dir)/lib)

.PHONY: $(BUILD) clean all

all: $(BUILD)

$(BUILD):
	@[ -f $(CAFEGLSL)/lib/libcafeglsl.a ] || (echo "Run ../tools/fetch-cafeglsl.sh first" && false)
	@[ -d $@ ] || mkdir -p $@
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile

clean:
	@echo clean ...
	@rm -fr $(BUILD) $(TARGET).wuhb $(TARGET).rpx $(TARGET).elf

else
.PHONY: all

DEPENDS	:=	$(OFILES:.o=.d)

all	:	$(OUTPUT).wuhb

$(OUTPUT).wuhb	:	$(OUTPUT).rpx
$(OUTPUT).rpx	:	$(OUTPUT).elf
$(OUTPUT).elf	:	$(OFILES)

-include $(DEPENDS)

endif
