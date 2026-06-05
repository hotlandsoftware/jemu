#include "chip8.h"
#include "jemu/jemu.h"
#ifndef JEMU_GTK
#include <SDL2/SDL.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void usage(const char *prog) {
    fprintf(stderr,
        "jemu — Jaguar Emulator v" JEMU_VERSION_STR "\n"
        "Usage: %s [options] <rom.ch8>\n"
        "\n"
        "Options:\n"
        "  -m SIZE    Memory size (default: 4K)\n"
        "  -cpu TYPE  CPU variant (use -cpu ? to list)\n"
        "  -vga TYPE  Display adapter (use -vga ? to list)\n"
        "  -vnc ADDR  VNC server, e.g. :0 or 127.0.0.1:1\n"
        "  -scale N   Pixel scale factor (default: 10)\n"
        "  -hz N      CPU speed instructions/sec (default: 700)\n"
        "  -h         Show this help\n",
        prog);
}

static uint32_t parse_size(const char *s) {
    char *end;
    uint32_t v = (uint32_t)strtoul(s, &end, 0);
    if      (*end == 'K' || *end == 'k') v *= 1024;
    else if (*end == 'M' || *end == 'm') v *= 1024 * 1024;
    return v ? v : CHIP8_MEM_SIZE;
}

int main(int argc, char *argv[]) {
    if (argc < 2) { usage(argv[0]); return 1; }

    Chip8Config cfg = {
        .rom_path    = NULL,
        .mem_size    = CHIP8_MEM_SIZE,
        .cpu_hz      = CHIP8_DEFAULT_HZ,
        .vga_enabled = true,
        .vga_scale   = 10,
        .quirk_shift = false,
        .quirk_jump  = false,
        .vnc_addr    = NULL,
    };
    bool vga_explicit = false;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0) {
            usage(argv[0]); return 0;

        } else if (strcmp(argv[i], "-m") == 0 && i + 1 < argc) {
            cfg.mem_size = parse_size(argv[++i]);

        } else if (strcmp(argv[i], "-cpu") == 0 && i + 1 < argc) {
            const char *cpu = argv[++i];
            if (strcmp(cpu, "?") == 0 || strcmp(cpu, "help") == 0) {
                printf("Available CPUs:\n"
                       "  chip8      CHIP-8 CPU\n");
                return 0;
            }
            if (strcmp(cpu, "chip8") != 0) {
                fprintf(stderr, "jemu-chip8: unknown cpu '%s' (try -cpu ?)\n", cpu);
                return 1;
            }

        } else if (strcmp(argv[i], "-vga") == 0 && i + 1 < argc) {
            vga_explicit = true;
            const char *vga = argv[++i];
            if (strcmp(vga, "?") == 0 || strcmp(vga, "help") == 0) {
                printf("Available VGA adapters:\n"
                       "  std        standard display (default)\n"
                       "  none       no graphics (headless)\n");
                return 0;
            }
            if (strcmp(vga, "std") == 0)        cfg.vga_enabled = true;
            else if (strcmp(vga, "none") == 0)  cfg.vga_enabled = false;
            else {
                fprintf(stderr, "jemu-chip8: unknown vga '%s' (try -vga ?)\n", vga);
                return 1;
            }

        } else if (strcmp(argv[i], "-vnc") == 0 && i + 1 < argc) {
            const char *vnc = argv[++i];
            if (strcmp(vnc, "?") == 0 || strcmp(vnc, "help") == 0) {
                printf("VNC address format:\n"
                       "  :N             listen on all interfaces, port 5900+N\n"
                       "  host:N         listen on host, port 5900+N\n"
                       "Examples: :0  127.0.0.1:0  0.0.0.0:1\n");
                return 0;
            }
            cfg.vnc_addr = vnc;

        } else if (strcmp(argv[i], "-scale") == 0 && i + 1 < argc) {
            cfg.vga_scale = atoi(argv[++i]);
            if (cfg.vga_scale < 1) cfg.vga_scale = 1;

        } else if (strcmp(argv[i], "-hz") == 0 && i + 1 < argc) {
            cfg.cpu_hz = atoi(argv[++i]);
            if (cfg.cpu_hz < 1) cfg.cpu_hz = CHIP8_DEFAULT_HZ;

        } else if (argv[i][0] != '-') {
            cfg.rom_path = argv[i];

        } else {
            fprintf(stderr, "jemu-chip8: unknown option '%s'\n", argv[i]);
            usage(argv[0]);
            return 1;
        }
    }

    if (!cfg.rom_path) {
        fprintf(stderr, "jemu-chip8: no ROM specified\n");
        usage(argv[0]);
        return 1;
    }

    /* VNC replaces the window by default; -vga std overrides */
    if (cfg.vnc_addr && !vga_explicit)
        cfg.vga_enabled = false;

#ifndef JEMU_GTK
    if (SDL_Init(0) < 0) {
        fprintf(stderr, "jemu-chip8: SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }
#endif

    Chip8State *s = chip8_machine_create(&cfg);
    if (!s) {
#ifndef JEMU_GTK
        SDL_Quit();
#endif
        return 1;
    }

    chip8_machine_run(s, &cfg);
    chip8_machine_destroy(s);

#ifndef JEMU_GTK
    SDL_Quit();
#endif
    return 0;
}
