#include "nes.h"
#include "../vga/nes_sdl.h"
#include "../audio/apu2a03.h"
#include "jemu/memory.h"
#include <SDL2/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── iNES cartridge loading ──────────────────────────────────────────────── */

static bool ines_load(NesState *s, const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "nes: cannot open '%s'\n", path); return false; }

    uint8_t hdr[16];
    if (fread(hdr, 1, 16, f) != 16 ||
        hdr[0] != 'N' || hdr[1] != 'E' || hdr[2] != 'S' || hdr[3] != 0x1A) {
        fprintf(stderr, "nes: '%s' is not a valid iNES file\n", path);
        fclose(f); return false;
    }

    s->cart.prg_banks   = hdr[4];
    s->cart.chr_banks   = hdr[5];
    s->cart.mapper      = (hdr[7] & 0xF0) | (hdr[6] >> 4);
    s->cart.has_battery = (hdr[6] >> 1) & 1;

    bool four_screen = (hdr[6] >> 3) & 1;
    bool vertical    = hdr[6] & 1;
    if      (four_screen) s->cart.mirror = RP2C02_MIRROR_4SCREEN;
    else if (vertical)    s->cart.mirror = RP2C02_MIRROR_VERTICAL;
    else                  s->cart.mirror = RP2C02_MIRROR_HORIZONTAL;

    if (s->cart.mapper != 0) {
        fprintf(stderr, "nes: mapper %u not supported (only NROM/0)\n", s->cart.mapper);
        fclose(f); return false;
    }
    if (s->cart.prg_banks == 0 || s->cart.prg_banks > 2) {
        fprintf(stderr, "nes: NROM requires 1 or 2 PRG banks, got %u\n", s->cart.prg_banks);
        fclose(f); return false;
    }

    /* Skip optional 512-byte trainer */
    if ((hdr[6] >> 2) & 1) fseek(f, 512, SEEK_CUR);

    uint32_t prg_bytes = (uint32_t)s->cart.prg_banks * 0x4000;
    s->prg = malloc(prg_bytes);
    if (!s->prg || fread(s->prg, 1, prg_bytes, f) != prg_bytes) {
        fprintf(stderr, "nes: failed to read PRG ROM\n");
        fclose(f); free(s->prg); s->prg = NULL; return false;
    }

    if (s->cart.chr_banks > 0) {
        uint32_t chr_bytes = (uint32_t)s->cart.chr_banks * 0x2000;
        s->chr = malloc(chr_bytes);
        if (!s->chr || fread(s->chr, 1, chr_bytes, f) != chr_bytes) {
            fprintf(stderr, "nes: failed to read CHR ROM\n");
            fclose(f); free(s->chr); s->chr = NULL; free(s->prg); return false;
        }
        s->chr_is_ram = false;
    } else {
        /* CHR RAM: 8 KB writable by PPU */
        s->chr = calloc(1, 0x2000);
        if (!s->chr) { fclose(f); free(s->prg); return false; }
        s->chr_is_ram = true;
        s->cart.chr_banks = 1;
    }

    fclose(f);
    printf("nes: loaded '%s' — mapper %u, %u×16KB PRG, %u×8KB CHR%s\n",
           path, s->cart.mapper,
           s->cart.prg_banks, s->cart.chr_banks,
           s->chr_is_ram ? " (RAM)" : "");
    return true;
}

/* ── CHR bus callbacks (PPU address space 0x0000–0x1FFF) ─────────────────── */

static uint8_t nes_chr_read(uint16_t addr, void *ud) {
    NesState *s = ud;
    return s->chr[addr & 0x1FFF];
}

static void nes_chr_write(uint16_t addr, uint8_t val, void *ud) {
    NesState *s = ud;
    if (s->chr_is_ram) s->chr[addr & 0x1FFF] = val;
}

/* ── CPU memory map ──────────────────────────────────────────────────────── */

static uint8_t nes_cpu_read(uint16_t addr, void *ud) {
    NesState *s = ud;

    if (addr < 0x2000) return s->ram[addr & 0x07FF];

    if (addr < 0x4000) return rp2c02_read(&s->ppu, (uint8_t)(addr & 7));

    if (addr == 0x4015) return apu2a03_read(&s->apu, 0x4015);

    if (addr == 0x4016) {
        if (s->ctrl_strobe) return (s->ctrl_state[0] & NES_BTN_A) ? 1u : 0u;
        uint8_t bit = s->ctrl_shift[0] & 1;
        s->ctrl_shift[0] = (s->ctrl_shift[0] >> 1) | 0x80u;
        return bit;
    }
    if (addr == 0x4017) {
        if (s->ctrl_strobe) return (s->ctrl_state[1] & NES_BTN_A) ? 1u : 0u;
        uint8_t bit = s->ctrl_shift[1] & 1;
        s->ctrl_shift[1] = (s->ctrl_shift[1] >> 1) | 0x80u;
        return bit;
    }

    if (addr >= 0x8000) {
        /* NROM: 1 bank → mirrored; 2 banks → direct 32 KB */
        uint32_t off = addr - 0x8000u;
        if (s->cart.prg_banks == 1) off &= 0x3FFF;
        return s->prg[off];
    }

    return 0;  /* open bus */
}

static void nes_cpu_write(uint16_t addr, uint8_t val, void *ud) {
    NesState *s = ud;

    if (addr < 0x2000) { s->ram[addr & 0x07FF] = val; return; }

    if (addr < 0x4000) { rp2c02_write(&s->ppu, (uint8_t)(addr & 7), val); return; }

    if (addr == 0x4014) {
        /* OAM DMA: stall CPU 513 cycles, copy 256 bytes to OAM */
        uint8_t page[256];
        uint16_t base = (uint16_t)val << 8;
        for (int i = 0; i < 256; i++)
            page[i] = nes_cpu_read((uint16_t)(base + i), s);
        rp2c02_oam_dma(&s->ppu, page);
        s->cpu.cycle_count += 513;
        return;
    }

    if (addr == 0x4016) {
        bool new_strobe = (val & 1) != 0;
        if (s->ctrl_strobe && !new_strobe) {
            s->ctrl_shift[0] = s->ctrl_state[0];
            s->ctrl_shift[1] = s->ctrl_state[1];
        }
        s->ctrl_strobe = new_strobe;
        return;
    }

    /* APU registers */
    if ((addr >= 0x4000 && addr <= 0x4013) || addr == 0x4015 || addr == 0x4017)
        apu2a03_write(&s->apu, addr, val);
}

/* ── VNC key → controller ─────────────────────────────────────────────────── */

/* X11 keysym constants used by VNC */
#define XK_Left   0xFF51u
#define XK_Up     0xFF52u
#define XK_Right  0xFF53u
#define XK_Down   0xFF54u
#define XK_Return 0xFF0Du
#define XK_ShiftR 0xFFE2u

static void nes_handle_keys(NesState *s) {
    /* SDL display: poll events and read back full button state */
    if (s->display) {
        nes_display_poll(s->display);
        s->ctrl_state[0] = nes_display_ctrl1(s->display);
    }

    /* VNC: process queued key-press/-release events */
    if (s->vnc) {
        JemuVncKeyEvent ev;
        while (jemu_vnc_pop_key_event(s->vnc, &ev)) {
            uint8_t btn = 0;
            switch (ev.keysym) {
            case 'z': case 'Z':   btn = NES_BTN_A;      break;
            case 'x': case 'X':   btn = NES_BTN_B;      break;
            case XK_Return:        btn = NES_BTN_START;  break;
            case XK_ShiftR:        btn = NES_BTN_SELECT; break;
            case XK_Up:            btn = NES_BTN_UP;     break;
            case XK_Down:          btn = NES_BTN_DOWN;   break;
            case XK_Left:          btn = NES_BTN_LEFT;   break;
            case XK_Right:         btn = NES_BTN_RIGHT;  break;
            default: break;
            }
            if (btn) {
                if (ev.down) s->ctrl_state[0] |=  btn;
                else         s->ctrl_state[0] &= ~btn;
            }
        }
    }
}

/* ── Machine lifecycle ───────────────────────────────────────────────────── */

static void nes_reset(NesState *s) {
    rp2c02_reset(&s->ppu);
    mos6502_reset(&s->cpu);
    apu2a03_reset(&s->apu);
}

NesState *nes_create(const MosConfig *cfg) {
    NesState *s = calloc(1, sizeof(*s));
    if (!s) return NULL;

    s->cfg = cfg;

    if (!cfg->cart_path) {
        fprintf(stderr, "nes: no cartridge specified — use -cartridge FILE.nes\n");
        free(s); return NULL;
    }

    if (!ines_load(s, cfg->cart_path)) { free(s); return NULL; }

    /* Wire up PPU CHR bus */
    rp2c02_init(&s->ppu);
    s->ppu.chr_read  = nes_chr_read;
    s->ppu.chr_write = nes_chr_write;
    s->ppu.chr_ud    = s;
    s->ppu.mirror    = s->cart.mirror;

    /* Wire up CPU — always 2A03 for NES */
    mos6502_init(&s->cpu);
    s->cpu.mem_read       = nes_cpu_read;
    s->cpu.mem_write      = nes_cpu_write;
    s->cpu.mem_ud         = s;
    s->cpu.decimal_disable = true;

    /* APU — only initialise when sound is enabled */
    if (cfg->sound == MOS_SOUND_2A03) {
        if (!apu2a03_init(&s->apu))
            fprintf(stderr, "nes: APU audio init failed (continuing silently)\n");
        s->apu.mem_read = nes_cpu_read;
        s->apu.mem_ud   = s;
    }

    s->monitor = jemu_monitor_create();

    if (cfg->display_type == JEMU_DISPLAY_SDL) {
        s->display = nes_display_create("jemu-6502 — NES",
                                        rp2c02_palette_rgb,
                                        cfg->display_scale);
        if (!s->display)
            fprintf(stderr, "nes: failed to create SDL window\n");
    }

    if (cfg->vnc_addr) {
        s->vnc = jemu_vnc_create(cfg->vnc_addr, RP2C02_WIDTH, RP2C02_HEIGHT);
        if (s->vnc)
            jemu_vnc_set_palette(s->vnc, rp2c02_palette_rgb, 64);
        else
            fprintf(stderr, "nes: failed to start VNC at %s\n", cfg->vnc_addr);
    }

    nes_reset(s);
    return s;
}

void nes_destroy(NesState *s) {
    apu2a03_destroy(&s->apu);
    jemu_monitor_destroy(s->monitor);
    nes_display_destroy(s->display);
    jemu_vnc_destroy(s->vnc);
    free(s->prg);
    free(s->chr);
    free(s);
}

/* ── Run loop ────────────────────────────────────────────────────────────── */

/* Fallback frame duration when audio is off (headless / -soundhw none). */
#define NES_FRAME_MS   17u   /* 1000/60 ≈ 16.67 ms; round up to avoid running fast */

/* Audio queue threshold for sync: allow up to 3 frames of latency (~50 ms).
 * ~735 samples/frame * 3 frames * 4 bytes/float = 8820 bytes */
#define AUDIO_QUEUE_MAX  (3u * 735u * (unsigned)sizeof(float))

void nes_run(NesState *s, const MosConfig *cfg) {
    jemu_monitor_start(s->monitor);

    bool quit = false;
    while (!quit) {
        Uint32 t0 = SDL_GetTicks();

        /* SDL event pump (headless/VNC path — SDL display polls in handle_keys) */
        if (!s->display) {
            SDL_Event ev;
            while (SDL_PollEvent(&ev))
                if (ev.type == SDL_QUIT) quit = true;
        }

        /* Monitor commands */
        JemuMonCmd cmd;
        while ((cmd = jemu_monitor_poll(s->monitor)) != JEMU_MON_NONE) {
            if      (cmd == JEMU_MON_QUIT)   { quit = true; break; }
            else if (cmd == JEMU_MON_RESET)  nes_reset(s);
            else if (cmd == JEMU_MON_CUSTOM) jemu_monitor_unknown_command(s->monitor);
        }
        if (quit) break;

        /* Input: SDL display keys + VNC events */
        nes_handle_keys(s);

        /* SDL window closed */
        if (s->display && nes_display_should_quit(s->display)) break;

        if (!jemu_monitor_is_paused(s->monitor)) {
            /* Run one full frame (until PPU marks frame complete) */
            s->ppu.dirty = false;
            while (!s->ppu.dirty) {
                uint64_t prev = s->cpu.cycle_count;
                mos6502_step(&s->cpu);
                uint64_t delta = s->cpu.cycle_count - prev;

                /* APU: one tick per CPU cycle */
                if (s->apu.audio_dev)
                    for (uint64_t i = 0; i < delta; i++)
                        apu2a03_tick(&s->apu);

                for (uint64_t i = 0; i < delta * 3; i++) {
                    rp2c02_tick(&s->ppu);
                    if (s->ppu.nmi_pending) {
                        s->cpu.nmi = true;
                        s->ppu.nmi_pending = false;
                    }
                    if (s->ppu.dirty) break;
                }
            }

            /* Flush APU samples to SDL audio */
            apu2a03_flush(&s->apu);

            /* Render completed frame */
            if (s->display)
                nes_display_render(s->display, s->ppu.pixels,
                                   RP2C02_WIDTH, RP2C02_HEIGHT);
            if (s->vnc)
                jemu_vnc_update(s->vnc, s->ppu.pixels,
                                RP2C02_WIDTH, RP2C02_HEIGHT);
        }

        /* Frame sync:
         * - Audio enabled: block until SDL has consumed enough samples so the
         *   queue stays at ~3 frames. This locks emulation to the audio clock
         *   (exactly 60.099 fps) and eliminates drift entirely.
         * - Audio off / headless: fall back to a software timer. */
        if (s->apu.audio_dev) {
            while (SDL_GetQueuedAudioSize(s->apu.audio_dev) > AUDIO_QUEUE_MAX)
                SDL_Delay(1);
        } else {
            Uint32 elapsed = SDL_GetTicks() - t0;
            if (elapsed < NES_FRAME_MS)
                SDL_Delay(NES_FRAME_MS - elapsed);
        }
    }

    printf("nes: %llu frames, %llu cpu cycles\n",
           (unsigned long long)s->ppu.frame,
           (unsigned long long)s->cpu.cycle_count);

    jemu_monitor_stop(s->monitor);
    (void)cfg;
}
