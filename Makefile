CC          := gcc
BASE_CFLAGS := -Wall -Wextra -O2 -std=c11 -Icore/include -pthread

# Windows (MinGW-w64 / MSYS2) detection
ifeq ($(OS),Windows_NT)
  WINDOWS    := 1
  EXESUFFIX  := .exe
else
  EXESUFFIX  :=
endif

SDL2_CFLAGS := $(shell sdl2-config --cflags 2>/dev/null || echo -I/usr/include/SDL2)
SDL2_LIBS   := $(shell sdl2-config --libs   2>/dev/null || echo -lSDL2)

# ── CHIP-8 ───────────────────────────────────────────────────────────────────

CHIP8_CFLAGS := $(BASE_CFLAGS) -Igemu-chip8/include $(SDL2_CFLAGS)
CHIP8_LDFLAGS  = $(SDL2_LIBS) -pthread

CORE_SRC := \
	core/src/memory.c \
	core/src/tcg.c \
	core/src/monitor.c \
	core/src/vnc.c \
	core/src/args.c \
	core/src/sha256.c \
	core/src/screendump.c

CHIP8_SRC := \
	gemu-chip8/src/main.c \
	gemu-chip8/src/cpu/chip8_cpu.c \
	gemu-chip8/src/hardware/machine.c \
	gemu-chip8/src/vga/display_sdl.c \
	gemu-chip8/src/vga/input_sdl.c

ifdef WINDOWS
CHIP8_CFLAGS  += -DGEMU_NO_CURSES
CHIP8_LDFLAGS += -lws2_32 -mconsole
else
CHIP8_SRC     += gemu-chip8/src/vga/display_curses.c
CHIP8_LDFLAGS += -lncursesw
endif

ifdef GTK
CHIP8_CFLAGS += $(shell pkg-config --cflags gtk+-3.0) -DGEMU_GTK
CHIP8_LDFLAGS += $(shell pkg-config --libs gtk+-3.0)
CHIP8_SRC    += gemu-chip8/src/vga/ui_gtk.c core/src/gtk_menu.c
BUILDDIR     := build/gtk
else
BUILDDIR     := build/sdl
endif

CHIP8_OBJ := $(patsubst %.c, $(BUILDDIR)/%.o, $(CORE_SRC) $(CHIP8_SRC))

# ── RCA ──────────────────────────────────────────────────────────────────────

RCA_CFLAGS := $(BASE_CFLAGS) \
	-Igemu-rca/include \
	-Igemu-rca/src \
	-Igemu-rca/src/cpu \
	-Igemu-rca/src/vga \
	-Igemu-rca/src/hardware \
	-Igemu-rca/src/devices \
	$(SDL2_CFLAGS)
RCA_LDFLAGS := $(SDL2_LIBS) -pthread

RCA_CORE_SRC := \
	core/src/memory.c \
	core/src/args.c \
	core/src/monitor.c \
	core/src/vnc.c \
	core/src/sha256.c \
	core/src/screendump.c

RCA_SRC := \
	gemu-rca/src/main.c \
	gemu-rca/src/cpu/cdp1802.c \
	gemu-rca/src/vga/cdp1861.c \
	gemu-rca/src/vga/cdp1869.c \
	gemu-rca/src/vga/display_sdl.c \
	gemu-rca/src/devices/vip_devices.c \
	gemu-rca/src/devices/pcspk.c \
	gemu-rca/src/devices/tape.c \
	gemu-rca/src/hardware/machine_vip.c \
	gemu-rca/src/hardware/machine_destroyer.c \
	gemu-rca/src/hardware/machine_studio2.c \
	gemu-rca/src/hardware/machine_pecom.c \
	gemu-rca/src/hardware/romdb.c

ifdef WINDOWS
RCA_CFLAGS  += -DGEMU_NO_CURSES
RCA_LDFLAGS += -lws2_32 -mconsole
else
RCA_SRC     += gemu-rca/src/vga/display_curses.c
RCA_LDFLAGS += -lncursesw
endif

ifdef GTK
RCA_CFLAGS += $(shell pkg-config --cflags gtk+-3.0) -DGEMU_GTK
RCA_LDFLAGS += $(shell pkg-config --libs gtk+-3.0)
RCA_SRC += gemu-rca/src/vga/display_gtk.c core/src/gtk_menu.c
endif

ifdef GTK
RCA_BUILDDIR := build/rca-gtk
else
RCA_BUILDDIR := build/rca
endif

RCA_OBJ := $(patsubst %.c, $(RCA_BUILDDIR)/%.o, $(RCA_CORE_SRC) $(RCA_SRC))

# ── Rules ─────────────────────────────────────────────────────────────────────

.PHONY: all clean rca-force

all: bin/gemu-chip8$(EXESUFFIX) bin/gemu-rca$(EXESUFFIX) bin/gemu-6502$(EXESUFFIX)

bin/gemu-chip8$(EXESUFFIX): $(CHIP8_OBJ)
	@mkdir -p bin
	$(CC) -o $@ $^ $(CHIP8_LDFLAGS) $(EXTRA_LDFLAGS)

bin/gemu-rca$(EXESUFFIX): rca-force $(RCA_OBJ)
	@mkdir -p bin
	$(CC) -o $@ $(filter %.o,$^) $(RCA_LDFLAGS) $(EXTRA_LDFLAGS)

$(BUILDDIR)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CHIP8_CFLAGS) -c -o $@ $<

$(RCA_BUILDDIR)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(RCA_CFLAGS) -c -o $@ $<

# ── 6502 ─────────────────────────────────────────────────────────────────────

MOS_CFLAGS := $(BASE_CFLAGS) \
	-Igemu-6502/include \
	-Igemu-6502/src \
	-Igemu-6502/src/cpu \
	-Igemu-6502/src/hardware \
	-Igemu-6502/src/vga \
	-Igemu-6502/src/audio \
	$(SDL2_CFLAGS)
MOS_LDFLAGS := $(SDL2_LIBS) -pthread
ifdef WINDOWS
MOS_LDFLAGS += -lws2_32 -mconsole
endif

MOS_CORE_SRC := \
	core/src/memory.c \
	core/src/args.c \
	core/src/monitor.c \
	core/src/vnc.c \
	core/src/sha256.c \
	core/src/screendump.c

MOS_SRC := \
	gemu-6502/src/main.c \
	gemu-6502/src/cpu/mos6502.c \
	gemu-6502/src/vga/rp2c02.c \
	gemu-6502/src/vga/display_sdl.c \
	gemu-6502/src/audio/apu2a03.c \
	gemu-6502/src/hardware/machine_generic.c \
	gemu-6502/src/hardware/machine_nes.c \
	gemu-6502/src/hardware/nes_devices.c \
	gemu-6502/src/hardware/romdb.c

ifdef GTK
MOS_CFLAGS   += $(shell pkg-config --cflags gtk+-3.0) -DGEMU_GTK
MOS_LDFLAGS  += $(shell pkg-config --libs gtk+-3.0)
MOS_SRC      += gemu-6502/src/vga/display_gtk.c core/src/gtk_menu.c
MOS_BUILDDIR := build/mos-gtk
else
MOS_BUILDDIR := build/mos
endif

MOS_OBJ := $(patsubst %.c, $(MOS_BUILDDIR)/%.o, $(MOS_CORE_SRC) $(MOS_SRC))

bin/gemu-6502$(EXESUFFIX): $(MOS_OBJ)
	@mkdir -p bin
	$(CC) -o $@ $^ $(MOS_LDFLAGS) $(EXTRA_LDFLAGS)

$(MOS_OBJ): $(CORE_HDRS) \
	core/include/gemu/memory.h \
	core/include/gemu/monitor.h \
	core/include/gemu/vnc.h \
	core/include/gemu/sha256.h \
	gemu-6502/include/mos6502cfg.h \
	gemu-6502/src/cpu/mos6502.h \
	gemu-6502/src/hardware/generic.h \
	gemu-6502/src/hardware/nes.h \
	gemu-6502/src/hardware/nes_devices.h \
	gemu-6502/src/hardware/romdb.h \
	gemu-6502/src/vga/rp2c02.h \
	gemu-6502/src/vga/nes_display.h \
	gemu-6502/src/audio/apu2a03.h \
	core/include/gemu/gtk_menu.h

$(MOS_BUILDDIR)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(MOS_CFLAGS) -c -o $@ $<

# ─────────────────────────────────────────────────────────────────────────────

clean:
	rm -rf build bin

# ── Header dependencies ───────────────────────────────────────────────────────

CORE_HDRS := \
	Makefile \
	core/include/gemu/gemu.h \
	core/include/gemu/args.h \
	core/include/gemu/display.h

$(CHIP8_OBJ): $(CORE_HDRS) \
	core/include/gemu/memory.h \
	core/include/gemu/cpu.h \
	core/include/gemu/device.h \
	core/include/gemu/monitor.h \
	core/include/gemu/tcg.h \
	core/include/gemu/vnc.h \
	gemu-chip8/include/chip8.h

$(RCA_OBJ): $(CORE_HDRS) \
	core/include/gemu/memory.h \
	core/include/gemu/monitor.h \
	core/include/gemu/vnc.h \
	gemu-rca/include/rca.h \
	gemu-rca/src/cpu/cdp1802.h \
	gemu-rca/src/vga/rca_display.h \
	gemu-rca/src/vga/cdp1861.h \
	gemu-rca/src/vga/cdp1869.h \
	gemu-rca/src/devices/pcspk.h \
	gemu-rca/src/devices/tape.h \
	gemu-rca/src/devices/vip_devices.h \
	gemu-rca/src/hardware/vip.h \
	gemu-rca/src/hardware/destroyer.h \
	gemu-rca/src/hardware/studio2.h \
	gemu-rca/src/hardware/romdb.h \
	core/include/gemu/sha256.h
