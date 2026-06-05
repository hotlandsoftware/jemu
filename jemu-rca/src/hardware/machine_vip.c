#include "vip.h"
#include "jemu/jemu.h"
#include "jemu/memory.h"
#include <SDL2/SDL.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>

/* ── DMA callback: CDP1861 → vram ────────────────────────────────────────── */

static void vip_dma_out(uint8_t *data, void *ud) {
    RcaVipState *s = ud;
    Cdp1861     *vdc = &s->vdc;

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

/* ── CPU I/O callbacks ───────────────────────────────────────────────────── */

static void vip_sync(void *ud) {
    RcaVipState *s = ud;
    cdp1861_sync(&s->vdc, &s->cpu);
}

static void vip_q_out(uint8_t q, void *ud) {
    RcaVipState *s = ud;
    cdp1861_set_display(&s->vdc, q != 0);
}

static uint8_t vip_io_in(uint8_t port, void *ud) {
    RcaVipState *s = ud;
    if (port == 1) {
        /* Return currently pressed key, 0xFF if none */
        if (s->key_down >= 0) return (uint8_t)s->key_down;
        return 0xFF;
    }
    return 0xFF;
}

static void vip_io_out(uint8_t port, uint8_t val, void *ud) {
    (void)port; (void)val; (void)ud;
}

/* ── Machine lifecycle ───────────────────────────────────────────────────── */

RcaVipState *rca_vip_create(const RcaConfig *cfg) {
    RcaVipState *s = calloc(1, sizeof(*s));
    if (!s) return NULL;

    cdp1802_init(&s->cpu, s->mem, VIP_MEM_SIZE);
    s->cpu.on_sync = vip_sync;
    s->cpu.q_out   = vip_q_out;
    s->cpu.io_in   = vip_io_in;
    s->cpu.io_out  = vip_io_out;
    s->cpu.io_ud   = s;

    cdp1861_init(&s->vdc, vip_dma_out, s);

    s->key_down = -1;

    JemuMemory tmp = {.data = s->mem, .size = VIP_MEM_SIZE};
    for (int i = 0; i < cfg->n_roms; i++) {
        size_t len = 0;
        if (!jemu_mem_load_file(&tmp, cfg->roms[i].addr, cfg->roms[i].path, &len)) {
            fprintf(stderr, "jemu-rca: failed to load '%s'\n", cfg->roms[i].path);
            free(s);
            return NULL;
        }
        printf("jemu-rca: %zu bytes @ 0x%04X  ← %s\n",
               len, cfg->roms[i].addr, cfg->roms[i].path);
    }
    return s;
}

void rca_vip_reset(RcaVipState *s, const RcaConfig *cfg) {
    cdp1802_reset(&s->cpu);
    cdp1861_reset(&s->vdc);
    memset(s->vram, 0, sizeof(s->vram));
    s->draw_flag = true;
    s->key_down  = -1;
    memset(s->keys, 0, sizeof(s->keys));

    JemuMemory tmp = {.data = s->mem, .size = VIP_MEM_SIZE};
    for (int i = 0; i < cfg->n_roms; i++)
        jemu_mem_load_file(&tmp, cfg->roms[i].addr, cfg->roms[i].path, NULL);
}

void rca_vip_destroy(RcaVipState *s) {
    free(s);
}

/* ── Keyboard layout: COSMAC VIP hex keypad ──────────────────────────────── */
/*
 *  Keypad  Keyboard
 *  1 2 3 C   1 2 3 4
 *  4 5 6 D   Q W E R
 *  7 8 9 E   A S D F
 *  A 0 B F   Z X C V
 */
static const SDL_Keycode key_map[16] = {
    SDLK_x, SDLK_1, SDLK_2, SDLK_3,
    SDLK_q, SDLK_w, SDLK_e, SDLK_a,
    SDLK_s, SDLK_d, SDLK_z, SDLK_c,
    SDLK_4, SDLK_r, SDLK_f, SDLK_v,
};

/* ── SDL run loop ─────────────────────────────────────────────────────────── */

void rca_machine_run(RcaVipState *s, const RcaConfig *cfg) {
    RcaDisplay *display;
    switch (cfg->display_type) {
    case JEMU_DISPLAY_SDL:
        display = rca_display_sdl_create(cfg->display_scale);
        break;
    default:
        display = rca_display_none_create();
        break;
    }

    if (!display) {
        fprintf(stderr, "jemu-rca: failed to create display\n");
        return;
    }

    /* backends that own their own loop */
    if (display->run) {
        display->run(display, s, cfg);
        rca_display_destroy(display);
        return;
    }

    /* SDL2 run loop */
    const Uint32 frame_ms = 1000 / 60;

    bool quit = false;
    while (!quit) {
        Uint32 t0 = SDL_GetTicks();

        /* ── Input ── */
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_QUIT) { quit = true; break; }
            if (ev.type == SDL_KEYDOWN && ev.key.keysym.sym == SDLK_ESCAPE) {
                quit = true; break;
            }
            for (int k = 0; k < 16; k++) {
                if (ev.key.keysym.sym == key_map[k]) {
                    if (ev.type == SDL_KEYDOWN) {
                        s->keys[k] = 1;
                        s->key_down = k;
                    } else if (ev.type == SDL_KEYUP) {
                        s->keys[k] = 0;
                        if (s->key_down == k) s->key_down = -1;
                    }
                }
            }
        }
        if (quit) break;

        /* Sync any held key to key_down */
        s->key_down = -1;
        for (int k = 0; k < 16; k++)
            if (s->keys[k]) { s->key_down = k; break; }

        /* EF4 = any key pressed */
        s->cpu.EF[3] = (s->key_down >= 0);

        /* ── Execute one NTSC frame worth of machine cycles ── */
        for (int i = 0; i < CDP1861_MCYCLES_PER_FRAME; i++)
            cdp1802_step(&s->cpu);

        /* ── Render ── */
        if (s->draw_flag) {
            rca_display_render(display, s->vram, CDP1861_DISPLAY_W, CDP1861_DISPLAY_H);
            s->draw_flag = false;
        }

        Uint32 elapsed = SDL_GetTicks() - t0;
        if (elapsed < frame_ms) SDL_Delay(frame_ms - elapsed);
    }

    printf("jemu-rca: %llu machine cycles, %llu instructions\n",
           (unsigned long long)s->cpu.cycle_count,
           (unsigned long long)s->cpu.insn_count);

    rca_display_destroy(display);
}
