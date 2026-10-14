# adaptive_triggers: background sysmodule that watches the running game and drives
# DualSense adaptive triggers through MissionControl's mc service.
ifeq ($(strip $(DEVKITPRO)),)
$(error "Please set DEVKITPRO in your environment. export DEVKITPRO=<path to>/devkitpro")
endif

TOPDIR ?= $(CURDIR)
include $(DEVKITPRO)/libnx/switch_rules

TARGET   := adaptive_triggers
BUILD    := build
SOURCES  := source
APP_JSON := adaptive_triggers.json
PROGRAM_ID := 42000000000d5e00

ARCH     := -march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE
CFLAGS   := -g -Wall -Wextra -O2 -ffunction-sections $(ARCH) $(INCLUDE) -D__SWITCH__
ASFLAGS  := -g $(ARCH)
LDFLAGS  = -specs=$(DEVKITPRO)/libnx/switch.specs -g $(ARCH) -Wl,-Map,$(notdir $*.map)
LIBS     := -lnx
LIBDIRS  := $(PORTLIBS) $(LIBNX)

ifneq ($(BUILD),$(notdir $(CURDIR)))
export OUTPUT   := $(CURDIR)/$(TARGET)
export TOPDIR   := $(CURDIR)
export VPATH    := $(foreach dir,$(SOURCES),$(CURDIR)/$(dir))
export DEPSDIR  := $(CURDIR)/$(BUILD)
CFILES          := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.c)))
export LD       := $(CC)
export OFILES   := $(CFILES:.c=.o)
export INCLUDE  := $(foreach dir,$(LIBDIRS),-I$(dir)/include) -I$(CURDIR)/$(BUILD)
export LIBPATHS := $(foreach dir,$(LIBDIRS),-L$(dir)/lib)
export APP_JSON := $(TOPDIR)/$(APP_JSON)

.PHONY: $(BUILD) clean all dist
all: $(BUILD)
$(BUILD):
	@mkdir -p $@
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile

# SD card layout: atmosphere/contents/<id>/exefs.nsp + boot2 flag
dist: all
	@rm -rf dist
	@mkdir -p dist/atmosphere/contents/$(PROGRAM_ID)/flags dist/config/adaptive_triggers
	@cp $(TARGET).nsp dist/atmosphere/contents/$(PROGRAM_ID)/exefs.nsp
	@touch dist/atmosphere/contents/$(PROGRAM_ID)/flags/boot2.flag
	@echo "dist ready"

clean:
	@rm -fr $(BUILD) dist $(TARGET).nsp $(TARGET).nso $(TARGET).npdm $(TARGET).elf
else
DEPENDS := $(OFILES:.o=.d)
all: $(OUTPUT).nsp
$(OUTPUT).nsp: $(OUTPUT).nso $(OUTPUT).npdm
$(OUTPUT).nso: $(OUTPUT).elf
$(OUTPUT).elf: $(OFILES)
-include $(DEPENDS)
endif
