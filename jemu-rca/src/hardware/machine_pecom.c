#include "pecom.h"
#include "devices/vip_devices.h"
#include "jemu/jemu.h"
#include "jemu/memory.h"
#include <SDL2/SDL.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ── Memory map ──────────────────────────────────────────────────────────────
 *
 *  0x0000–0x3FFF  ROM (16 KB, read-only)
 *  0x4000–0x7FFF  RAM bank 0 (16 KB)
 *  0x8000–0xBFFF  ROM mirror (same 16 KB, read-only)
 *  0xC000–0xFFFF  RAM bank 1 (16 KB)
 *
 * Total user RAM: 32 KB.
 * ──────────────────────────────────────────────────────────────────────────── */

static inline uint8_t *pecom_ram_ptr(RcaPecom32State *s, uint16_t addr) {
    if (addr >= 0x4000u && addr < 0x8000u)
        return &s->ram[addr - 0x4000u];
    if (addr >= 0xC000u)
        return &s->ram[0x4000u + (addr - 0xC000u)];
    return NULL;
}

/* ── CPU callbacks ───────────────────────────────────────────────────────── */

static uint8_t pecom_mem_read(uint16_t addr, void *ud) {
    RcaPecom32State *s = ud;
    /* ROM visible at 0x0000–0x3FFF and mirrored at 0x8000–0xBFFF */
    if (addr < 0x4000u || (addr >= 0x8000u && addr < 0xC000u))
        return s->rom[addr & (PECOM32_ROM_SIZE - 1u)];
    uint8_t *p = pecom_ram_ptr(s, addr);
    return p ? *p : 0xFFu;
}

static void pecom_mem_write(uint16_t addr, uint8_t val, void *ud) {
    RcaPecom32State *s = ud;
    uint8_t *p = pecom_ram_ptr(s, addr);
    if (p) *p = val;
}

static uint8_t pecom_io_in(uint8_t port, void *ud) {
    RcaPecom32State *s = ud;
    if (port == 1) {
        /* IN 1: enable CDP1861 display; return pending key or 0xFF */
        cdp1861_set_display(&s->vdc, true);
        if (s->ascii_key >= 0) {
            uint8_t k = (uint8_t)s->ascii_key;
            s->ascii_key = -1;
            s->cpu.EF[3] = true;   /* EF4 deasserted: no more key */
            return k;
        }
        return 0xFFu;
    }
    return 0xFFu;
}

static void pecom_io_out(uint8_t port, uint8_t val, void *ud) {
    RcaPecom32State *s = ud;
    (void)val;
    if (port == 1)
        cdp1861_set_display(&s->vdc, false);
}

static void pecom_sync(void *ud) {
    RcaPecom32State *s = ud;
    cdp1861_sync(&s->vdc, &s->cpu);
}

static void pecom_q_out(uint8_t q, void *ud) {
    RcaPecom32State *s = ud;
    rca_pcspk_set_gate(s->speaker, q);
}

/* ── DMA callback: CDP1861 → vram ────────────────────────────────────────── */

static void pecom_dma_out(uint8_t *data, void *ud) {
    RcaPecom32State *s = ud;
    Cdp1861 *vdc = &s->vdc;

    int row = vdc->line_counter - CDP1861_FIRST_LINE;
    if (row < 0 || row >= CDP1861_DISPLAY_H) return;

    int byte_col = vdc->display_addr % CDP1861_BYTES_PER_LINE;
    uint8_t b = *data;
    for (int bit = 0; bit < 8; bit++)
        s->vram[row * CDP1861_DISPLAY_W + byte_col * 8 + bit] = (b >> (7 - bit)) & 1u;

    vdc->display_addr++;
    if (vdc->display_addr >= (CDP1861_DISPLAY_W * CDP1861_DISPLAY_H / 8))
        vdc->display_addr = 0;

    s->draw_flag = true;
}

/* ── Machine lifecycle ───────────────────────────────────────────────────── */

RcaPecom32State *rca_pecom32_create(const RcaConfig *cfg) {
    RcaPecom32State *s = calloc(1, sizeof(*s));
    if (!s) return NULL;

    s->cfg       = cfg;
    s->ascii_key = -1;

    cdp1802_init(&s->cpu, NULL, 0);
    s->cpu.mem_read  = pecom_mem_read;
    s->cpu.mem_write = pecom_mem_write;
    s->cpu.io_in     = pecom_io_in;
    s->cpu.io_out    = pecom_io_out;
    s->cpu.on_sync   = pecom_sync;
    s->cpu.q_out     = pecom_q_out;
    s->cpu.io_ud     = s;

    cdp1861_init(&s->vdc, pecom_dma_out, s);
    s->vdc.lines_total = CDP1861_PAL_LINES_TOTAL;   /* PAL: 312 lines */

    s->monitor = jemu_monitor_create();

    if (cfg->sound_hw == RCA_SOUND_PCSPK && !cfg->vnc_addr) {
        s->speaker = rca_pcspk_create(250u);
        if (!s->speaker)
            fprintf(stderr, "jemu-rca: pecom32: failed to init audio\n");
    }

    if (cfg->vnc_addr) {
        s->vnc = jemu_vnc_create(cfg->vnc_addr,
                                 CDP1861_DISPLAY_W * cfg->display_scale,
                                 CDP1861_DISPLAY_H * cfg->display_scale);
        jemu_vnc_set_colors(s->vnc, 0xFFFFFFu, 0x000000u);
    }

    /* Load ROM(s) */
    for (int i = 0; i < cfg->n_roms; i++) {
        uint32_t addr = cfg->roms[i].addr;
        if (addr >= PECOM32_ROM_SIZE) {
            fprintf(stderr, "jemu-rca: pecom32: ROM address 0x%04X out of range\n", addr);
            free(s);
            return NULL;
        }
        JemuMemory tmp = {.data = s->rom, .size = PECOM32_ROM_SIZE};
        size_t len = 0;
        if (!jemu_mem_load_file(&tmp, addr, cfg->roms[i].path, &len)) {
            fprintf(stderr, "jemu-rca: pecom32: failed to load '%s'\n", cfg->roms[i].path);
            free(s);
            return NULL;
        }
        printf("jemu-rca: %zu bytes @ 0x%04X  ← %s\n",
               len, addr, cfg->roms[i].path);
    }

    return s;
}

void rca_pecom32_reset(RcaPecom32State *s, const RcaConfig *cfg) {
    cdp1802_reset(&s->cpu);
    rca_pcspk_set_gate(s->speaker, 0);
    cdp1861_reset(&s->vdc);
    s->vdc.lines_total = CDP1861_PAL_LINES_TOTAL;
    memset(s->vram, 0, sizeof(s->vram));
    s->draw_flag = true;
    s->ascii_key = -1;
    s->cpu.EF[3] = true;   /* EF4 idle: no key pending */

    for (int i = 0; i < cfg->n_roms; i++) {
        JemuMemory tmp = {.data = s->rom, .size = PECOM32_ROM_SIZE};
        jemu_mem_load_file(&tmp, cfg->roms[i].addr, cfg->roms[i].path, NULL);
    }
}

void rca_pecom32_destroy(RcaPecom32State *s) {
    if (!s) return;
    rca_pcspk_destroy(s->speaker);
    jemu_monitor_destroy(s->monitor);
    jemu_vnc_destroy(s->vnc);
    free(s);
}

/* ── Input helpers ───────────────────────────────────────────────────────── */

static void pecom_poll_display(RcaPecom32State *s, RcaDisplay *display, bool *quit) {
    rca_display_poll(display);
    if (rca_display_should_quit(display)) {
        *quit = true;
        return;
    }

    uint32_t keysym = rca_display_pop_keysym(display);
    if (keysym) {
        int ascii = (int)keysym;
        if (ascii >= 'a' && ascii <= 'z') ascii -= 32;
        if ((ascii >= 0x20 && ascii <= 0x7e) ||
             ascii == '\r' || ascii == '\n' || ascii == '\b' || ascii == 0x7f) {
            if (ascii == '\n') ascii = '\r';
            if (ascii == 0x7f) ascii = '\b';
            s->ascii_key = ascii;
            s->cpu.EF[3] = false;   /* EF4 asserted: key waiting */
        }
    }
}

/* ── SDL run loop ────────────────────────────────────────────────────────── */

void rca_pecom32_run(RcaPecom32State *s, const RcaConfig *cfg) {
    RcaDisplay *display = rca_display_create_mono(cfg->display_type, "JEMU | Pecom 32",
                                                   CDP1861_DISPLAY_W,
                                                   CDP1861_DISPLAY_H,
                                                   cfg->display_scale,
                                                   0xFFFFFFFFu, 0xFF000000u);
    if (!display) {
        fprintf(stderr, "jemu-rca: pecom32: failed to create display\n");
        return;
    }

    if (display->run) {
        display->run(display, s, cfg);
        rca_display_destroy(display);
        return;
    }

    /* PAL: 312 lines × 14 mcycles = 4368 per frame, ~50 Hz */
    const Uint32 frame_ms = 20u;
    const int mcycles_per_frame = CDP1861_PAL_MCYCLES_PER_FRAME;

    jemu_monitor_start(s->monitor);
    SDL_StartTextInput();

    /* EF4 idle: no key */
    s->cpu.EF[3] = true;

    bool quit = false;
    while (!quit) {
        Uint32 t0 = SDL_GetTicks();

        pecom_poll_display(s, display, &quit);
        if (quit) break;

        /* VNC keyboard */
        int vnc_ascii = rca_vp601_vnc_keysym_to_ascii(jemu_vnc_pop_keysym(s->vnc));
        if (vnc_ascii >= 0) {
            s->ascii_key = vnc_ascii;
            s->cpu.EF[3] = false;
        }

        JemuMonCmd cmd;
        while ((cmd = jemu_monitor_poll(s->monitor)) != JEMU_MON_NONE) {
            if      (cmd == JEMU_MON_QUIT)  quit = true;
            else if (cmd == JEMU_MON_RESET) rca_pecom32_reset(s, cfg);
            else if (cmd == JEMU_MON_CUSTOM) jemu_monitor_unknown_command(s->monitor);
        }
        if (quit) break;

        if (jemu_monitor_is_paused(s->monitor)) {
            Uint32 elapsed = SDL_GetTicks() - t0;
            if (elapsed < frame_ms) SDL_Delay(frame_ms - elapsed);
            continue;
        }

        /* EF4: active-low key-available strobe */
        s->cpu.EF[3] = (s->ascii_key < 0);

        for (int i = 0; i < mcycles_per_frame; i++) {
            s->cpu.EF[3] = (s->ascii_key < 0);
            cdp1802_step(&s->cpu);
        }

        if (s->draw_flag) {
            rca_display_render(display, s->vram, CDP1861_DISPLAY_W, CDP1861_DISPLAY_H);
            jemu_vnc_update(s->vnc, s->vram, CDP1861_DISPLAY_W, CDP1861_DISPLAY_H);
            s->draw_flag = false;
        }

        Uint32 elapsed = SDL_GetTicks() - t0;
        if (elapsed < frame_ms) SDL_Delay(frame_ms - elapsed);
    }

    printf("jemu-rca: %llu machine cycles, %llu instructions\n",
           (unsigned long long)s->cpu.cycle_count,
           (unsigned long long)s->cpu.insn_count);

    SDL_StopTextInput();
    jemu_monitor_stop(s->monitor);
    rca_display_destroy(display);
}
