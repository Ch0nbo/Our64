#-------------------------------------------------------------------------------
# Our64 stage-1 headless CPU benchmark (Aroma .wuhb)
# Builds Not64's CPU core straight from the Not64 tree (one core, two platform
# layers) plus a Wii U platform layer in source/ and libogc stand-ins in shim/.
#-------------------------------------------------------------------------------
.SUFFIXES:

ifeq ($(strip $(DEVKITPRO)),)
$(error "Please set DEVKITPRO in your environment. export DEVKITPRO=<path to>/devkitpro")
endif

APP_NAME	:= Our64 CPU Bench
APP_SHORTNAME	:= Our64 Bench
APP_AUTHOR	:= Our64

include $(DEVKITPRO)/wut/share/wut_rules

# Not64, vendored (third_party/not64/VENDORED.md); NOT64= builds another tree.
NOT64		?=	$(CURDIR)/../third_party/not64
TARGET		:=	our64-bench
BUILD		:=	build

include $(CURDIR)/not64.mk

LOCAL_SRC	:=	$(wildcard source/*.c)

OFILES		:=	$(addprefix $(BUILD)/not64/,$(NOT64_SRC:.c=.o)) \
			$(addprefix $(BUILD)/,$(LOCAL_SRC:.c=.o))

# Same core switches as Not64's Wii build, minus GUI, audio thread and graphics.
DEFINES		:=	-D__WIIU__ -D__WUT__ -DHW_RVL -DNOASM -DUSE_EXPANSION \
			-DUSE_RECOMP_CACHE -DPPC_DYNAREC -DFASTMEM -DRELEASE -DPROFILE -DOUR64_COUNT_MEM

INCLUDE		:=	-I$(CURDIR)/shim -I$(CURDIR)/source -I$(NOT64) \
			$(foreach dir,$(PORTLIBS) $(WUT_ROOT),-I$(dir)/include)
LIBPATHS	:=	$(foreach dir,$(PORTLIBS) $(WUT_ROOT),-L$(dir)/lib)

CFLAGS		:=	-g -O2 -std=gnu11 -fgnu89-inline -ffunction-sections -fno-strict-aliasing \
			-Wall -Wno-unused-variable -Wno-unused-but-set-variable \
			-Wno-unused-function -Wno-pointer-sign -Wno-parentheses \
			-Wno-int-conversion -Wno-incompatible-pointer-types \
			-Wno-error=implicit-function-declaration \
			$(MACHDEP) $(DEFINES) $(INCLUDE)
CFLAGS		+=	$(EXTRA)
LDFLAGS		:=	-g $(MACHDEP) $(RPXSPECS) -Wl,-Map,$(BUILD)/$(TARGET).map
LIBS		:=	-lz -lwut -lm

export LD	:=	$(CC)

.PHONY: all clean

all: $(TARGET).wuhb

$(TARGET).wuhb: $(TARGET).rpx
$(TARGET).rpx: $(BUILD)/$(TARGET).elf
	@elf2rpl $< $@
	@echo built ... $@

$(BUILD)/$(TARGET).elf: $(OFILES)
	@echo linking ... $(notdir $@)
	@$(LD) $(LDFLAGS) $(OFILES) $(LIBPATHS) $(LIBS) -o $@

$(BUILD)/not64/%.o: $(NOT64)/%.c
	@mkdir -p $(dir $@)
	@echo $*.c
	@$(CC) -MMD -MP $(CFLAGS) -c $< -o $@

# wut's headers need C23; Not64's old-style prototypes need gnu11.
$(BUILD)/source/%.o: source/%.c
	@mkdir -p $(dir $@)
	@echo $*.c
	@$(CC) -MMD -MP $(CFLAGS) -std=gnu23 -c $< -o $@

clean:
	@rm -rf $(BUILD) $(TARGET).wuhb $(TARGET).rpx

-include $(OFILES:.o=.d)
