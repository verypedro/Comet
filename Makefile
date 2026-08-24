#-------------------------------------------------------------------------------
# Comet - Luma screenshot manager & MPO converter
# Standard devkitARM / citro2d Makefile
#-------------------------------------------------------------------------------
.SUFFIXES:

ifeq ($(strip $(DEVKITARM)),)
$(error "Please set DEVKITARM in your environment. export DEVKITARM=<path to>devkitARM")
endif

TOPDIR ?= $(CURDIR)
include $(DEVKITARM)/3ds_rules

#-------------------------------------------------------------------------------
TARGET      :=  Comet
BUILD       :=  build
SOURCES     :=  source source/assets
DATA        :=  data
INCLUDES    :=  include
ROMFS       :=  romfs

APP_TITLE      := Comet
APP_DESCRIPTION:= Observatory of Luma Screenshots
APP_AUTHOR     := verypedro

#-------------------------------------------------------------------------------
ARCH    :=  -march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft

CFLAGS  :=  -g -Wall -O2 -mword-relocations \
            -fomit-frame-pointer -ffunction-sections \
            $(ARCH)

CFLAGS  +=  $(INCLUDE) -D__3DS__

CXXFLAGS := $(CFLAGS) -fno-rtti -fno-exceptions -std=gnu++11

ASFLAGS :=  -g $(ARCH)
LDFLAGS =   -specs=3dsx.specs -g $(ARCH) -Wl,-Map,$(notdir $*.map)

LIBS    :=  -lcitro2d -lcitro3d -lctru -lm

#-------------------------------------------------------------------------------
LIBDIRS := $(CTRULIB) $(PORTLIBS)

#-------------------------------------------------------------------------------
ifneq ($(BUILD),$(notdir $(CURDIR)))
#-------------------------------------------------------------------------------

export OUTPUT   :=  $(CURDIR)/$(TARGET)
export TOPDIR   :=  $(CURDIR)

export VPATH    :=  $(foreach dir,$(SOURCES),$(CURDIR)/$(dir)) \
                     $(foreach dir,$(DATA),$(CURDIR)/$(dir))

export DEPSDIR  :=  $(CURDIR)/$(BUILD)

CFILES      :=  $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.c)))
CPPFILES    :=  $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.cpp)))
SFILES      :=  $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.s)))
BINFILES    :=  $(foreach dir,$(DATA),$(notdir $(wildcard $(dir)/*.*)))

ifeq ($(strip $(CPPFILES)),)
    export LD   :=  $(CC)
else
    export LD   :=  $(CXX)
endif

export OFILES_BIN  :=  $(addsuffix .o,$(BINFILES))
export OFILES_SRC  :=  $(CPPFILES:.cpp=.o) $(CFILES:.c=.o) $(SFILES:.s=.o)
export OFILES       =  $(OFILES_BIN) $(OFILES_SRC)
export HFILES_BIN   =  $(addsuffix .h,$(subst .,_,$(BINFILES)))

export INCLUDE  :=  $(foreach dir,$(INCLUDES),-I$(CURDIR)/$(dir)) \
                     $(foreach dir,$(LIBDIRS),-I$(dir)/include) \
                     -I$(CURDIR)/$(BUILD)

export LIBPATHS :=  $(foreach dir,$(LIBDIRS),-L$(dir)/lib)

ifeq ($(strip $(ICON)),)
    icons := $(wildcard *.png)
    ifneq (,$(findstring $(TARGET).png,$(icons)))
        export APP_ICON := $(TOPDIR)/$(TARGET).png
    else
        ifneq (,$(findstring icon.png,$(icons)))
            export APP_ICON := $(TOPDIR)/icon.png
        endif
    endif
else
    export APP_ICON := $(TOPDIR)/$(ICON)
endif

ifeq ($(strip $(NO_SMDH)),)
    export _3DSXFLAGS += --smdh=$(CURDIR)/$(TARGET).smdh
    export _3DSXDEPS  := $(OUTPUT).smdh
endif

ifneq ($(ROMFS),)
    export _3DSXFLAGS += --romfs=$(CURDIR)/$(ROMFS)
endif

.PHONY: $(BUILD) clean all cia 3dsx

#-------------------------------------------------------------------------------
all: $(BUILD)

$(BUILD):
	@[ -d $@ ] || mkdir -p $@
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile

cia:
	@[ -d $(BUILD) ] || mkdir -p $(BUILD)
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile cia

3dsx:
	@[ -d $(BUILD) ] || mkdir -p $(BUILD)
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile 3dsx

clean:
	@echo clean ...
	@rm -fr $(BUILD) $(TARGET).3dsx $(OUTPUT).smdh $(TARGET).elf $(TARGET).cia $(TARGET).bnr $(TARGET).icn

#-------------------------------------------------------------------------------
else
.PHONY: all cia 3dsx

DEPENDS :=  $(OFILES:.o=.d)

all  :   $(OUTPUT).3dsx $(OUTPUT).cia
3dsx :   $(OUTPUT).3dsx

$(OUTPUT).3dsx  :   $(OUTPUT).elf $(_3DSXDEPS)
$(OUTPUT).elf   :   $(OFILES)

# --- CIA build -----------------------------------------------------------
# makerom and bannertool are NOT part of the 3ds-dev package group (unlike
# mkbcfnt/tex3ds) -- download them from 3DSGuy/Project_CTR and
# Steveice10/bannertool releases on GitHub and put both on PATH.
MAKEROM     := makerom
BANNERTOOL  := bannertool
APP_RSF     := $(TOPDIR)/meta/app.rsf
APP_BANNER_IMG := $(TOPDIR)/meta/banner.png   # 256x128, transparency OK
APP_BANNER_WAV := $(TOPDIR)/meta/banner.wav   # 16-bit PCM, ~3s or less

cia: $(OUTPUT).cia

$(OUTPUT).bnr: $(APP_BANNER_IMG) $(APP_BANNER_WAV)
	$(BANNERTOOL) makebanner -i $(APP_BANNER_IMG) -a $(APP_BANNER_WAV) -o $(OUTPUT).bnr

$(OUTPUT).icn: $(APP_ICON)
	$(BANNERTOOL) makesmdh -i $(APP_ICON) -s "$(APP_TITLE)" -l "$(APP_DESCRIPTION)" -p "$(APP_AUTHOR)" -o $(OUTPUT).icn

$(OUTPUT).cia: $(OUTPUT).elf $(OUTPUT).bnr $(OUTPUT).icn
	cd $(TOPDIR) && $(MAKEROM) -f cia -o $(OUTPUT).cia -rsf $(APP_RSF) -target t -exefslogo \
		-elf $(OUTPUT).elf -icon $(OUTPUT).icn -banner $(OUTPUT).bnr

-include $(DEPENDS)

endif
