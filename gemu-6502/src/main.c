#include "mos6502cfg.h"
#include "hardware/generic.h"
#include "hardware/nes.h"
#include "hardware/nes_devices.h"
#include "hardware/romdb.h"
#include "gemu/gemu.h"
#include "gemu/args.h"
#include <SDL2/SDL.h>
#include <sys/stat.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/* ── Device registry ─────────────────────────────────────────────────────── */

static const GemuDevDesc machines[] = {
    {"generic", "Generic MOS 6502 (flat 64 KB, ROM at user-specified address)"},
    {"nes",     "Nintendo Entertainment System (Ricoh 2A03 + RP2C02, Mapper 0/1)"},
};
static const GemuDevDesc cpus[] = {
    {"6502", "MOS Technology 6502"},
    {"2a03", "Ricoh 2A03 (NES CPU — 6502 without decimal mode)"},
};
static const GemuDevDesc vgas[] = {
    {"2c02", "Ricoh RP2C02 (NES PPU — 256x240 NTSC)"},
};

static const GemuArgsDef def = {
    .prog       = "gemu-6502",
    .machines   = machines, .n_machines = (int)GEMU_ARRAY_LEN(machines),
    .cpus       = cpus,     .n_cpus     = (int)GEMU_ARRAY_LEN(cpus),
    .vgas       = vgas,     .n_vgas     = (int)GEMU_ARRAY_LEN(vgas),
    .display_mask = GEMU_DISP_F(GEMU_DISPLAY_SDL)
#ifdef GEMU_GTK
                  | GEMU_DISP_F(GEMU_DISPLAY_GTK)
#endif
                  | GEMU_DISP_F(GEMU_DISPLAY_NONE),
    .vnc_support  = true,
    .extra_help =
        "\n6502 options:\n"
        "  -rom ADDR:FILE     Load a ROM image at CPU address ADDR\n"
        "  -rom FILE          Load a ROM image; infer address or default to 0x0000\n"
        "  -start ADDR        Override reset vector and start execution at ADDR\n"
        "\nNES options:\n"
        "  -cartridge FILE    Load an iNES (.nes) cartridge (requires -M nes)\n"
        "  -soundhw CHIP      Sound hardware: none | 2a03  (default: 2a03 for NES)\n"
        "  -device NAME       Attach a device to the next controller port (use -device ? to list)\n"
        "\nExample commands:\n"
        "  ./bin/gemu-6502 -M generic -rom 0xE000:rom.bin\n"
        "  ./bin/gemu-6502 -rom 0x0000:6502_functional_test.bin -start 0x0400\n"
        "  ./bin/gemu-6502 -M nes -cartridge game.nes -vnc :1\n",
};

/* ── ROM argument parsing ────────────────────────────────────────────────── */

static bool add_rom(MosConfig *cfg, uint32_t addr, const char *path) {
    if (cfg->n_roms >= MOS_MAX_ROM_LOADS) {
        fprintf(stderr, "gemu-6502: too many -rom loads (max %d)\n", MOS_MAX_ROM_LOADS);
        return false;
    }
    cfg->roms[cfg->n_roms].addr = addr;
    cfg->roms[cfg->n_roms].path = path;
    cfg->n_roms++;
    return true;
}

static bool parse_rom_arg(MosConfig *cfg, const char *arg) {
    uint32_t addr = 0;
    const char *path;
    if (gemu_parse_addr_arg("gemu-6502", arg, &addr, &path) < 0) return false;
    return add_rom(cfg, addr, path);
}

/* ── main ────────────────────────────────────────────────────────────────── */

int main(int argc, char *argv[]) {
    if (argc < 2) {
        char *fake[] = {argv[0], "-h"};
        int rem = 0;
        gemu_args_parse(2, fake, &def, &(GemuArgs){}, &rem, NULL);
        return 1;
    }

    MosConfig cfg = {
        .machine      = MOS_MACHINE_GENERIC,
        .cpu          = MOS_CPU_6502,
        .vga          = MOS_VGA_NONE,
        .display_type = GEMU_DISPLAY_NONE,
        .display_scale = 1,
    };

    GemuArgs args = {
        .display_type  = cfg.display_type,
        .display_scale = cfg.display_scale,
    };

    char *rem[32]; int nrem = 0;
    if (!gemu_args_parse(argc, argv, &def, &args, &nrem, rem))
        return 1;

    if (args.machine) {
        if      (strcmp(args.machine, "generic") == 0) cfg.machine = MOS_MACHINE_GENERIC;
        else if (strcmp(args.machine, "nes")     == 0) cfg.machine = MOS_MACHINE_NES;
    }

    if (args.cpu) {
        if      (strcmp(args.cpu, "6502") == 0) cfg.cpu = MOS_CPU_6502;
        else if (strcmp(args.cpu, "2a03") == 0) cfg.cpu = MOS_CPU_2A03;
    }

    if (args.vga) {
        if (strcmp(args.vga, "2c02") == 0) cfg.vga = MOS_VGA_RP2C02;
    }

    /* 6502-specific flags */
    for (int i = 0; i < nrem; i++) {
        if (strcmp(rem[i], "-rom") == 0 && i + 1 < nrem) {
            const char *val = rem[++i];
            struct stat st;
            if (stat(val, &st) == 0 && S_ISDIR(st.st_mode)) {
                const char *alias = args.machine ? args.machine : "generic";
                int n = mos_romdb_load_dir(&cfg, val, alias);
                if (n < 0) return 1;
                if (n == 0) {
                    fprintf(stderr, "gemu-6502: no known ROMs in '%s' for machine '%s'\n",
                            val, alias);
                    return 1;
                }
            } else {
                if (!parse_rom_arg(&cfg, val)) return 1;
            }
        } else if (strcmp(rem[i], "-start") == 0 && i + 1 < nrem) {
            cfg.start_addr     = (uint16_t)strtoul(rem[++i], NULL, 0);
            cfg.has_start_addr = true;
        } else if (strcmp(rem[i], "-cartridge") == 0 && i + 1 < nrem) {
            cfg.cart_path = rem[++i];
        } else if (strcmp(rem[i], "-device") == 0 && i + 1 < nrem) {
            const char *name = rem[++i];
            if (strcmp(name, "?") == 0) {
                nes_device_list_print();
                SDL_Quit(); return 0;
            }
            const NesDeviceDesc *dev = nes_device_find(name);
            if (!dev) {
                fprintf(stderr, "gemu-6502: unknown device '%s' (try -device ?)\n", name);
                return 1;
            }
            if (cfg.n_ports >= NES_PORTS) {
                fprintf(stderr, "gemu-6502: all %d controller ports are already occupied\n", NES_PORTS);
                return 1;
            }
            cfg.ports[cfg.n_ports++] = dev->type;
        } else if (strcmp(rem[i], "-soundhw") == 0 && i + 1 < nrem) {
            const char *hw = rem[++i];
            if (strcmp(hw, "?") == 0) {
                printf("Available sound hardware:\n");
                printf("  2a03  Ricoh 2A03 APU (NES pulse x2, triangle, noise, DMC)\n");
                printf("  none  Disable sound output\n");
                SDL_Quit(); return 0;
            }
            if      (strcmp(hw, "none") == 0) { cfg.sound = MOS_SOUND_NONE;  cfg.sound_explicit = true; }
            else if (strcmp(hw, "2a03") == 0) { cfg.sound = MOS_SOUND_2A03; cfg.sound_explicit = true; }
            else {
                fprintf(stderr, "gemu-6502: unknown -soundhw '%s' (use -soundhw ? to list)\n", hw);
                return 1;
            }
        } else {
            fprintf(stderr, "gemu-6502: unknown option '%s' (try -h)\n", rem[i]);
            return 1;
        }
    }

    /* Positional ROM (single-file usage: gemu-6502 rom.bin) */
    if (args.rom_path) {
        if (!add_rom(&cfg, 0x0000u, args.rom_path)) return 1;
    }

    cfg.display_type  = args.display_type;
    cfg.display_scale = args.display_scale;
    cfg.vnc_addr      = args.vnc_addr;

    /* NES without any display preference → GTK if available, else SDL */
    if (cfg.machine == MOS_MACHINE_NES
        && !cfg.vnc_addr && !args.display_explicit) {
#ifdef GEMU_GTK
        cfg.display_type = GEMU_DISPLAY_GTK;
#else
        cfg.display_type = GEMU_DISPLAY_SDL;
#endif
    }

    /* NES default: standard controller on port 0 if no -device was given */
    if (cfg.machine == MOS_MACHINE_NES && cfg.n_ports == 0)
        cfg.ports[0] = NES_DEVICE_CONTROLLER;

    /* NES default sound: 2A03 when any output is active; silent when headless.
     * Skipped if the user already chose -soundhw explicitly. */
    if (cfg.machine == MOS_MACHINE_NES && !cfg.sound_explicit && cfg.cart_path) {
        bool has_output = (cfg.display_type == GEMU_DISPLAY_SDL ||
                           cfg.display_type == GEMU_DISPLAY_GTK);
        if (has_output)
            cfg.sound = MOS_SOUND_2A03;
    }

    /* Validate machine-specific requirements */
    if (cfg.machine == MOS_MACHINE_NES) {
        if (!cfg.cart_path) {
            fprintf(stderr, "gemu-6502: NES requires -cartridge FILE.nes\n");
            return 1;
        }
    } else {
        if (cfg.n_roms == 0) {
            fprintf(stderr, "gemu-6502: no ROM specified — use -rom ADDR:FILE\n");
            return 1;
        }
    }

    if (SDL_Init(0) < 0) {
        fprintf(stderr, "gemu-6502: SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }

    int rc = 0;
    if (cfg.machine == MOS_MACHINE_NES) {
        NesState *s = nes_create(&cfg);
        if (!s) { SDL_Quit(); return 1; }
        nes_run(s, &cfg);
        nes_destroy(s);
    } else {
        MosGenericState *s = mos_generic_create(&cfg);
        if (!s) { SDL_Quit(); return 1; }
        mos_generic_run(s, &cfg);
        mos_generic_destroy(s);
    }

    SDL_Quit();
    return rc;
}
