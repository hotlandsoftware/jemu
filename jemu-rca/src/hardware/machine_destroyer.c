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
/* Lines-from-end constants matching emma02's nonDisplayPeriodEndLine_/preDisplayPeriodLine_ */
#define DESTRYER_PREDISPLAY_LINE     269u   /* EF1 asserts 1 line before display */
#define DESTRYER_NONDISPLAY_END_LINE 268u   /* non-display ends; active display begins */

static uint32_t destroyer_palette[72] = {
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
    static int debug = -1;
    static uint8_t last_in0 = 0;
    static uint8_t last_in1 = 0;

    if (debug < 0)
        debug = getenv("JEMU_DESTROYER_DEBUG") != NULL;

    if (port == 1) {
        uint8_t v = (uint8_t)(s->in0 | (!s->vis.pcb ? 0x80u : 0x00u));
        if (debug && v != last_in0) {
            fprintf(stderr,
                    "destroyer port1: %02x pcb=%u start1=%u start2=%u left=%u right=%u fire=%u\n",
                    v, s->vis.pcb, s->start1, s->start2, s->left, s->right, s->fire);
            last_in0 = v;
        }
        return v;
    }
    if (port == 2) {
        if (debug && s->in1 != last_in1) {
            fprintf(stderr, "destroyer port2: %02x\n", s->in1);
            last_in1 = s->in1;
        }
        return s->in1;
    }
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

static uint32_t cdp1869_rgb(int color, int luma) {
    int y = 0;
    y += (luma & 4) ? 30 : 0;
    y += (luma & 1) ? 59 : 0;
    y += (luma & 2) ? 11 : 0;
    y = (y * 255) / 100;

    uint32_t r = (color & 4) ? (uint32_t)y : 0;
    uint32_t g = (color & 1) ? (uint32_t)y : 0;
    uint32_t b = (color & 2) ? (uint32_t)y : 0;
    return 0xff000000u | (r << 16) | (g << 8) | b;
}

static void destroyer_init_palette(void) {
    static bool init = false;
    if (init)
        return;

    for (int i = 0; i < 8; i++)
        destroyer_palette[i] = cdp1869_rgb(i, 15);
    for (int c = 0, i = 8; c < 8; c++)
        for (int l = 0; l < 8; l++, i++)
            destroyer_palette[i] = cdp1869_rgb(c, l);
    init = true;
}

/*
 * Return the cycle (counting from frame start) at which the active display ends.
 * Accounts for 2× pixel scaling (fresvert/freshorz): the CDP1869 doubles each
 * character row/column when those flags are clear, halving the number of unique
 * scan lines that need to be rendered and therefore the display window in cycles.
 */
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
    /* All cycle values count from frame start (0 … MCYCLES_PER_FRAME-1). */
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

    /* non_display: true during VBlank so CPU can access page/char RAM */
    s->vis.non_display = frame_cycle < non_display_end ||
                         frame_cycle >= display_end ||
                         s->vis.dispoff;

    /* EF1 (active-low) goes asserted one line before display starts */
    s->cpu.EF[0] = !(frame_cycle >= predisplay && frame_cycle < display_end &&
                     !s->vis.dispoff);

    /* IRQ fires at the START of VBlank (= end of active display) */
    if (frame_cycle == display_end && !s->vis.dispoff)
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
    destroyer_init_palette();
    cdp1802_init(&s->cpu, s->mem, DESTRYER_MEM_SIZE);
    s->cpu.io_in = destroyer_io_in;
    s->cpu.io_out = destroyer_io_out;
    s->cpu.mem_read = destroyer_mem_read;
    s->cpu.mem_write = destroyer_mem_write;
    s->cpu.q_out = destroyer_q_out;
    s->cpu.io_ud = s;
    cdp1869_init(&s->vis);
    cdp1869_set_page_ram_mask(&s->vis, 0x03ffu);
    destroyer_default_inputs(s);
    destroyer_sync_inputs(s);

    s->monitor = jemu_monitor_create();
    if (cfg->vnc_addr) {
        int w = (cfg->vga == RCA_VGA_CDP1869) ? CDP1869_VISIBLE_W : CDP1861_DISPLAY_W;
        int h = (cfg->vga == RCA_VGA_CDP1869) ? CDP1869_VISIBLE_H : CDP1861_DISPLAY_H;
        s->vnc = jemu_vnc_create(cfg->vnc_addr, w * cfg->display_scale,
                                 h * cfg->display_scale);
        if (cfg->vga == RCA_VGA_CDP1869)
            jemu_vnc_set_palette(s->vnc, destroyer_palette,
                                 (int)(sizeof(destroyer_palette) /
                                       sizeof(destroyer_palette[0])));
    }

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
        else if (sym == SDLK_5 || sc == SDL_SCANCODE_5 ||
                 sym == SDLK_a || sc == SDL_SCANCODE_A) {
            s->coin1 = down;
            if (down) s->coin1_latch = 15;
            name = "coin1";
        } else if (sym == SDLK_6 || sc == SDL_SCANCODE_6 ||
                   sym == SDLK_b || sc == SDL_SCANCODE_B) {
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
        case '5':
        case 'a':
        case 'A':
            s->coin1_latch = 15;
            break;
        case '6':
        case 'b':
        case 'B':
            s->coin2_latch = 15;
            break;
        case 0xff51: s->left = true; break;  /* Left */
        case 0xff53: s->right = true; break; /* Right */
        default: break;
        }
    }
}

void rca_destroyer_run(RcaDestroyerState *s, const RcaConfig *cfg) {
    RcaDisplay *display = NULL;
    if (cfg->display_type == JEMU_DISPLAY_SDL) {
        if (cfg->vga == RCA_VGA_CDP1869)
            display = rca_display_sdl_create_indexed("JEMU",
                                                     CDP1869_VISIBLE_W,
                                                     CDP1869_VISIBLE_H,
                                                     cfg->display_scale,
                                                     destroyer_palette,
                                                     (int)(sizeof(destroyer_palette) /
                                                           sizeof(destroyer_palette[0])));
        else if (cfg->vga == RCA_VGA_NONE)
            display = rca_display_none_create();
        else
            display = rca_display_sdl_create(cfg->display_scale);
    } else {
        display = rca_display_none_create();
    }
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
            if (cfg->vga == RCA_VGA_CDP1869) {
                cdp1869_render(&s->vis);
                rca_display_render(display, s->vis.bitmap,
                                   CDP1869_VISIBLE_W, CDP1869_VISIBLE_H);
                jemu_vnc_update(s->vnc, s->vis.bitmap,
                                CDP1869_VISIBLE_W, CDP1869_VISIBLE_H);
            } else if (cfg->vga == RCA_VGA_CDP1861) {
                static const uint8_t blank[CDP1861_DISPLAY_W * CDP1861_DISPLAY_H];
                rca_display_render(display, blank,
                                   CDP1861_DISPLAY_W, CDP1861_DISPLAY_H);
                jemu_vnc_update(s->vnc, blank,
                                CDP1861_DISPLAY_W, CDP1861_DISPLAY_H);
            }
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
