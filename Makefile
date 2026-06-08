CC          := gcc
BASE_CFLAGS := -Wall -Wextra -O2 -std=c11 -Icore/include -pthread

SDL2_CFLAGS := $(shell sdl2-config --cflags 2>/dev/null || echo -I/usr/include/SDL2)
SDL2_LIBS   := $(shell sdl2-config --libs   2>/dev/null || echo -lSDL2)

# ── CHIP-8 ───────────────────────────────────────────────────────────────────

CHIP8_CFLAGS := $(BASE_CFLAGS) -Ijemu-chip8/include $(SDL2_CFLAGS)
CHIP8_LDFLAGS = $(SDL2_LIBS) -lncursesw -pthread

CORE_SRC := \
	core/src/memory.c \
	core/src/tcg.c \
	core/src/monitor.c \
	core/src/vnc.c \
	core/src/args.c

CHIP8_SRC := \
	jemu-chip8/src/main.c \
	jemu-chip8/src/cpu/chip8_cpu.c \
	jemu-chip8/src/hardware/machine.c \
	jemu-chip8/src/vga/display_sdl.c \
	jemu-chip8/src/vga/input_sdl.c \
	jemu-chip8/src/vga/display_curses.c

ifdef GTK
CHIP8_CFLAGS += $(shell pkg-config --cflags gtk+-3.0) -DJEMU_GTK
CHIP8_LDFLAGS += $(shell pkg-config --libs gtk+-3.0)
CHIP8_SRC    += jemu-chip8/src/vga/ui_gtk.c
BUILDDIR     := build/gtk
else
BUILDDIR     := build/sdl
endif

CHIP8_OBJ := $(patsubst %.c, $(BUILDDIR)/%.o, $(CORE_SRC) $(CHIP8_SRC))

# ── RCA ──────────────────────────────────────────────────────────────────────

RCA_CFLAGS := $(BASE_CFLAGS) \
	-Ijemu-rca/include \
	-Ijemu-rca/src \
	-Ijemu-rca/src/cpu \
	-Ijemu-rca/src/vga \
	-Ijemu-rca/src/hardware \
	-Ijemu-rca/src/devices \
	$(SDL2_CFLAGS)
RCA_LDFLAGS := $(SDL2_LIBS) -lncursesw -pthread

RCA_CORE_SRC := \
	core/src/memory.c \
	core/src/args.c \
	core/src/monitor.c \
	core/src/vnc.c

RCA_SRC := \
	jemu-rca/src/main.c \
	jemu-rca/src/cpu/cdp1802.c \
	jemu-rca/src/vga/cdp1861.c \
	jemu-rca/src/vga/cdp1869.c \
	jemu-rca/src/vga/display_sdl.c \
	jemu-rca/src/vga/display_curses.c \
	jemu-rca/src/devices/vip_devices.c \
	jemu-rca/src/devices/pcspk.c \
	jemu-rca/src/devices/tape.c \
	jemu-rca/src/hardware/machine_vip.c \
	jemu-rca/src/hardware/machine_destroyer.c \
	jemu-rca/src/hardware/machine_studio2.c

ifdef GTK
RCA_CFLAGS += $(shell pkg-config --cflags gtk+-3.0) -DJEMU_GTK
RCA_LDFLAGS += $(shell pkg-config --libs gtk+-3.0)
RCA_SRC += jemu-rca/src/vga/display_gtk.c
endif

ifdef GTK
RCA_BUILDDIR := build/rca-gtk
else
RCA_BUILDDIR := build/rca
endif

RCA_OBJ := $(patsubst %.c, $(RCA_BUILDDIR)/%.o, $(RCA_CORE_SRC) $(RCA_SRC))

# ── Rules ─────────────────────────────────────────────────────────────────────

.PHONY: all clean rca-force

all: bin/jemu-chip8 bin/jemu-rca

bin/jemu-chip8: $(CHIP8_OBJ)
	@mkdir -p bin
	$(CC) -o $@ $^ $(CHIP8_LDFLAGS)

bin/jemu-rca: rca-force $(RCA_OBJ)
	@mkdir -p bin
	$(CC) -o $@ $(filter %.o,$^) $(RCA_LDFLAGS)

$(BUILDDIR)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CHIP8_CFLAGS) -c -o $@ $<

$(RCA_BUILDDIR)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(RCA_CFLAGS) -c -o $@ $<

clean:
	rm -rf build bin

# ── Header dependencies ───────────────────────────────────────────────────────

CORE_HDRS := \
	Makefile \
	core/include/jemu/jemu.h \
	core/include/jemu/args.h \
	core/include/jemu/display.h

$(CHIP8_OBJ): $(CORE_HDRS) \
	core/include/jemu/memory.h \
	core/include/jemu/cpu.h \
	core/include/jemu/device.h \
	core/include/jemu/monitor.h \
	core/include/jemu/tcg.h \
	core/include/jemu/vnc.h \
	jemu-chip8/include/chip8.h

$(RCA_OBJ): $(CORE_HDRS) \
	core/include/jemu/memory.h \
	core/include/jemu/monitor.h \
	core/include/jemu/vnc.h \
	jemu-rca/include/rca.h \
	jemu-rca/src/cpu/cdp1802.h \
	jemu-rca/src/vga/rca_display.h \
	jemu-rca/src/vga/cdp1861.h \
	jemu-rca/src/vga/cdp1869.h \
	jemu-rca/src/devices/pcspk.h \
	jemu-rca/src/devices/tape.h \
	jemu-rca/src/devices/vip_devices.h \
	jemu-rca/src/hardware/vip.h \
	jemu-rca/src/hardware/destroyer.h \
	jemu-rca/src/hardware/studio2.h
