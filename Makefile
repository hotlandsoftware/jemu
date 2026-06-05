CC     := gcc
CFLAGS := -Wall -Wextra -O2 -std=c11 -Icore/include -Ijemu-chip8/include -pthread

CORE_SRC := \
	core/src/memory.c \
	core/src/tcg.c \
	core/src/monitor.c \
	core/src/vnc.c

CHIP8_BASE := \
	jemu-chip8/src/main.c \
	jemu-chip8/src/cpu.c \
	jemu-chip8/src/machine.c

ifdef GTK
# ── GTK3 backend: real menu bar, Cairo rendering, GDK input ──────────────────
CFLAGS  += $(shell pkg-config --cflags gtk+-3.0) -DJEMU_GTK
LDFLAGS  = $(shell pkg-config --libs   gtk+-3.0) -pthread
CHIP8_UI = jemu-chip8/src/ui_gtk.c
else
# ── SDL2 backend: plain window, no menu bar ───────────────────────────────────
CFLAGS  += $(shell sdl2-config --cflags 2>/dev/null || echo -I/usr/include/SDL2)
LDFLAGS  = $(shell sdl2-config --libs   2>/dev/null || echo -lSDL2) -pthread
CHIP8_UI = jemu-chip8/src/display_sdl.c \
           jemu-chip8/src/input_sdl.c
endif

ifdef GTK
BUILDDIR := build/gtk
else
BUILDDIR := build/sdl
endif

CHIP8_SRC := $(CHIP8_BASE) $(CHIP8_UI)
CHIP8_OBJ := $(patsubst %.c, $(BUILDDIR)/%.o, $(CORE_SRC) $(CHIP8_SRC))

.PHONY: all clean

all: bin/jemu-chip8

bin/jemu-chip8: $(CHIP8_OBJ)
	@mkdir -p bin
	$(CC) -o $@ $^ $(LDFLAGS)

$(BUILDDIR)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -rf build bin

$(CHIP8_OBJ): \
	Makefile \
	core/include/jemu/jemu.h \
	core/include/jemu/memory.h \
	core/include/jemu/cpu.h \
	core/include/jemu/device.h \
	core/include/jemu/tcg.h \
	core/include/jemu/vnc.h \
	jemu-chip8/include/chip8.h
