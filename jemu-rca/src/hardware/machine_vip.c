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
    /* Q output drives the audio tone generator on COSMAC VIP, not the display.
     * Audio is not yet implemented; this is a placeholder. */
    (void)q; (void)ud;
}

static uint8_t vip_io_in(uint8_t port, void *ud) {
    RcaVipState *s = ud;
    if (port == 1) {
        /*
         * IN 1 on the COSMAC VIP: the SC lines from the 1802 assert the CDP1861
         * display-enable pin.  The instruction simultaneously reads the hex keypad
         * value off the bus (0x0-0xF, or 0xFF if no key is pressed).
         */
        cdp1861_set_display(&s->vdc, true);
        return (s->key_down >= 0) ? (uint8_t)s->key_down : 0xFF;
    }
    return 0xFF;
}

static void vip_io_out(uint8_t port, uint8_t val, void *ud) {
    RcaVipState *s = ud;
    (void)val;
    if (port == 1) {
        /* OUT 1 on the COSMAC VIP disables the CDP1861 display. */
        cdp1861_set_display(&s->vdc, false);
    }
}

/* ── Machine lifecycle ───────────────────────────────────────────────────── */

static bool vip_fpb_basic_loaded(const RcaVipState *s) {
    static const uint8_t marker[] = "BASIC-REL V2.2";
    return memcmp(&s->mem[0x1025], marker, sizeof(marker) - 1) == 0;
}

static bool vip_start_addr(const RcaVipState *s, const RcaConfig *cfg,
                           uint16_t *addr) {
    if (cfg->has_start_addr) {
        *addr = cfg->start_addr;
        return true;
    }

    if (vip_fpb_basic_loaded(s)) {
        *addr = 0x1000;
        return true;
    }

    return false;
}

static void vip_apply_start_addr(RcaVipState *s, const RcaConfig *cfg) {
    uint16_t addr;
    if (!vip_start_addr(s, cfg, &addr))
        return;

    s->cpu.X = 0;
    s->cpu.P = 0;
    s->cpu.R[0] = addr;
    s->cpu.state = CDP1802_S_FETCH;
    s->cpu.exec_left = 0;
    s->cpu.idle = false;
    s->cpu.init_pending = false;
}

static void vip_step_instruction(RcaVipState *s) {
    uint64_t start = s->cpu.insn_count;
    for (int i = 0; i < 1024 && s->cpu.insn_count == start; i++) {
        s->cpu.EF[1] = s->ef2_down;
        s->cpu.EF[2] = s->ef3_down;
        cdp1802_step(&s->cpu);
    }
}

RcaVipState *rca_vip_create(const RcaConfig *cfg) {
    RcaVipState *s = calloc(1, sizeof(*s));
    if (!s) return NULL;

    cdp1802_init(&s->cpu, s->mem, VIP_MEM_SIZE);
    s->cpu.on_sync = vip_sync;
    s->cpu.q_out   = vip_q_out;
    s->cpu.io_in   = vip_io_in;
    s->cpu.io_out  = vip_io_out;
    s->cpu.io_ud   = s;

    s->monitor = jemu_monitor_create();

    cdp1861_init(&s->vdc, vip_dma_out, s);
    if (cfg->vnc_addr)
        s->vnc = jemu_vnc_create(cfg->vnc_addr,
                                 CDP1861_DISPLAY_W * cfg->display_scale,
                                 CDP1861_DISPLAY_H * cfg->display_scale);

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
    vip_apply_start_addr(s, cfg);
    return s;
}

void rca_vip_reset(RcaVipState *s, const RcaConfig *cfg) {
    cdp1802_reset(&s->cpu);
    cdp1861_reset(&s->vdc);
    memset(s->vram, 0, sizeof(s->vram));
    s->draw_flag = true;
    s->key_down  = -1;
    s->ef2_down  = false;
    s->ef3_down  = false;
    memset(s->keys, 0, sizeof(s->keys));

    JemuMemory tmp = {.data = s->mem, .size = VIP_MEM_SIZE};
    for (int i = 0; i < cfg->n_roms; i++)
        jemu_mem_load_file(&tmp, cfg->roms[i].addr, cfg->roms[i].path, NULL);

    vip_apply_start_addr(s, cfg);
}

void rca_vip_destroy(RcaVipState *s) {
    if (!s) return;
    jemu_monitor_destroy(s->monitor);
    jemu_vnc_destroy(s->vnc);
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
    jemu_monitor_start(s->monitor);

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
            if (ev.type == SDL_KEYDOWN || ev.type == SDL_KEYUP) {
                bool down = (ev.type == SDL_KEYDOWN);
                if (ev.key.keysym.sym == SDLK_TAB)
                    s->ef2_down = down;
                if (ev.key.keysym.sym == SDLK_RETURN || ev.key.keysym.sym == SDLK_KP_ENTER)
                    s->ef3_down = down;
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
        uint8_t vnc_keys[16];
        jemu_vnc_get_keys(s->vnc, vnc_keys);

        s->key_down = -1;
        for (int k = 0; k < 16; k++)
            if (s->keys[k] || vnc_keys[k]) { s->key_down = k; break; }

        /* EF4 = any key pressed */
        s->cpu.EF[3] = (s->key_down >= 0);

        JemuMonCmd cmd;
        while ((cmd = jemu_monitor_poll(s->monitor)) != JEMU_MON_NONE) {
            if      (cmd == JEMU_MON_QUIT)  quit = true;
            else if (cmd == JEMU_MON_RESET) rca_vip_reset(s, cfg);
            else if (cmd == JEMU_MON_STEP)  vip_step_instruction(s);
        }
        if (quit) break;

        if (jemu_monitor_is_paused(s->monitor)) {
            Uint32 elapsed = SDL_GetTicks() - t0;
            if (elapsed < frame_ms) SDL_Delay(frame_ms - elapsed);
            continue;
        }

        /* ── Execute one NTSC frame worth of machine cycles ── */
        for (int i = 0; i < CDP1861_MCYCLES_PER_FRAME; i++) {
            s->cpu.EF[1] = s->ef2_down;
            s->cpu.EF[2] = s->ef3_down;
            cdp1802_step(&s->cpu);
        }

        /* ── Render ── */
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

    jemu_monitor_stop(s->monitor);
    rca_display_destroy(display);
}
