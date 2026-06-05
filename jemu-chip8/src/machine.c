#include "chip8.h"
#include "jemu/memory.h"
#ifndef JEMU_GTK
#include <SDL2/SDL.h>
#endif
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>

static const uint8_t chip8_font[CHIP8_FONT_BYTES] = {
    0xF0, 0x90, 0x90, 0x90, 0xF0, /* 0 */
    0x20, 0x60, 0x20, 0x20, 0x70, /* 1 */
    0xF0, 0x10, 0xF0, 0x80, 0xF0, /* 2 */
    0xF0, 0x10, 0xF0, 0x10, 0xF0, /* 3 */
    0x90, 0x90, 0xF0, 0x10, 0x10, /* 4 */
    0xF0, 0x80, 0xF0, 0x10, 0xF0, /* 5 */
    0xF0, 0x80, 0xF0, 0x90, 0xF0, /* 6 */
    0xF0, 0x10, 0x20, 0x40, 0x40, /* 7 */
    0xF0, 0x90, 0xF0, 0x90, 0xF0, /* 8 */
    0xF0, 0x90, 0xF0, 0x10, 0xF0, /* 9 */
    0xF0, 0x90, 0xF0, 0x90, 0x90, /* A */
    0xE0, 0x90, 0xE0, 0x90, 0xE0, /* B */
    0xF0, 0x80, 0x80, 0x80, 0xF0, /* C */
    0xE0, 0x90, 0x90, 0x90, 0xE0, /* D */
    0xF0, 0x80, 0xF0, 0x80, 0xF0, /* E */
    0xF0, 0x80, 0xF0, 0x80, 0x80, /* F */
};

Chip8State *chip8_machine_create(const Chip8Config *cfg) {
    Chip8State *s = calloc(1, sizeof(*s));
    if (!s) return NULL;

    s->PC      = CHIP8_ROM_BASE;
    s->monitor = jemu_monitor_create();
    jemu_tb_cache_init(&s->tb_cache, free);
    if (cfg->vnc_addr)
        s->vnc = jemu_vnc_create(cfg->vnc_addr,
                                 CHIP8_DISPLAY_W * cfg->vga_scale,
                                 CHIP8_DISPLAY_H * cfg->vga_scale);

    memcpy(s->mem + CHIP8_FONT_BASE, chip8_font, CHIP8_FONT_BYTES);

    JemuMemory tmp = {.data = s->mem, .size = cfg->mem_size};
    size_t rom_len = 0;
    if (!jemu_mem_load_file(&tmp, CHIP8_ROM_BASE, cfg->rom_path, &rom_len)) {
        fprintf(stderr, "jemu-chip8: failed to load ROM '%s'\n", cfg->rom_path);
        free(s);
        return NULL;
    }
    printf("jemu-chip8: loaded %zu bytes from '%s'\n", rom_len, cfg->rom_path);
    return s;
}

void chip8_machine_reset(Chip8State *s, const Chip8Config *cfg) {
    memset(s->V,         0, sizeof(s->V));
    memset(s->stack,     0, sizeof(s->stack));
    memset(s->vram,      0, sizeof(s->vram));
    memset(s->keys,      0, sizeof(s->keys));
    memset(s->keys_prev, 0, sizeof(s->keys_prev));
    s->I = 0; s->PC = CHIP8_ROM_BASE; s->SP = 0;
    s->delay = 0; s->sound = 0;
    s->draw_flag = true; s->wait_key = false;
    s->insn_count = s->tb_hits = s->tb_misses = 0;

    jemu_tb_cache_flush(&s->tb_cache);

    memset(s->mem + CHIP8_ROM_BASE, 0, CHIP8_MEM_SIZE - CHIP8_ROM_BASE);
    JemuMemory tmp = {.data = s->mem, .size = cfg->mem_size};
    jemu_mem_load_file(&tmp, CHIP8_ROM_BASE, cfg->rom_path, NULL);

    printf("jemu-chip8: reset\n");
}

void chip8_machine_destroy(Chip8State *s) {
    if (!s) return;
    jemu_tb_cache_flush(&s->tb_cache);
    jemu_monitor_destroy(s->monitor);
    jemu_vnc_destroy(s->vnc);
    free(s);
}

/* ── Run loop ─────────────────────────────────────────────────────────────── */

void chip8_machine_run(Chip8State *s, const Chip8Config *cfg) {
    Chip8Display *display = cfg->vga_enabled
        ? chip8_display_create(cfg->vga_scale)
        : chip8_display_none_create();

    if (!display) {
        fprintf(stderr, "jemu-chip8: failed to create display\n");
        return;
    }

    jemu_monitor_start(s->monitor);

    /* Backends that own their main loop (GTK) handle everything in run(). */
    if (display->run) {
        display->run(display, s, cfg);
        jemu_monitor_stop(s->monitor);
        chip8_display_destroy(display);
        return;
    }

    /* Headless loop — no window, no SDL input (VNC-only or silent bench). */
    if (!cfg->vga_enabled) {
        struct timespec ts = {0, 1000000000 / CHIP8_TIMER_HZ};
        const int insns_frame = cfg->cpu_hz / CHIP8_TIMER_HZ;
        bool running = true;
        while (running) {
            JemuMonCmd cmd;
            while ((cmd = jemu_monitor_poll(s->monitor)) != JEMU_MON_NONE) {
                if      (cmd == JEMU_MON_QUIT)  running = false;
                else if (cmd == JEMU_MON_RESET) chip8_machine_reset(s, cfg);
                else if (cmd == JEMU_MON_STEP)  chip8_exec_single(s);
            }
            if (!running || jemu_monitor_is_paused(s->monitor)) {
                nanosleep(&ts, NULL);
                continue;
            }

            /* Poll VNC key state (edge detection for wait_key) */
            memcpy(s->keys_prev, s->keys, CHIP8_NUM_KEYS);
            jemu_vnc_get_keys(s->vnc, s->keys);

            /* LD Vx, K stall */
            if (s->wait_key) {
                for (int k = 0; k < CHIP8_NUM_KEYS; k++) {
                    if (s->keys[k] && !s->keys_prev[k]) {
                        s->V[s->wait_reg] = (uint8_t)k;
                        s->wait_key = false;
                        s->PC += 2;
                        break;
                    }
                }
                nanosleep(&ts, NULL);
                continue;
            }

            int budget = insns_frame;
            while (budget > 0 && !s->wait_key) {
                JemuTb *tb = jemu_tb_lookup(&s->tb_cache, s->PC);
                if (!tb) tb = chip8_translate_block(s, s->PC);
                if (!tb) break;
                chip8_execute_tb(s, tb);
                budget -= (int)tb->n_insns;
            }
            if (s->delay > 0) s->delay--;
            if (s->sound > 0) s->sound--;
            if (s->draw_flag) {
                jemu_vnc_update(s->vnc, s->vram, CHIP8_DISPLAY_W, CHIP8_DISPLAY_H);
                s->draw_flag = false;
            }
            nanosleep(&ts, NULL);
        }
        printf("jemu-chip8: %llu instructions executed, %llu TB hits, %llu misses\n",
               (unsigned long long)s->insn_count,
               (unsigned long long)s->tb_hits,
               (unsigned long long)s->tb_misses);
        jemu_monitor_stop(s->monitor);
        chip8_display_destroy(display);
        return;
    }

#ifndef JEMU_GTK
    /* ── SDL2 run loop ── */
    Chip8Input *input = chip8_input_create();
    if (!input) {
        fprintf(stderr, "jemu-chip8: failed to init input\n");
        chip8_display_destroy(display);
        return;
    }

    const int    insns_frame = cfg->cpu_hz / CHIP8_TIMER_HZ;
    const Uint32 frame_ms    = 1000 / CHIP8_TIMER_HZ;
    bool         quit        = false;

    while (!quit) {
        Uint32 frame_start = SDL_GetTicks();

        /* Input (always, even when paused — keeps window alive) */
        memcpy(s->keys_prev, s->keys, CHIP8_NUM_KEYS);
        chip8_input_poll(input, s->keys, &quit);
        if (quit) break;

        /* Monitor commands */
        JemuMonCmd cmd;
        while ((cmd = jemu_monitor_poll(s->monitor)) != JEMU_MON_NONE) {
            if      (cmd == JEMU_MON_QUIT)  quit = true;
            else if (cmd == JEMU_MON_RESET) chip8_machine_reset(s, cfg);
            else if (cmd == JEMU_MON_STEP)  chip8_exec_single(s);
            /* STOP/CONT: paused flag already updated by jemu_monitor_poll */
        }
        if (quit) break;

        /* Display menu actions */
        if (chip8_display_take_reset(display))
            chip8_machine_reset(s, cfg);

        /* While paused: service events but don't execute */
        if (jemu_monitor_is_paused(s->monitor)) {
            Uint32 e = SDL_GetTicks() - frame_start;
            if (e < frame_ms) SDL_Delay(frame_ms - e);
            continue;
        }

        /* LD Vx, K stall */
        if (s->wait_key) {
            for (int k = 0; k < CHIP8_NUM_KEYS; k++) {
                if (s->keys[k] && !s->keys_prev[k]) {
                    s->V[s->wait_reg] = (uint8_t)k;
                    s->wait_key = false;
                    s->PC += 2;
                    break;
                }
            }
            if (s->wait_key) {
                Uint32 e = SDL_GetTicks() - frame_start;
                if (e < frame_ms) SDL_Delay(frame_ms - e);
                continue;
            }
        }

        /* Execute instructions for this frame */
        int budget = insns_frame;
        while (budget > 0 && !s->wait_key) {
            JemuTb *tb = jemu_tb_lookup(&s->tb_cache, s->PC);
            if (!tb) tb = chip8_translate_block(s, s->PC);
            if (!tb) break;
            chip8_execute_tb(s, tb);
            budget -= (int)tb->n_insns;
        }

        if (s->delay > 0) s->delay--;
        if (s->sound > 0) s->sound--;

        if (s->draw_flag) {
            chip8_display_render(display, s->vram);
            jemu_vnc_update(s->vnc, s->vram, CHIP8_DISPLAY_W, CHIP8_DISPLAY_H);
            s->draw_flag = false;
        }

        Uint32 e = SDL_GetTicks() - frame_start;
        if (e < frame_ms) SDL_Delay(frame_ms - e);
    }

    printf("jemu-chip8: %llu instructions executed, %llu TB hits, %llu misses\n",
           (unsigned long long)s->insn_count,
           (unsigned long long)s->tb_hits,
           (unsigned long long)s->tb_misses);

    jemu_monitor_stop(s->monitor);
    chip8_input_destroy(input);
    chip8_display_destroy(display);
#endif /* !JEMU_GTK */
}
