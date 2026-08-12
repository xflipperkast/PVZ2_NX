#---------------------------------------------------------------------------------
.SUFFIXES:
#---------------------------------------------------------------------------------

ifeq ($(strip $(DEVKITPRO)),)
$(error "Please set DEVKITPRO in your environment. export DEVKITPRO=<path to>/devkitpro")
endif

TOPDIR ?= $(CURDIR)
include $(DEVKITPRO)/libnx/switch_rules

#---------------------------------------------------------------------------------
TARGET		:=	pvz2_nx
APP_TITLE	:=	Plants vs. Zombies 2
APP_AUTHOR	:=	Electronic Arts, Flippy
APP_VERSION	:=	0.1.0
BUILD		:=	build
SOURCES		:=	source
DATA		:=	data
INCLUDES	:=	include
RELEASE_DIR	:=	release/$(TARGET)
CONSENT_ASSETS	:=	PlantsVsZombies 2 data/game apk/assets
GAME_MANIFEST	:=	PlantsVsZombies\ 2\ data/manifest.json
VERSION_ASSET	:=	$(RELEASE_DIR)/assets/version.txt
VERSION_TAG	:=	$(RELEASE_DIR)/tags/app_version
VERSION_HEADER	:=	source/game_version.h

#---------------------------------------------------------------------------------
# options for code generation
#---------------------------------------------------------------------------------
ARCH	:=	-march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE

CFLAGS	:=	-g -Wall -O2 -ffunction-sections \
			$(ARCH) $(DEFINES)

CFLAGS	+=	$(INCLUDE) -D__SWITCH__

CXXFLAGS	:= $(CFLAGS)

ASFLAGS	:=	-g $(ARCH)
LDFLAGS	=	-specs=$(DEVKITPRO)/libnx/switch.specs -g $(ARCH) -Wl,-Map,$(notdir $*.map) \
			-Wl,--wrap=getaddrinfo -Wl,--wrap=freeaddrinfo

# The game uses the GLES2/EGL stack supplied by switch-mesa. SDL2 provides the
# audio device, while the game supplies its own runtime and asset decoding.
LIBS	:= -lSDL2 -lGLESv2 -lEGL -lglapi -ldrm_nouveau -lcurl -lmbedcrypto -lz -lnx -lm

#---------------------------------------------------------------------------------
# list of directories containing libraries, this must be the top level containing
# include and lib
#---------------------------------------------------------------------------------
LIBDIRS	:= $(PORTLIBS) $(LIBNX)


#---------------------------------------------------------------------------------
# no real need to edit anything past this point unless you need to add additional
# rules for different file extensions
#---------------------------------------------------------------------------------
ifneq ($(BUILD),$(notdir $(CURDIR)))
#---------------------------------------------------------------------------------

export OUTPUT	:=	$(CURDIR)/$(TARGET)
export TOPDIR	:=	$(CURDIR)

export VPATH	:=	$(foreach dir,$(SOURCES),$(CURDIR)/$(dir)) \
			$(foreach dir,$(DATA),$(CURDIR)/$(dir))

export DEPSDIR	:=	$(CURDIR)/$(BUILD)

CFILES		:=	main.c error.c imports.c jni_fake.c libc_shim.c \
			music.c nx_crash_handler.c obb.c opensles.c os_shims.c platform.c pvz2.c so_util.c util.c watchdog.c editbox.c
CPPFILES	:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.cpp)))
SFILES		:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.s)))
BINFILES	:=	$(foreach dir,$(DATA),$(notdir $(wildcard $(dir)/*.*)))

#---------------------------------------------------------------------------------
# use CXX for linking C++ projects, CC for standard C
#---------------------------------------------------------------------------------
export LD	:=	$(CXX)

export OFILES_BIN	:=	$(addsuffix .o,$(BINFILES))
export OFILES_SRC	:=	$(CPPFILES:.cpp=.o) $(CFILES:.c=.o) $(SFILES:.s=.o)
export OFILES 	:=	$(OFILES_BIN) $(OFILES_SRC)
export HFILES_BIN	:=	$(addsuffix .h,$(subst .,_,$(BINFILES)))

export INCLUDE	:=	$(foreach dir,$(INCLUDES),-I$(CURDIR)/$(dir)) \
			$(foreach dir,$(LIBDIRS),-I$(dir)/include) \
			-I$(CURDIR)/$(BUILD)

export LIBPATHS	:=	$(foreach dir,$(LIBDIRS),-L$(dir)/lib)

ifeq ($(strip $(ICON)),)
	ifneq ($(wildcard $(TOPDIR)/icon.jpg),)
		export APP_ICON := $(TOPDIR)/icon.jpg
	else ifneq ($(wildcard $(TOPDIR)/icon_256.png),)
		export APP_ICON := $(TOPDIR)/icon_256.png
	else ifneq ($(wildcard $(TOPDIR)/icon.png),)
		export APP_ICON := $(TOPDIR)/icon.png
	else
		icons := $(wildcard *.jpg)
		ifneq (,$(findstring $(TARGET).jpg,$(icons)))
			export APP_ICON := $(TOPDIR)/$(TARGET).jpg
		else
			ifneq (,$(findstring icon.jpg,$(icons)))
				export APP_ICON := $(TOPDIR)/icon.jpg
			endif
		endif
	endif
else
	export APP_ICON := $(TOPDIR)/$(ICON)
endif

ifeq ($(strip $(NO_ICON)),)
	export NROFLAGS += --icon=$(APP_ICON)
endif

ifeq ($(strip $(NO_NACP)),)
	export NROFLAGS += --nacp=$(CURDIR)/$(TARGET).nacp
endif

ifneq ($(APP_TITLEID),)
	export NACPFLAGS += --titleid=$(APP_TITLEID)
endif

ifneq ($(ROMFS),)
	export NROFLAGS += --romfsdir=$(CURDIR)/$(ROMFS)
endif

.PHONY: $(BUILD) clean all release

#---------------------------------------------------------------------------------
all: $(BUILD) release

release: $(BUILD) $(VERSION_ASSET) $(VERSION_TAG)
	@mkdir -p $(RELEASE_DIR)
	@cp $(TARGET).nro $(RELEASE_DIR)/$(TARGET).nro
	@mkdir -p $(RELEASE_DIR)/assets
	@cp "$(CONSENT_ASSETS)/consentform.html" "$(RELEASE_DIR)/assets/consentform.html"
	@cp "$(CONSENT_ASSETS)/consentformMeta.json" "$(RELEASE_DIR)/assets/consentformMeta.json"
	@echo updated $(RELEASE_DIR)/$(TARGET).nro

$(VERSION_ASSET) $(VERSION_TAG): $(GAME_MANIFEST)
	@mkdir -p $(dir $@)
	@sed -n 's/.*"version_name":"\([^"]*\)".*/\1/p' "$<" > "$@"

$(VERSION_HEADER): $(GAME_MANIFEST)
	@sed -n 's/.*"version_name":"\([^"]*\)".*/#define GAME_VERSION "\1"/p' "$<" > "$@"

$(BUILD): $(VERSION_HEADER)
	@[ -d $@ ] || mkdir -p $@
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile

#---------------------------------------------------------------------------------
clean:
	@echo clean ...
	@rm -fr $(BUILD) $(TARGET).nro $(TARGET).nacp $(TARGET).elf

#---------------------------------------------------------------------------------
else
.PHONY:	all

DEPENDS	:=	$(OFILES:.o=.d)

#---------------------------------------------------------------------------------
# main targets
#---------------------------------------------------------------------------------
all	:	$(OUTPUT).nro

$(OUTPUT).nro	:	$(OUTPUT).elf $(OUTPUT).nacp
$(OUTPUT).elf	:	$(OFILES)

$(OFILES_SRC)	: $(HFILES_BIN)

#---------------------------------------------------------------------------------
# you need a rule like this for each extension you use as binary data
#---------------------------------------------------------------------------------
%.bin.o	%_bin.h :	%.bin
#---------------------------------------------------------------------------------
	@echo $(notdir $<)
	@$(bin2o)

-include $(DEPENDS)


#---------------------------------------------------------------------------------------
endif
#---------------------------------------------------------------------------------------
