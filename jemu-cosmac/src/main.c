#include "cosmac.h"
#include "jemu/jemu.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void usage(const char *prog) {
    fprintf(stderr,
        "jemu — Jaguar Emulator v" JEMU_VERSION_STR "\n"
        "Usage: %s [options] <rom.bin>\n"
        "\n"
        "Options:\n"
        "  -M TYPE      Machine type (use -M ? to list)\n"
        "  -display T   Display backend (use -display ? to list)\n"
        "  -h           Show this help\n",
        prog);
}

int main(int argc, char *argv[]) {
    if (argc < 2) { usage(argv[0]); return 1; }

    CosmacConfig cfg = {
        .rom_path    = NULL,
        .machine     = COSMAC_MACHINE_GENERIC,
        .display_type = JEMU_DISPLAY_SDL,
    };

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0) {
            usage(argv[0]); return 0;

        } else if (strcmp(argv[i], "-M") == 0 && i + 1 < argc) {
            const char *mach = argv[++i];
            if (strcmp(mach, "?") == 0 || strcmp(mach, "help") == 0) {
                printf("Available machines:\n"
                       "  generic    Generic COSMAC (default)\n");
                return 0;
            }
            if (strcmp(mach, "generic") == 0) cfg.machine = COSMAC_MACHINE_GENERIC;
            else {
                fprintf(stderr, "jemu-cosmac: unknown machine '%s' (try -M ?)\n", mach);
                return 1;
            }

        } else if (strcmp(argv[i], "-display") == 0 && i + 1 < argc) {
            const char *disp = argv[++i];
            if (strcmp(disp, "?") == 0 || strcmp(disp, "help") == 0) {
                printf("Available display types:\n"
                       "  sdl      SDL2 window (default)\n"
                       "  curses   ncurses terminal\n"
                       "  none     No display (headless)\n"
                       "  gtk      GTK3 window\n");
                return 0;
            }
            if      (strcmp(disp, "sdl")    == 0) cfg.display_type = JEMU_DISPLAY_SDL;
            else if (strcmp(disp, "curses") == 0) cfg.display_type = JEMU_DISPLAY_CURSES;
            else if (strcmp(disp, "none")   == 0) cfg.display_type = JEMU_DISPLAY_NONE;
            else if (strcmp(disp, "gtk")    == 0) cfg.display_type = JEMU_DISPLAY_GTK;
            else {
                fprintf(stderr, "jemu-cosmac: unknown display '%s' (try -display ?)\n", disp);
                return 1;
            }

        } else if (argv[i][0] != '-') {
            cfg.rom_path = argv[i];

        } else {
            fprintf(stderr, "jemu-cosmac: unknown option '%s'\n", argv[i]);
            usage(argv[0]);
            return 1;
        }
    }

    if (!cfg.rom_path) {
        fprintf(stderr, "jemu-cosmac: no ROM specified\n");
        usage(argv[0]);
        return 1;
    }

    fprintf(stderr, "jemu-cosmac: not yet implemented\n");
    return 1;
}
