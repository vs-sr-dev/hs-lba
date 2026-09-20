# HS-LBA - Little Big Adventure 1 for the Mattel HyperScan
#
# Uses the OFFICIAL Sunplus toolchain (GCC 4.2.1 + binutils, extracted from
# "S+core IDE-V2.6.1.exe" in the PPCSDK) rather than a modern GCC graft.
# Flags are the SDK's own: -mscore7 -mel -Os. Note -mel: the console is
# LITTLE-ENDIAN, so there is NO byte-swap post-processing step here.

TOOLDIR := toolchain/gnu/bin
CC      := $(TOOLDIR)/score-elf-gcc.exe
OBJCOPY := $(TOOLDIR)/score-elf-objcopy.exe
OBJDUMP := $(TOOLDIR)/score-elf-objdump.exe
READELF := $(TOOLDIR)/score-elf-readelf.exe
NM      := $(TOOLDIR)/score-elf-nm.exe

BUILD   := build
SDK     := sdk
LDSCRIPT:= $(SDK)/boot/hyperscan_Prog.ld

# EXTRA_CFLAGS is for one-off experiment builds, e.g.
#   make EXTRA_CFLAGS=-DHS_VIDEO_QVGA
CFLAGS  := -mscore7 -mel -Os -Wall -g '-DNEW_HEAPSIZE=8*(1024*1024)' $(EXTRA_CFLAGS)

# Header dependencies. Without these a header edit rebuilds only the .c files
# that were themselves touched, and everything else keeps the old macro values —
# which is how a build once ended up with half its translation units at
# VIDEO_W 640 and half at 320, linked, and running. The failure had no
# diagnostic: it looked like a rendering bug.
DEPFLAGS := -MMD -MP
INCLUDES:= -I$(SDK)/SPG290/scorelibs/include -I$(SDK)/System/HyperScan/hslibs/include -Isrc -Itranslate
ASFLAGS := -Wa,-gdwarf-2 -x assembler-with-cpp
LDFLAGS := -T$(LDSCRIPT) -lm -lc -lgcc -Wl,-Map,$(BUILD)/hyperscan.map

# SDK objects: startup, board support, drivers.
SDK_SRC := \
	$(SDK)/SPG290/scorelibs/TV/TV.c \
	$(SDK)/SPG290/scorelibs/UART/UART.c \
	$(SDK)/SPG290/scorelibs/PPU/PPU.c \
	$(SDK)/SPG290/scorelibs/NorFlash/NorFlash.c

# Deliberately NOT built:
#   scorelibs/I2C + hslibs/HS_Controller — both deadlock (see src/input.c), and
#     HS_Controller_Read() writes controller[1] into a `ControllerUnion [1]`.
#   IRQ/Sys_IRQ.c + IRQ/User_IRQ.c — a 64-way switch into forty empty handlers.
#     src/irq.c provides irq_dispatch() with a handler table instead.
#   libgloss.c — its heap is an 8 MB .bss array that contains the framebuffers,
#     and its _open_r always fails. src/syscalls.c replaces the whole bottom end.
# Sys_isr.s (the vector table and register save/restore) IS built.

# The entry point. Milestones that test one subsystem in isolation replace it —
#   make MAIN=src/m19_card.c
# — which is what the m*.c files beside main.c are. They are not built
# otherwise; only one of them can define main().
MAIN ?= src/main.c

APP_SRC := $(MAIN) src/irq.c src/timer.c src/video.c src/input.c \
           src/syscalls.c src/fs.c src/cd.c src/hqr.c src/dospath.c \
           src/platform.c src/platform_gfx.c src/platform_in.c \
           src/platform_snd.c src/audio.c src/card.c src/save.c

# --------------------------------------------------------------------------
# The engine proper, plus translate/ (Adeline's x86 assembly, already in C).
#
# 1994 Watcom C for DOS, compiled on its own terms:
#   -x c          .C is C, not the C++ GCC infers from the capital extension
#   -std=gnu89    K&R-era declarations, implicit int, duplicate commons
#   -fsigned-char x86 code assumes signed char; score defaults to unsigned
#   -include      compat/watcom_compat.h kills the Watcom keywords
#   -w            27k lines of pre-ANSI style would otherwise bury real output
# compat/inc is on the include path so the engine's own
# "../LIB386/LIB_SYS/SYS_*.H" resolves to the forwarders in compat/LIB386.
ENGINE    := engine
TRANSLATE := translate

ENGINE_SRC := $(wildcard $(ENGINE)/game/*.C)     $(wildcard $(ENGINE)/LIB_3D/*.C)  \
              $(wildcard $(ENGINE)/LIB_CD/*.C)   $(wildcard $(ENGINE)/LIB_MENU/*.C) \
              $(wildcard $(ENGINE)/LIB_MIDI/*.C) $(wildcard $(ENGINE)/LIB_MIX/*.C)  \
              $(wildcard $(ENGINE)/LIB_SVGA/*.C) $(wildcard $(ENGINE)/LIB_SYS/*.C)

TRANSLATE_SRC := $(wildcard $(TRANSLATE)/*.c)

ENGINE_INC := -I$(ENGINE) -I$(ENGINE)/game -I$(ENGINE)/LIB_SYS -I$(ENGINE)/LIB_CD \
              -I$(ENGINE)/LIB_MENU -I$(ENGINE)/LIB_SVGA -I$(ENGINE)/LIB_3D \
              -Icompat -Icompat/inc

# PORT_HS joins PORT_SDL/PORT_NDS on the guards the earlier ports already added
# for two symbols the Watcom librarian resolved by link order (Message,
# CreateMaskGph) and for the menu preview, which on DOS wrote to the VGA
# aperture at a hardcoded 0xA0000.
ENGINE_CFLAGS := -mscore7 -mel -Os -g -x c -std=gnu89 -fsigned-char -w \
                 -DPORT_HS -DCDROM \
                 -include compat/watcom_compat.h $(INCLUDES) $(ENGINE_INC) \
                 $(EXTRA_CFLAGS)

ENGINE_OBJ    := $(patsubst $(ENGINE)/%.C,$(BUILD)/engine/%.o,$(ENGINE_SRC))
TRANSLATE_OBJ := $(patsubst $(TRANSLATE)/%.c,$(BUILD)/translate/%.o,$(TRANSLATE_SRC))

ASM_SRC := $(SDK)/boot/hyperscan_startup.s $(SDK)/SPG290/scorelibs/IRQ/Sys_isr.s

# Our sources keep flat object names; engine/ and translate/ mirror their
# directory structure, because engine/LIB_SYS/HQR.C and src/hqr.c would
# otherwise both want to be build/hqr.o.
OBJS := $(addprefix $(BUILD)/,$(notdir $(ASM_SRC:.s=.o))) \
        $(addprefix $(BUILD)/,$(notdir $(SDK_SRC:.c=.o))) \
        $(addprefix $(BUILD)/,$(notdir $(APP_SRC:.c=.o))) \
        $(ENGINE_OBJ) $(TRANSLATE_OBJ)

vpath %.c src translate $(SDK)/SPG290/scorelibs $(SDK)/SPG290/scorelibs/TV $(SDK)/SPG290/scorelibs/UART $(SDK)/SPG290/scorelibs/PPU $(SDK)/SPG290/scorelibs/I2C $(SDK)/SPG290/scorelibs/IRQ $(SDK)/SPG290/scorelibs/NorFlash $(SDK)/System/HyperScan/hslibs/HS_Controller
vpath %.s $(SDK)/boot $(SDK)/SPG290/scorelibs/IRQ

all: $(BUILD)/HYPER.EXE $(BUILD)/syms.lua $(BUILD)/fsyms.lua

$(BUILD):
	@mkdir -p $(BUILD)

$(BUILD)/%.o: %.c | $(BUILD)
	$(CC) -c $(CFLAGS) $(DEPFLAGS) $(INCLUDES) -o $@ $<

$(BUILD)/%.o: %.s | $(BUILD)
	$(CC) -c $(CFLAGS) $(ASFLAGS) $(INCLUDES) -o $@ $<

$(BUILD)/engine/%.o: $(ENGINE)/%.C
	@mkdir -p $(dir $@)
	$(CC) -c $(ENGINE_CFLAGS) $(DEPFLAGS) -o $@ $<

$(BUILD)/translate/%.o: $(TRANSLATE)/%.c
	@mkdir -p $(dir $@)
	$(CC) -c $(ENGINE_CFLAGS) $(DEPFLAGS) -o $@ $<

$(BUILD)/hyperscan.elf: $(OBJS) $(LDSCRIPT)
	$(CC) $(CFLAGS) $(OBJS) -o $@ $(LDFLAGS)

# HYPER.EXE is a raw image starting at the exception vector (0xA00901FC);
# MAME quickloads it there and enters at 0xA0091000.
$(BUILD)/HYPER.EXE: $(BUILD)/hyperscan.elf
	$(OBJCOPY) -O binary --gap-fill 0 $< $@
	@$(READELF) -h $<
	@ls -l $@

# The MAME harnesses in tools/ read engine globals out of a running machine, and
# every rebuild moves them. Emitting the symbol table as a Lua table keeps the
# scripts written in names instead of addresses; they load it through the
# HSLBA_SYMS environment variable.
$(BUILD)/syms.lua: $(BUILD)/hyperscan.elf
	@echo 'local S = {}' > $@
	@$(NM) $< | awk 'NF==3 { printf "S[\"%s\"] = 0x%s\n", $$3, $$1 }' >> $@
	@echo 'return S' >> $@

# Functions only, sorted by address, for the sampling profiler in
# tools/m21_profile.lua: a sampled PC has to be turned back into a name, and
# that needs ranges, which means order — which syms.lua, keyed by name, cannot
# express. Text symbols only, so a PC can never be attributed to a variable.
#
# `.L*` is dropped, and that is not cosmetic. GCC emits its local branch targets
# into the symbol table as text, so a loop body inside a function gets a symbol
# of its own — and since attribution is "nearest symbol at or below the PC",
# every hot loop is charged to a label like `.L69` instead of to the function it
# belongs to. The first profile run came back led by four such labels and was
# unreadable. Dropping them charges the loop to its enclosing function, which is
# the question being asked.
$(BUILD)/fsyms.lua: $(BUILD)/hyperscan.elf
	@echo 'return {' > $@
	@$(NM) -n $< | awk 'NF==3 && toupper($$2)=="T" && $$3 !~ /^\./ { printf "{0x%s,\"%s\"},\n", $$1, $$3 }' >> $@
	@echo '}' >> $@

# The data image. Named explicitly rather than globbed: LBA.CFG is not an .HQR,
# and the order here is the order the files land on the disc — which is the
# only lever there is against seek time on the real mechanism.
#
# The .VOX files are the spoken dialogue, one archive per island plus GAM,
# 32 MB for English alone — which is why only one language ships. There is no
# EN_SYS or EN_CRE: those two dialogue files were never voiced, and MESSAGE.C
# handles their absence by leaving FdNar null, so they are silently text-only.
DATA_FILES := LBA.CFG TEXT.HQR ANIM.HQR BODY.HQR FILE3D.HQR INVOBJ.HQR \
              LBA_BLL.HQR LBA_BRK.HQR LBA_GRI.HQR RESS.HQR SAMPLES.HQR \
              SCENE.HQR SPRITES.HQR \
              $(notdir $(wildcard data/EN_*.VOX)) \
              FLASAMP.HQR $(notdir $(wildcard data/*.FLA))

iso: $(BUILD)/hslba.iso

$(BUILD)/hslba.iso: $(addprefix data/,$(DATA_FILES)) tools/mkcd.py | $(BUILD)
	python tools/mkcd.py data $@ $(DATA_FILES)

disasm: $(BUILD)/hyperscan.elf
	$(OBJDUMP) -d $< > $(BUILD)/hyperscan.dis

clean:
	rm -rf $(BUILD)

-include $(OBJS:.o=.d)

.PHONY: all clean disasm iso

