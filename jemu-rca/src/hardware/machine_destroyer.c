#include "destroyer.h"
#include "jemu/memory.h"
#include <SDL2/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DESTRYER_CPU_HZ 3579000u
#define DESTRYER_FRAME_HZ 50u
#define DESTRYER_MCYCLES_PER_FRAME ((DESTRYER_CPU_HZ / CDP1802_CLOCKS_PER_MCYCLE) / DESTRYER_FRAME_HZ)
#define DESTRYER_PAL_LINES 312u
#define DESTRYER_PREDISPLAY_LINE 269u
#define DESTRYER_NONDISPLAY_END_LINE 268u

static const uint32_t destroyer_palette[8] = {
    0xFF000000u, 0xFF00FF00u, 0xFF0000FFu, 0xFF00FFFFu,
    0xFFFF0000u, 0xFFFFFF00u, 0xFFFF00FFu, 0xFFFFFFFFu,
};

static uint8_t destroyer_mem_read(uint16_t addr, void *ud) {
    RcaDestroyerState *s = ud;
    if (addr >= 0xf400 && addr <= 0xf7ff)
        return cdp1869_char_read(&s->vis, (uint16_t)(addr - 0xf400));
    if (addr >= 0xf800)
        return cdp1869_page_read(&s->vis, (uint16_t)(addr - 0xf800));
    return s->mem[addr];
}

static void destroyer_mem_write(uint16_t addr, uint8_t val, void *ud) {
    RcaDestroyerState *s = ud;
    if (addr <= 0x1fff)
        return;
    if (addr >= 0x2000 && addr <= 0x20ff) {
        s->mem[addr] = val;
        return;
    }
    if (addr >= 0xf400 && addr <= 0xf7ff) {
        cdp1869_char_write(&s->vis, (uint16_t)(addr - 0xf400), val);
        return;
    }
    if (addr >= 0xf800) {
        cdp1869_page_write(&s->vis, (uint16_t)(addr - 0xf800), val);
        return;
    }
    s->mem[addr] = val;
}

static uint8_t destroyer_io_in(uint8_t port, void *ud) {
    RcaDestroyerState *s = ud;
    if (port == 1)
        return (uint8_t)(s->in0 | (!s->vis.pcb ? 0x80u : 0x00u));
    if (port == 2)
        return s->in1;
    return 0xff;
}

static void destroyer_io_out(uint8_t port, uint8_t val, void *ud) {
    RcaDestroyerState *s = ud;
    if (port >= 3 && port <= 7)
        cdp1869_out(&s->vis, port, s->cpu.memory_addr, val);
}

static void destroyer_q_out(uint8_t q, void *ud) {
    RcaDestroyerState *s = ud;
    s->vis.q = q & 1u;
}

static unsigned destroyer_display_end_cycle(const Cdp1869 *vis) {
    unsigned pixel_w = vis->freshorz ? 1u : 2u;
    unsigned pixel_h = vis->fresvert ? 1u : 2u;
    unsigned non_display_end =
        (DESTRYER_MCYCLES_PER_FRAME * DESTRYER_NONDISPLAY_END_LINE) /
        DESTRYER_PAL_LINES;
    unsigned display_period =
        (DESTRYER_MCYCLES_PER_FRAME * CDP1869_VISIBLE_H) /
        DESTRYER_PAL_LINES;
    return non_display_end - (display_period / (pixel_w * pixel_h));
}

static void destroyer_video_timing(RcaDestroyerState *s, unsigned frame_cycle) {
    unsigned predisplay =
        DESTRYER_MCYCLES_PER_FRAME -
        ((DESTRYER_MCYCLES_PER_FRAME * DESTRYER_PREDISPLAY_LINE) /
         DESTRYER_PAL_LINES);
    unsigned non_display_end =
        DESTRYER_MCYCLES_PER_FRAME -
        ((DESTRYER_MCYCLES_PER_FRAME * DESTRYER_NONDISPLAY_END_LINE) /
         DESTRYER_PAL_LINES);
    unsigned display_end =
        DESTRYER_MCYCLES_PER_FRAME - destroyer_display_end_cycle(&s->vis);

    s->vis.non_display = frame_cycle < non_display_end ||
                         frame_cycle >= display_end ||
                         s->vis.dispoff;

    s->cpu.EF[0] = !(frame_cycle >= predisplay && frame_cycle < display_end &&
                     !s->vis.dispoff);
    if (frame_cycle == predisplay && !s->vis.dispoff)
        cdp1802_request_irq(&s->cpu);
}

static void destroyer_sync_inputs(RcaDestroyerState *s) {
    s->in0 = 0x7f; /* bit 7 is the active-high CDP1869 PCB callback */
    if (s->start1) s->in0 &= (uint8_t)~0x02u;
    if (s->start2) s->in0 &= (uint8_t)~0x04u;
    if (s->right)  s->in0 &= (uint8_t)~0x08u;
    if (s->left)   s->in0 &= (uint8_t)~0x10u;
    if (s->fire)   s->in0 &= (uint8_t)~0x20u;

    s->cpu.EF[1] = !s->service;
    s->cpu.EF[2] = !(s->coin2 || s->coin2_latch > 0);
    s->cpu.EF[3] = !(s->coin1 || s->coin1_latch > 0);
}

static void destroyer_default_inputs(RcaDestroyerState *s) {
    s->in0 = 0x7f;
    s->in1 = 0xd6;             /* easy, 10000 bonus, 3 lives, default coinage */
    s->service = false;
    s->coin1 = false;
    s->coin2 = false;
    s->start1 = false;
    s->start2 = false;
    s->left = false;
    s->right = false;
    s->fire = false;
    s->coin1_latch = 0;
    s->coin2_latch = 0;
}

RcaDestroyerState *rca_destroyer_create(const RcaConfig *cfg) {
    RcaDestroyerState *s = calloc(1, sizeof(*s));
    if (!s) return NULL;

    s->cfg = cfg;
    cdp1802_init(&s->cpu, s->mem, DESTRYER_MEM_SIZE);
    s->cpu.io_in = destroyer_io_in;
    s->cpu.io_out = destroyer_io_out;
    s->cpu.mem_read = destroyer_mem_read;
    s->cpu.mem_write = destroyer_mem_write;
    s->cpu.q_out = destroyer_q_out;
    s->cpu.io_ud = s;
    cdp1869_init(&s->vis);
    destroyer_default_inputs(s);
    destroyer_sync_inputs(s);

    s->monitor = jemu_monitor_create();
    if (cfg->vnc_addr)
        s->vnc = jemu_vnc_create(cfg->vnc_addr,
                                 CDP1869_VISIBLE_W * cfg->display_scale,
                                 CDP1869_VISIBLE_H * cfg->display_scale);

    JemuMemory tmp = {.data = s->mem, .size = DESTRYER_MEM_SIZE};
    for (int i = 0; i < cfg->n_roms; i++) {
        size_t len = 0;
        if (!jemu_mem_load_file(&tmp, cfg->roms[i].addr, cfg->roms[i].path, &len)) {
            fprintf(stderr, "jemu-rca: failed to load '%s'\n", cfg->roms[i].path);
            rca_destroyer_destroy(s);
            return NULL;
        }
        printf("jemu-rca: %zu bytes @ 0x%04X  <- %s\n",
               len, cfg->roms[i].addr, cfg->roms[i].path);
    }
    return s;
}

void rca_destroyer_reset(RcaDestroyerState *s, const RcaConfig *cfg) {
    memset(s->mem + 0x2000, 0, 0x100);
    cdp1802_reset(&s->cpu);
    cdp1869_reset(&s->vis);
    destroyer_default_inputs(s);
    destroyer_sync_inputs(s);

    JemuMemory tmp = {.data = s->mem, .size = DESTRYER_MEM_SIZE};
    for (int i = 0; i < cfg->n_roms; i++)
        jemu_mem_load_file(&tmp, cfg->roms[i].addr, cfg->roms[i].path, NULL);
}

void rca_destroyer_destroy(RcaDestroyerState *s) {
    if (!s) return;
    jemu_monitor_destroy(s->monitor);
    jemu_vnc_destroy(s->vnc);
    free(s);
}

static void destroyer_poll_sdl(RcaDestroyerState *s, bool *quit) {
    static int debug = -1;
    if (debug < 0)
        debug = getenv("JEMU_INPUT_DEBUG") != NULL;

    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {
        if (ev.type == SDL_QUIT) { *quit = true; return; }
        if (ev.type != SDL_KEYDOWN && ev.type != SDL_KEYUP)
            continue;
        bool down = ev.type == SDL_KEYDOWN;
        if (down && ev.key.repeat)
            continue;

        SDL_Keycode sym = ev.key.keysym.sym;
        SDL_Scancode sc = ev.key.keysym.scancode;
        const char *name = NULL;
        if (down && sym == SDLK_ESCAPE)
            *quit = true;
        else if (sym == SDLK_1 || sc == SDL_SCANCODE_1) { s->start1 = down; name = "start1"; }
        else if (sym == SDLK_2 || sc == SDL_SCANCODE_2) { s->start2 = down; name = "start2"; }
        else if (sym == SDLK_RIGHT || sc == SDL_SCANCODE_RIGHT) { s->right = down; name = "right"; }
        else if (sym == SDLK_LEFT || sc == SDL_SCANCODE_LEFT) { s->left = down; name = "left"; }
        else if (sym == SDLK_SPACE || sc == SDL_SCANCODE_SPACE) { s->fire = down; name = "fire"; }
        else if (sym == SDLK_F2 || sc == SDL_SCANCODE_F2) { s->service = down; name = "service"; }
        else if (sym == SDLK_5 || sc == SDL_SCANCODE_5) {
            s->coin1 = down;
            if (down) s->coin1_latch = 15;
            name = "coin1";
        } else if (sym == SDLK_6 || sc == SDL_SCANCODE_6) {
            s->coin2 = down;
            if (down) s->coin2_latch = 15;
            name = "coin2";
        }
        if (debug && name)
            fprintf(stderr, "destroyer input: %s %s\n", name, down ? "down" : "up");
    }
}

static void destroyer_poll_vnc(RcaDestroyerState *s) {
    uint32_t sym;
    while ((sym = jemu_vnc_pop_keysym(s->vnc)) != 0) {
        switch (sym) {
        case '1': s->start1 = true; break;
        case '2': s->start2 = true; break;
        case ' ': s->fire = true; break;
        case '5': s->coin1_latch = 15; break;
        case '6': s->coin2_latch = 15; break;
        case 0xff51: s->left = true; break;  /* Left */
        case 0xff53: s->right = true; break; /* Right */
        default: break;
        }
    }
}

void rca_destroyer_run(RcaDestroyerState *s, const RcaConfig *cfg) {
    RcaDisplay *display = NULL;
    if (cfg->display_type == JEMU_DISPLAY_SDL)
        display = rca_display_sdl_create_indexed("JEMU",
                                                 CDP1869_VISIBLE_W,
                                                 CDP1869_VISIBLE_H,
                                                 cfg->display_scale,
                                                 destroyer_palette, 8);
    else
        display = rca_display_none_create();
    if (!display) {
        fprintf(stderr, "jemu-rca: failed to create Destroyer display\n");
        return;
    }

    jemu_monitor_start(s->monitor);
    const Uint32 frame_ms = 1000 / DESTRYER_FRAME_HZ;
    bool quit = false;
    while (!quit) {
        Uint32 t0 = SDL_GetTicks();

        destroyer_poll_sdl(s, &quit);
        destroyer_poll_vnc(s);
        destroyer_sync_inputs(s);

        JemuMonCmd cmd;
        while ((cmd = jemu_monitor_poll(s->monitor)) != JEMU_MON_NONE) {
            if (cmd == JEMU_MON_QUIT) quit = true;
            else if (cmd == JEMU_MON_RESET) rca_destroyer_reset(s, cfg);
            else if (cmd == JEMU_MON_STEP) cdp1802_step(&s->cpu);
        }
        if (quit) break;

        if (!jemu_monitor_is_paused(s->monitor)) {
            for (unsigned i = 0; i < DESTRYER_MCYCLES_PER_FRAME; i++) {
                destroyer_video_timing(s, i);
                cdp1802_step(&s->cpu);
            }

            s->vis.non_display = true;
            s->cpu.EF[0] = true;
            cdp1869_render(&s->vis);
            rca_display_render(display, s->vis.bitmap,
                               CDP1869_VISIBLE_W, CDP1869_VISIBLE_H);
            jemu_vnc_update(s->vnc, s->vis.bitmap,
                            CDP1869_VISIBLE_W, CDP1869_VISIBLE_H);
        }

        Uint32 elapsed = SDL_GetTicks() - t0;
        if (s->coin1_latch > 0) s->coin1_latch--;
        if (s->coin2_latch > 0) s->coin2_latch--;
        if (elapsed < frame_ms) SDL_Delay(frame_ms - elapsed);
    }

    printf("jemu-rca: %llu machine cycles, %llu instructions\n",
           (unsigned long long)s->cpu.cycle_count,
           (unsigned long long)s->cpu.insn_count);
    jemu_monitor_stop(s->monitor);
    rca_display_destroy(display);
}
