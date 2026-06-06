#include "rca.h"
#include "hardware/vip.h"
#include "hardware/destroyer.h"
#include "devices/vip_devices.h"
#include "jemu/jemu.h"
#include "jemu/args.h"
#include <SDL2/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/* ── Device registry ─────────────────────────────────────────────────────── */

static const JemuDevDesc machines[] = {
    {"cosmac-vip", "RCA COSMAC VIP (CDP1802 + CDP1861 Pixie, 2 KB RAM)"},
    {"destroyer",  "Cidelsa Destroyer arcade board (CDP1802 + CDP1869 VIS)"},
    {"generic",    "Generic RCA COSMAC (stub, not yet implemented)"},
};
static const JemuDevDesc cpus[] = {
    {"cdp1802", "RCA CDP1802 COSMAC (1.76 MHz, 8-bit accumulator, 16 × 16-bit scratchpad)"},
};
static const JemuDevDesc vgas[] = {
    {"cdp1861", "RCA CDP1861 Pixie (64×128 px, NTSC 60 Hz, DMA-driven)"},
    {"cdp1869", "RCA CDP1869/1870 VIS tile display"},
    {"none",    "No video output"},
};
static const JemuDevDesc soundhws[] = {
    {"pcspk", "Standard PC speaker / one-bit loudspeaker"},
    {"none",  "Disable sound output"},
};
static const JemuArgsDef def = {
    .prog         = "jemu-rca",
    .machines     = machines, .n_machines = 3,
    .cpus         = cpus,     .n_cpus     = 1,
    .vgas         = vgas,     .n_vgas     = 3,
    .display_mask = JEMU_DISP_F(JEMU_DISPLAY_SDL)
                  | JEMU_DISP_F(JEMU_DISPLAY_CURSES)
                  | JEMU_DISP_F(JEMU_DISPLAY_NONE),
    .vnc_support  = true,
    .extra_help =
        "\nRCA options:\n"
        "  -rom ADDR:FILE   Load a ROM/blob at address ADDR; may be repeated\n"
        "  -rom FILE        Load a ROM/blob using machine/content address detection, or 0x0000\n"
        "  -load-addr ADDR  Load positional ROM at ADDR (default 0x0000)\n"
        "  -start ADDR      Start CDP1802 execution at ADDR\n"
        "  -device NAME     Attach device      (use -device ? to list)\n"
        "  -soundhw NAME    Sound hardware     (use -soundhw ? to list)\n"
        "\nExamples:\n"
        "  ./bin/jemu-rca -rom roms/fpb_color.bin -rom roms/vip.32.rom\n"
        "  ./bin/jemu-rca -device vip-keypad -rom roms/fpb_color.bin -rom roms/vip.32.rom\n"
        "  ./bin/jemu-rca -rom 0x0000:roms/fpb_color.bin -rom 0x8000:roms/vip.32.rom\n"
        "  ./bin/jemu-rca -rom 0x0000:roms/fpb_color.bin -rom 0x8000:roms/vip.32.rom -vnc localhost:15\n"
        "  ./bin/jemu-rca -M destroyer -rom roms/destroyer/des\\ a\\ 2.ic4 -rom roms/destroyer/des\\ b\\ 2.ic5 -rom roms/destroyer/des\\ c\\ 2.ic6 -rom roms/destroyer/des\\ d\\ 2.ic7\n"
        "  ./bin/jemu-rca -start 0x1000 -rom 0x0000:roms/fpb_color.bin\n",
};

/* ── ROM load helper ─────────────────────────────────────────────────────── */

static bool add_rom(RcaConfig *cfg, uint32_t addr, const char *path) {
    if (cfg->n_roms >= RCA_MAX_ROM_LOADS) {
        fprintf(stderr, "jemu-rca: too many -rom loads (max %d)\n", RCA_MAX_ROM_LOADS);
        return false;
    }
    cfg->roms[cfg->n_roms].addr = addr;
    cfg->roms[cfg->n_roms].path = path;
    cfg->n_roms++;
    return true;
}

static bool infer_rom_addr(const char *path, uint32_t *addr) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "jemu-rca: failed to open '%s'\n", path);
        return false;
    }

    uint8_t header[3] = {0};
    size_t n = fread(header, 1, sizeof(header), f);
    fclose(f);

    /*
     * Many RCA/VIP ROM images begin with:
     *   F8 pp  LDI pp
     *   Bn     PHI Rn
     * The loaded page byte is therefore visible in the image itself.
     */
    if (n == sizeof(header) && header[0] == 0xF8 &&
        header[2] >= 0xB0 && header[2] <= 0xBF) {
        *addr = (uint32_t)header[1] << 8;
        return true;
    }

    return false;
}

/* Parse "0xADDR:path/to/file", "ADDR:path/to/file", or infer from "FILE". */
static bool parse_rom_arg(RcaConfig *cfg, const char *arg) {
    const char *colon = strchr(arg, ':');
    if (!colon) {
        uint32_t addr = 0;
        if (!infer_rom_addr(arg, &addr) && cfg->machine == RCA_MACHINE_DESTROYER)
            addr = (uint32_t)cfg->n_roms * 0x0800u;
        return add_rom(cfg, addr, arg);
    }
    if (colon == arg) {
        fprintf(stderr, "jemu-rca: -rom expects ADDR:FILE or FILE, got '%s'\n", arg);
        return false;
    }
    /* Copy address portion so strtoul can read it as a C string */
    char addr_buf[32];
    size_t addr_len = (size_t)(colon - arg);
    if (addr_len >= sizeof(addr_buf)) addr_len = sizeof(addr_buf) - 1;
    memcpy(addr_buf, arg, addr_len);
    addr_buf[addr_len] = '\0';
    uint32_t addr = (uint32_t)strtoul(addr_buf, NULL, 0);
    const char *path = colon + 1;
    if (*path == '\0') {
        fprintf(stderr, "jemu-rca: -rom missing file path in '%s'\n", arg);
        return false;
    }
    return add_rom(cfg, addr, path);
}

static void soundhw_list_print(void) {
    printf("Available RCA sound hardware:\n");
    int maxw = 0;
    for (int i = 0; i < (int)(sizeof(soundhws) / sizeof(soundhws[0])); i++) {
        int w = (int)strlen(soundhws[i].name);
        if (w > maxw) maxw = w;
    }
    for (int i = 0; i < (int)(sizeof(soundhws) / sizeof(soundhws[0])); i++)
        printf("  %-*s  %s\n", maxw, soundhws[i].name, soundhws[i].desc);
}

static bool parse_soundhw(const char *name, RcaSoundHwType *out) {
    if (strcmp(name, "pcspk") == 0) {
        *out = RCA_SOUND_PCSPK;
        return true;
    }
    if (strcmp(name, "none") == 0) {
        *out = RCA_SOUND_NONE;
        return true;
    }
    return false;
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        char *fake[] = {argv[0], "-h"};
        int rem = 0;
        jemu_args_parse(2, fake, &def, &(JemuArgs){}, &rem, NULL);
        return 1;
    }

    RcaConfig cfg = {
        .machine       = RCA_MACHINE_COSMAC_VIP,
        .cpu           = RCA_CPU_CDP1802,
        .vga           = RCA_VGA_CDP1861,
        .keyboard      = RCA_KEYBOARD_VP601,
        .sound_hw      = RCA_SOUND_PCSPK,
        .display_type  = JEMU_DISPLAY_SDL,
        .display_scale = 4,
    };

    JemuArgs args = {
        .display_type  = cfg.display_type,
        .display_scale = cfg.display_scale,
    };

    char *rem[32]; int nrem = 0;
    if (!jemu_args_parse(argc, argv, &def, &args, &nrem, rem))
        return 1;

    if (args.machine) {
        if      (strcmp(args.machine, "cosmac-vip") == 0) cfg.machine = RCA_MACHINE_COSMAC_VIP;
        else if (strcmp(args.machine, "destroyer")  == 0) cfg.machine = RCA_MACHINE_DESTROYER;
        else if (strcmp(args.machine, "generic")    == 0) cfg.machine = RCA_MACHINE_GENERIC;
    }
    if (args.vga) {
        if      (strcmp(args.vga, "cdp1861") == 0) cfg.vga = RCA_VGA_CDP1861;
        else if (strcmp(args.vga, "cdp1869") == 0) cfg.vga = RCA_VGA_CDP1869;
        else if (strcmp(args.vga, "none")    == 0) cfg.vga = RCA_VGA_NONE;
    }
    if (cfg.machine == RCA_MACHINE_DESTROYER && !args.vga)
        cfg.vga = RCA_VGA_CDP1869;
    if (cfg.machine == RCA_MACHINE_DESTROYER)
        cfg.keyboard = RCA_KEYBOARD_NONE;

    /* RCA-specific remainder flags */
    uint32_t positional_addr = 0x0000;
    for (int i = 0; i < nrem; i++) {
        if (strcmp(rem[i], "-rom") == 0 && i + 1 < nrem) {
            if (!parse_rom_arg(&cfg, rem[++i])) return 1;
        } else if (strcmp(rem[i], "-load-addr") == 0 && i + 1 < nrem) {
            positional_addr = (uint32_t)strtoul(rem[++i], NULL, 0);
        } else if (strcmp(rem[i], "-start") == 0 && i + 1 < nrem) {
            cfg.start_addr = (uint16_t)strtoul(rem[++i], NULL, 0);
            cfg.has_start_addr = true;
        } else if (strcmp(rem[i], "-device") == 0 && i + 1 < nrem) {
            const char *v = rem[++i];
            if (strcmp(v, "?") == 0 || strcmp(v, "help") == 0) {
                rca_device_list_print();
                return 0;
            }
            const RcaDeviceDesc *dev = rca_device_find(v);
            if (!dev) {
                fprintf(stderr, "jemu-rca: unknown device '%s' (try -device ?)\n", v);
                return 1;
            }
            if (!rca_device_supports_machine(dev, cfg.machine)) {
                char machines[96];
                rca_device_supported_machines(dev, machines, sizeof(machines));
                fprintf(stderr, "jemu-rca: %s device is only supported by %s\n",
                        dev->name, machines);
                return 1;
            }
            rca_device_attach(&cfg, dev->keyboard);
        } else if (strcmp(rem[i], "-soundhw") == 0) {
            if (i + 1 >= nrem) {
                fprintf(stderr, "jemu-rca: -soundhw requires an argument\n");
                return 1;
            }
            const char *v = rem[++i];
            if (strcmp(v, "?") == 0 || strcmp(v, "help") == 0) {
                soundhw_list_print();
                return 0;
            }
            if (!parse_soundhw(v, &cfg.sound_hw)) {
                fprintf(stderr, "jemu-rca: unknown sound hardware '%s' (try -soundhw ?)\n", v);
                return 1;
            }
        } else {
            fprintf(stderr, "jemu-rca: unknown option '%s' (try -h)\n", rem[i]);
            return 1;
        }
    }

    /* Positional ROM (backwards-compatible single-file usage) */
    if (args.rom_path) {
        if (!add_rom(&cfg, positional_addr, args.rom_path)) return 1;
    }

    cfg.display_type  = args.display_type;
    cfg.display_scale = args.display_scale;
    cfg.vnc_addr      = args.vnc_addr;
    if (cfg.vnc_addr)
        cfg.sound_hw = RCA_SOUND_NONE;
    if (cfg.n_roms == 0) {
        fprintf(stderr, "jemu-rca: no ROM specified — use positional arg or -rom ADDR:FILE\n");
        return 1;
    }

    /* VNC with no explicit -display → headless, matching jemu-chip8. */
    if (cfg.vnc_addr && !args.display_explicit)
        cfg.display_type = JEMU_DISPLAY_NONE;

    if (cfg.machine == RCA_MACHINE_GENERIC) {
        fprintf(stderr, "jemu-rca: generic machine not yet implemented\n");
        return 1;
    }
    if (cfg.machine == RCA_MACHINE_DESTROYER &&
        cfg.display_type == JEMU_DISPLAY_CURSES) {
        fprintf(stderr, "jemu-rca: curses display is not yet supported by destroyer\n");
        return 1;
    }

    if (cfg.display_type == JEMU_DISPLAY_SDL ||
        cfg.display_type == JEMU_DISPLAY_CURSES ||
        cfg.display_type == JEMU_DISPLAY_NONE) {
        if (SDL_Init(0) < 0) {
            fprintf(stderr, "jemu-rca: SDL_Init failed: %s\n", SDL_GetError());
            return 1;
        }
    }

    if (cfg.machine == RCA_MACHINE_DESTROYER) {
        RcaDestroyerState *s = rca_destroyer_create(&cfg);
        if (!s) {
            if (cfg.display_type == JEMU_DISPLAY_SDL ||
                cfg.display_type == JEMU_DISPLAY_CURSES ||
                cfg.display_type == JEMU_DISPLAY_NONE) SDL_Quit();
            return 1;
        }
        rca_destroyer_run(s, &cfg);
        rca_destroyer_destroy(s);
    } else {
        RcaVipState *s = rca_vip_create(&cfg);
        if (!s) {
            if (cfg.display_type == JEMU_DISPLAY_SDL ||
                cfg.display_type == JEMU_DISPLAY_CURSES ||
                cfg.display_type == JEMU_DISPLAY_NONE) SDL_Quit();
            return 1;
        }
        rca_machine_run(s, &cfg);
        rca_vip_destroy(s);
    }

    if (cfg.display_type == JEMU_DISPLAY_SDL ||
        cfg.display_type == JEMU_DISPLAY_CURSES ||
        cfg.display_type == JEMU_DISPLAY_NONE) SDL_Quit();
    return 0;
}
