#include "chip8.h"
#include "jemu/jemu.h"
#include "jemu/args.h"
#include <SDL2/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── Device registry ─────────────────────────────────────────────────────── */

static const JemuDevDesc machines[] = {
    {"generic", "Generic CHIP-8 interpreter (modern quirk mix, compatible with most ROMs)"},
};
static const JemuDevDesc cpus[] = {
    {"chip8", "CHIP-8 bytecode interpreter (TCG-accelerated)"},
};
static const JemuArgsDef def = {
    .prog       = "jemu-chip8",
    .machines   = machines,  .n_machines = 1,
    .cpus       = cpus,      .n_cpus     = 1,
    .vgas       = NULL,      .n_vgas     = 0,
    .display_mask = JEMU_DISP_F(JEMU_DISPLAY_SDL)
                  | JEMU_DISP_F(JEMU_DISPLAY_CURSES)
                  | JEMU_DISP_F(JEMU_DISPLAY_NONE)
#ifdef JEMU_GTK
                  | JEMU_DISP_F(JEMU_DISPLAY_GTK)
#endif
    ,
    .vnc_support = true,
};

/* ── Chip8-specific extra flags ──────────────────────────────────────────── */

static uint32_t parse_size(const char *s) {
    char *end;
    uint32_t v = (uint32_t)strtoul(s, &end, 0);
    if      (*end == 'K' || *end == 'k') v *= 1024;
    else if (*end == 'M' || *end == 'm') v *= 1024 * 1024;
    return v ? v : CHIP8_MEM_SIZE;
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        /* jemu_args_parse won't handle empty argv; print_usage manually */
        char *fake[] = {argv[0], "-h"};
        int rem = 0;
        jemu_args_parse(2, fake, &def, &(JemuArgs){}, &rem, NULL);
        return 1;
    }

    Chip8Config cfg = {
        .mem_size      = CHIP8_MEM_SIZE,
        .cpu_hz        = CHIP8_DEFAULT_HZ,
        .display_type  = JEMU_DISPLAY_SDL,
        .display_scale = 10,
        .quirk_shift   = false,
        .quirk_jump    = false,
    };

    JemuArgs args = {
        .display_type  = cfg.display_type,
        .display_scale = cfg.display_scale,
    };

    char *rem[32]; int nrem = 0;
    if (!jemu_args_parse(argc, argv, &def, &args, &nrem, rem))
        return 1;

    /* Apply shared results */
    cfg.rom_path     = args.rom_path;
    cfg.display_type = args.display_type;
    cfg.display_scale = args.display_scale;
    cfg.vnc_addr     = args.vnc_addr;
    /* -M generic is the only option; nothing to switch on */

    /* Binary-specific remainder flags */
    for (int i = 0; i < nrem; i++) {
        if (strcmp(rem[i], "-hz") == 0 && i + 1 < nrem) {
            int hz = atoi(rem[++i]);
            cfg.cpu_hz = (hz > 0) ? hz : CHIP8_DEFAULT_HZ;
        } else if (strcmp(rem[i], "-m") == 0 && i + 1 < nrem) {
            cfg.mem_size = parse_size(rem[++i]);
        } else if (strcmp(rem[i], "-quirk-shift") == 0) {
            cfg.quirk_shift = true;
        } else if (strcmp(rem[i], "-quirk-jump") == 0) {
            cfg.quirk_jump = true;
        } else {
            fprintf(stderr, "jemu-chip8: unknown option '%s' (try -h)\n", rem[i]);
            return 1;
        }
    }

    if (!cfg.rom_path) {
        fprintf(stderr, "jemu-chip8: no ROM specified\n");
        return 1;
    }

    /* VNC with no explicit -display → headless */
    if (cfg.vnc_addr && !args.display_explicit)
        cfg.display_type = JEMU_DISPLAY_NONE;

    if (cfg.display_type == JEMU_DISPLAY_SDL) {
        if (SDL_Init(0) < 0) {
            fprintf(stderr, "jemu-chip8: SDL_Init failed: %s\n", SDL_GetError());
            return 1;
        }
    }

    Chip8State *s = chip8_machine_create(&cfg);
    if (!s) {
        if (cfg.display_type == JEMU_DISPLAY_SDL) SDL_Quit();
        return 1;
    }

    chip8_machine_run(s, &cfg);
    chip8_machine_destroy(s);

    if (cfg.display_type == JEMU_DISPLAY_SDL) SDL_Quit();
    return 0;
}
