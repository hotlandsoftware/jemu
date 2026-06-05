CC          := gcc
BASE_CFLAGS := -Wall -Wextra -O2 -std=c11 -Icore/include -pthread
CFLAGS      := $(BASE_CFLAGS) -Ijemu-chip8/include

CORE_SRC := \
	core/src/memory.c \
	core/src/tcg.c \
	core/src/monitor.c \
	core/src/vnc.c

CHIP8_BASE := \
	jemu-chip8/src/main.c \
	jemu-chip8/src/cpu.c \
	jemu-chip8/src/machine.c

# SDL2 and ncurses are always compiled in
CFLAGS  += $(shell sdl2-config --cflags 2>/dev/null || echo -I/usr/include/SDL2)
LDFLAGS  = $(shell sdl2-config --libs   2>/dev/null || echo -lSDL2) -lncursesw -pthread

CHIP8_UI := \
	jemu-chip8/src/display_sdl.c \
	jemu-chip8/src/input_sdl.c \
	jemu-chip8/src/display_curses.c

ifdef GTK
# GTK3 is opt-in: adds the GTK backend on top of the always-present SDL/curses
CFLAGS  += $(shell pkg-config --cflags gtk+-3.0) -DJEMU_GTK
LDFLAGS += $(shell pkg-config --libs   gtk+-3.0)
CHIP8_UI += jemu-chip8/src/ui_gtk.c
BUILDDIR := build/gtk
else
BUILDDIR := build/sdl
endif

CHIP8_SRC := $(CHIP8_BASE) $(CHIP8_UI)
CHIP8_OBJ := $(patsubst %.c, $(BUILDDIR)/%.o, $(CORE_SRC) $(CHIP8_SRC))

COSMAC_SRC := jemu-cosmac/src/main.c
COSMAC_OBJ := $(patsubst %.c, build/cosmac/%.o, $(COSMAC_SRC))

.PHONY: all clean

all: bin/jemu-chip8 bin/jemu-cosmac

bin/jemu-chip8: $(CHIP8_OBJ)
	@mkdir -p bin
	$(CC) -o $@ $^ $(LDFLAGS)

bin/jemu-cosmac: $(COSMAC_OBJ)
	@mkdir -p bin
	$(CC) -o $@ $^ -pthread

build/cosmac/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(BASE_CFLAGS) -Ijemu-cosmac/include -c -o $@ $<

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
	core/include/jemu/display.h \
	jemu-chip8/include/chip8.h

$(COSMAC_OBJ): \
	Makefile \
	core/include/jemu/jemu.h \
	core/include/jemu/display.h \
	jemu-cosmac/include/cosmac.h
