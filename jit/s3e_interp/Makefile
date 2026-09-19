#---------------------------------------------------------------------------------
.SUFFIXES:
#---------------------------------------------------------------------------------

ifeq ($(strip $(DEVKITPRO)),)
$(error "Please set DEVKITPRO in your environment. export DEVKITPRO=<path to>/devkitpro")
endif

TOPDIR ?= $(CURDIR)
include $(DEVKITPRO)/libnx/switch_rules

#---------------------------------------------------------------------------------
# TARGET is the name of the output
# BUILD is the directory where object files & intermediate files will be placed
# SOURCES is a list of directories containing source code
# DATA is a list of directories containing data files
# INCLUDES is a list of directories containing header files
# ROMFS is the directory containing data to be added to RomFS, relative to the Makefile (Optional)
#
# NO_ICON: if set to anything, do not use icon.
# NO_NACP: if set to anything, no .nacp file is generated.
# APP_TITLE is the name of the app stored in the .nacp file (Optional)
# APP_AUTHOR is the author of the app stored in the .nacp file (Optional)
# APP_VERSION is the version of the app stored in the .nacp file (Optional)
# APP_TITLEID is the titleID of the app stored in the .nacp file (Optional)
# ICON is the filename of the icon (.jpg), relative to the project folder.
#   If not set, it attempts to use one of the following (in this order):
#     - <Project name>.jpg
#     - icon.jpg
#     - <libnx folder>/default_icon.jpg
#
# CONFIG_JSON is the filename of the NPDM config file (.json), relative to the project folder.
#   If not set, it attempts to use one of the following (in this order):
#     - <Project name>.json
#     - config.json
#   If a JSON file is provided or autodetected, an ExeFS PFS0 (.nsp) is built instead
#   of a homebrew executable (.nro). This is intended to be used for sysmodules.
#   NACP building is skipped as well.
#---------------------------------------------------------------------------------
TARGET		:=	$(notdir $(CURDIR))
BUILD		:=	build
APP_TITLE	:=	s3e interpreter test
APP_AUTHOR	:=	codboz_nx
APP_VERSION	:=	0.1
SOURCES		:=	source .. ../../loader ../third_party/imgui
DATA		:=	data
INCLUDES	:=	.. ../../loader ../third_party/imgui
#ROMFS	:=	romfs

#---------------------------------------------------------------------------------
# options for code generation
#---------------------------------------------------------------------------------
ARCH	:=	-march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE

# -O2, not -O3: measured on the device across matched scenes, -O3 was 0.4%
# faster on one and 1.2% slower on another -- a wash, for a larger binary.
# -ffp-contract stays off below regardless; that one is correctness.
CFLAGS	:=	-g -Wall -O2 -ffunction-sections \
			$(ARCH) $(DEFINES)

CFLAGS	+=	$(INCLUDE) -D__SWITCH__
# The JIT dispatch in guest_run is compiled out unless this is defined, so a
# build without it pays nothing at all rather than relying on a branch being
# predicted. Whether that branch actually costs anything could not be
# resolved on the host bench -- the difference was inside its noise -- which
# is reason to remove the question rather than answer it. The JIT itself is
# still opt-in at runtime via jit.txt on the card.
CFLAGS	+=	-DBOZ_JIT
# VFP VMLA/VMLS are not fused: the interpreter rounds the product separately,
# and GCC's default -ffp-contract=fast would merge it back into an FMA and
# reintroduce the last-bit difference against real hardware.
CFLAGS	+=	-ffp-contract=off

# Dynarmic. Opt-in with `make DYNARMIC=1`, because the library is checked out
# beside this repo rather than vendored into it -- it is 150 MB of archive and
# carries its own externals -- and a build should not fail merely because a
# sibling directory is missing.
#
# Both trees are patched for Horizon by jit/dynarmic_patches/*.py; see there
# for what and why. Build the library first with:
#
#   CXXFLAGS=-D_GNU_SOURCE CFLAGS=-D_GNU_SOURCE cmake -S . -B build-nx -G Ninja #     -DCMAKE_TOOLCHAIN_FILE=$(DEVKITPRO)/cmake/Switch.cmake ...
#
# Note the flags go through the environment: passing -DCMAKE_CXX_FLAGS on the
# command line REPLACES the devkitPro platform module's own flags and silently
# drops -D__SWITCH__, which compiles every Horizon branch out and still
# "succeeds".
DYNARMIC_ROOT	?=	$(TOPDIR)/../../../dynarmic
BOOST_ROOT	?=	$(TOPDIR)/../../../boost_hdr
ifeq ($(DYNARMIC),1)
CFLAGS	+=	-DBOZ_DYNARMIC 			-I$(DYNARMIC_ROOT)/src 			-I$(DYNARMIC_ROOT)/externals/mcl/include 			-I$(DYNARMIC_ROOT)/externals/oaknut/include 			-I$(DYNARMIC_ROOT)/externals/robin-map/include 			-I$(DYNARMIC_ROOT)/externals/fmt/include 			-I$(BOOST_ROOT)
endif

# Profile-guided optimisation, the last item on the performance plan with a
# plausible double-digit gain. Two passes:
#
#   make PGO=gen     instrumented; run the benchmark, then fetch the .gcda
#                    files off the card with the control socket's GET
#   make PGO=use     rebuilt using them
#
# BOZ_PGO also switches on the GCOV_PREFIX redirection and the explicit
# __gcov_dump() in main.c -- nothing calls exit() on the normal path, so the
# counters would never be flushed otherwise.
#
# -fprofile-correction because the guest runs on one thread but the profile is
# gathered across a run that libnx may service from another; without it gcc
# treats inconsistent counters as an error rather than smoothing them.
ifeq ($(PGO),gen)
CFLAGS	+=	-fprofile-generate -DBOZ_PGO
LDFLAGS	+=	-fprofile-generate
# -lgcov goes in LIBS, not LDFLAGS. devkitPro's link rule expands LDFLAGS
# BEFORE the object files, and a static archive only resolves references the
# linker has already seen -- so -lgcov placed there is scanned against nothing
# and every __gcov_merge_* reference comes out undefined even though libgcov.a
# defines all of them.
endif
ifeq ($(PGO),use)
CFLAGS	+=	-fprofile-use -fprofile-correction -Wno-missing-profile
endif

# Dear ImGui (the settings menu): no shell on Horizon to open URLs with.
CXXFLAGS	:= $(CFLAGS) -fno-rtti -fno-exceptions -DIMGUI_DISABLE_DEFAULT_SHELL_FUNCTIONS

ASFLAGS	:=	-g $(ARCH)
LDFLAGS	=	-specs=$(DEVKITPRO)/libnx/switch.specs -g $(ARCH) -Wl,-Map,$(notdir $*.map)

LIBS	:= -lGLESv1_CM -lEGL -lglapi -ldrm_nouveau -lstdc++ -lm -lnx

# After the assignment above, not before it: LIBS is set with := further down
# the file than the PGO block, so adding -lgcov up there was simply
# overwritten. It has to be here, and it has to be in LIBS rather than LDFLAGS
# because devkitPro expands LDFLAGS ahead of the object files and a static
# archive only resolves references the linker has already seen.
ifeq ($(PGO),gen)
LIBS	:=	-lgcov $(LIBS)
endif

# Dynarmic and its externals, ahead of -lstdc++ because a static archive only
# resolves what the linker has already seen and all three need the C++ runtime.
# dynarmic before fmt and mcl for the same reason.
ifeq ($(DYNARMIC),1)
LIBS	:=	$(DYNARMIC_ROOT)/build-nx/src/dynarmic/libdynarmic.a 			$(DYNARMIC_ROOT)/build-nx/externals/fmt/libfmt.a 			$(DYNARMIC_ROOT)/build-nx/externals/mcl/src/libmcl.a 			$(LIBS)
endif

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

CFILES		:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.c)))
CPPFILES	:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.cpp)))
SFILES		:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.s)))
BINFILES	:=	$(foreach dir,$(DATA),$(notdir $(wildcard $(dir)/*.*)))

#---------------------------------------------------------------------------------
# use CXX for linking C++ projects, CC for standard C
#---------------------------------------------------------------------------------
ifeq ($(strip $(CPPFILES)),)
#---------------------------------------------------------------------------------
	export LD	:=	$(CC)
#---------------------------------------------------------------------------------
else
#---------------------------------------------------------------------------------
	export LD	:=	$(CXX)
#---------------------------------------------------------------------------------
endif
#---------------------------------------------------------------------------------

export OFILES_BIN	:=	$(addsuffix .o,$(BINFILES))
export OFILES_SRC	:=	$(CPPFILES:.cpp=.o) $(CFILES:.c=.o) $(SFILES:.s=.o)
export OFILES 	:=	$(OFILES_BIN) $(OFILES_SRC)
export HFILES_BIN	:=	$(addsuffix .h,$(subst .,_,$(BINFILES)))

export INCLUDE	:=	$(foreach dir,$(INCLUDES),-I$(CURDIR)/$(dir)) \
			$(foreach dir,$(LIBDIRS),-I$(dir)/include) \
			-I$(CURDIR)/$(BUILD)

export LIBPATHS	:=	$(foreach dir,$(LIBDIRS),-L$(dir)/lib)

ifeq ($(strip $(CONFIG_JSON)),)
	jsons := $(wildcard *.json)
	ifneq (,$(findstring $(TARGET).json,$(jsons)))
		export APP_JSON := $(TOPDIR)/$(TARGET).json
	else
		ifneq (,$(findstring config.json,$(jsons)))
			export APP_JSON := $(TOPDIR)/config.json
		endif
	endif
else
	export APP_JSON := $(TOPDIR)/$(CONFIG_JSON)
endif

ifeq ($(strip $(ICON)),)
	icons := $(wildcard *.jpg)
	ifneq (,$(findstring $(TARGET).jpg,$(icons)))
		export APP_ICON := $(TOPDIR)/$(TARGET).jpg
	else
		ifneq (,$(findstring icon.jpg,$(icons)))
			export APP_ICON := $(TOPDIR)/icon.jpg
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

#---------------------------------------------------------------------------------
# Build identity.
#
# The banner was "jit-r82 " __DATE__ " " __TIME__, and those two macros expand
# when main.c is COMPILED -- not when the binary is linked. Change jit.c alone
# and main.o is not rebuilt, so the banner keeps reporting the moment main.o
# was built and two binaries that behave differently announce themselves with
# the same string. That is not a cosmetic problem: it cost three restarts spent
# believing a fix had failed to reach the card when it had been running all
# along, and the wrong conclusion was drawn from the one field that looked
# authoritative.
#
# The identity is now a hash of the sources that actually go into the binary.
# It changes when the code changes and not otherwise, so the same sources
# always produce the same string and any difference in the string is a real
# difference in the build.
#---------------------------------------------------------------------------------
BOZ_ID_SRCS := $(sort $(foreach dir,$(SOURCES) $(INCLUDES),$(wildcard $(dir)/*.c $(dir)/*.h)))
BOZ_BUILD_ID := $(shell cat $(BOZ_ID_SRCS) 2>/dev/null | md5sum 2>/dev/null | cut -c1-8)
ifeq ($(strip $(BOZ_BUILD_ID)),)
BOZ_BUILD_ID := nohash
endif
BOZ_ID_HDR := $(BUILD)/boz_build_id.h

.PHONY: $(BUILD) clean all

#---------------------------------------------------------------------------------
all: $(BUILD)

$(BUILD):
	@[ -d $@ ] || mkdir -p $@
	@echo '#define BOZ_BUILD_ID "$(BOZ_BUILD_ID)"' > $(BOZ_ID_HDR).tmp
	@cmp -s $(BOZ_ID_HDR).tmp $(BOZ_ID_HDR) 2>/dev/null || mv -f $(BOZ_ID_HDR).tmp $(BOZ_ID_HDR)
	@rm -f $(BOZ_ID_HDR).tmp
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile

#---------------------------------------------------------------------------------
clean:
	@echo clean ...
ifeq ($(strip $(APP_JSON)),)
	@rm -fr $(BUILD) $(TARGET).nro $(TARGET).nacp $(TARGET).elf
else
	@rm -fr $(BUILD) $(TARGET).nsp $(TARGET).nso $(TARGET).npdm $(TARGET).elf
endif


#---------------------------------------------------------------------------------
else
.PHONY:	all

DEPENDS	:=	$(OFILES:.o=.d)

#---------------------------------------------------------------------------------
# main targets
#---------------------------------------------------------------------------------
ifeq ($(strip $(APP_JSON)),)

all	:	$(OUTPUT).nro

ifeq ($(strip $(NO_NACP)),)
$(OUTPUT).nro	:	$(OUTPUT).elf $(OUTPUT).nacp
else
$(OUTPUT).nro	:	$(OUTPUT).elf
endif

else

all	:	$(OUTPUT).nsp

$(OUTPUT).nsp	:	$(OUTPUT).nso $(OUTPUT).npdm

$(OUTPUT).nso	:	$(OUTPUT).elf

endif

$(OUTPUT).elf	:	$(OFILES)

$(OFILES_SRC)	: $(HFILES_BIN)

# mcl's ASSERT throws, so every dynarmic header needs exceptions enabled. Only
# this one translation unit includes them; the rest of the program keeps
# -fno-exceptions. Nothing here throws, so the cost is the unwind tables for
# one object file.
dynarmic_glue.o: CXXFLAGS += -fexceptions


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
