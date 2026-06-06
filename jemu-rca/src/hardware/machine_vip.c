#include "vip.h"
#include "devices/vip_devices.h"
#include "jemu/jemu.h"
#include "jemu/memory.h"
#include <SDL2/SDL.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <signal.h>
#include <time.h>

static RcaVipState *crash_state;

static const uint32_t vip_vis_palette[8] = {
    0xFF000000u, 0xFF00FF00u, 0xFF0000FFu, 0xFF00FFFFu,
    0xFFFF0000u, 0xFFFFFF00u, 0xFFFF00FFu, 0xFFFFFFFFu,
};

static const char *cpu_state_name(Cdp1802CycleState state) {
    switch (state) {
    case CDP1802_S_EXECUTE:   return "EXECUTE";
    case CDP1802_S_FETCH:     return "FETCH";
    case CDP1802_S_DMA:       return "DMA";
    case CDP1802_S_INTERRUPT: return "INTERRUPT";
    default:                  return "INVALID";
    }
}

static uint8_t mem8(const RcaVipState *s, uint16_t addr) {
    return s->mem[addr & (VIP_MEM_SIZE - 1u)];
}

static bool vip_ascii_pending(const RcaVipState *s) {
    return rca_vip_has_vp601(s->cfg) && s->ascii_key >= 0;
}

static void vip_dump_state(const RcaVipState *s, const char *reason) {
    if (!s) return;

    const Cdp1802 *c = &s->cpu;
    uint16_t rp = c->R[c->P];
    uint16_t rx = c->R[c->X];
    uint16_t op_addr = (c->state == CDP1802_S_EXECUTE) ? (uint16_t)(rp - 1u) : rp;
    uint8_t op = mem8(s, op_addr);
    unsigned vram_on = 0;
    for (unsigned i = 0; i < sizeof(s->vram); i++)
        vram_on += s->vram[i] != 0;

    fprintf(stderr, "\n==== JEMU RCA fatal dump ====\n");
    fprintf(stderr, "reason: %s\n", reason ? reason : "unknown");
    fprintf(stderr, "cycles=%llu instructions=%llu state=%s idle=%d exec_left=%d\n",
            (unsigned long long)c->cycle_count,
            (unsigned long long)c->insn_count,
            cpu_state_name(c->state), c->idle, c->exec_left);
    fprintf(stderr, "P=%X X=%X I=%X N=%X opcode=%02X @ %04X D=%02X DF=%u B=%02X T=%02X Q=%u IE=%u\n",
            c->P, c->X, c->I, c->N, op, op_addr, c->D, c->DF, c->B, c->T, c->Q, c->IE);
    fprintf(stderr, "EF1=%u EF2=%u EF3=%u EF4=%u RP=%04X RX=%04X\n",
            c->EF[0], c->EF[1], c->EF[2], c->EF[3], rp, rx);

    for (int i = 0; i < 16; i += 4) {
        fprintf(stderr, "R%X=%04X R%X=%04X R%X=%04X R%X=%04X\n",
                i, c->R[i], i + 1, c->R[i + 1],
                i + 2, c->R[i + 2], i + 3, c->R[i + 3]);
    }

    fprintf(stderr, "pending: dma_in=%d dma_out=%d irq=%d dma_count=%u dma_head=%u\n",
            c->dma_in_pending, c->dma_out_pending, c->irq_pending,
            c->dma_count, c->dma_head);
    fprintf(stderr, "pixie: display_on=%d line=%u mcycle=%u display_addr=%u int_fired=%d draw=%d vram_on=%u\n",
            s->vdc.display_on, s->vdc.line_counter, s->vdc.mcycle,
            s->vdc.display_addr, s->vdc.int_fired, s->draw_flag, vram_on);
    fprintf(stderr, "input: keyboard=%s key_down=%d ascii_key=%d ef2_down=%d\n",
            s->cfg ? rca_vip_keyboard_name(s->cfg->keyboard) : "unknown",
            s->key_down, s->ascii_key, s->ef2_down);

    fprintf(stderr, "roms:\n");
    if (s->cfg) {
        for (int i = 0; i < s->cfg->n_roms; i++)
            fprintf(stderr, "  %04X  %s\n",
                    (unsigned)s->cfg->roms[i].addr, s->cfg->roms[i].path);
    }

    fprintf(stderr, "memory around opcode:\n");
    uint16_t base = (uint16_t)(op_addr - 8u);
    for (int row = 0; row < 2; row++) {
        uint16_t addr = (uint16_t)(base + row * 16);
        fprintf(stderr, "  %04X:", addr);
        for (int col = 0; col < 16; col++)
            fprintf(stderr, " %02X", mem8(s, (uint16_t)(addr + col)));
        fprintf(stderr, "\n");
    }
    fprintf(stderr, "=============================\n");
}

static void vip_cpu_panic(Cdp1802 *cpu, const char *reason, void *ud) {
    (void)cpu;
    vip_dump_state((const RcaVipState *)ud, reason);
    exit(1);
}

static void vip_signal_handler(int sig) {
    char reason[64];
    snprintf(reason, sizeof(reason), "host signal %d", sig);
    vip_dump_state(crash_state, reason);
    fflush(stderr);
    _Exit(128 + sig);
}

static void vip_install_crash_handlers(RcaVipState *s) {
    crash_state = s;
    signal(SIGSEGV, vip_signal_handler);
    signal(SIGABRT, vip_signal_handler);
    signal(SIGFPE,  vip_signal_handler);
    signal(SIGILL,  vip_signal_handler);
#ifdef SIGBUS
    signal(SIGBUS,  vip_signal_handler);
#endif
}

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

static uint8_t vip_mem_read(uint16_t addr, void *ud) {
    RcaVipState *s = ud;
    if (s->cfg->vga == RCA_VGA_CDP1869) {
        if (addr >= 0xf400 && addr <= 0xf7ff)
            return cdp1869_char_read(&s->vis, (uint16_t)(addr - 0xf400));
        if (addr >= 0xf800)
            return cdp1869_page_read(&s->vis, (uint16_t)(addr - 0xf800));
    }
    return s->mem[addr];
}

static void vip_mem_write(uint16_t addr, uint8_t val, void *ud) {
    RcaVipState *s = ud;
    if (s->cfg->vga == RCA_VGA_CDP1869) {
        if (addr >= 0xf400 && addr <= 0xf7ff) {
            cdp1869_char_write(&s->vis, (uint16_t)(addr - 0xf400), val);
            return;
        }
        if (addr >= 0xf800) {
            cdp1869_page_write(&s->vis, (uint16_t)(addr - 0xf800), val);
            return;
        }
    }
    s->mem[addr] = val;
}

static void vip_sync(void *ud) {
    RcaVipState *s = ud;
    if (s->cfg->vga == RCA_VGA_CDP1861)
        cdp1861_sync(&s->vdc, &s->cpu);
    else if (s->cfg->vga == RCA_VGA_CDP1869)
        cdp1869_sync(&s->vis, &s->cpu);
}

static void vip_q_out(uint8_t q, void *ud) {
    RcaVipState *s = ud;
    if (s->cfg->vga == RCA_VGA_CDP1869)
        s->vis.q = q & 1u;
}

static uint8_t vip_io_in(uint8_t port, void *ud) {
    RcaVipState *s = ud;
    if (port == 1) {
        /*
         * IN 1 on the COSMAC VIP: the SC lines from the 1802 assert the CDP1861
         * display-enable pin.  The instruction simultaneously reads the hex keypad
         * value off the bus (0x0-0xF, or 0xFF if no key is pressed).
         */
        if (s->cfg->vga == RCA_VGA_CDP1861)
            cdp1861_set_display(&s->vdc, true);
        if (!rca_vip_has_keypad(s->cfg))
            return 0xFF;
        return (s->key_down >= 0) ? (uint8_t)s->key_down : 0xFF;
    }
    if (port == 3) {
        /* IN 3: ASCII keyboard (for BASIC/text programs).
         * EF4 is the active-low strobe; reading the port consumes the key. */
        if (!rca_vip_has_vp601(s->cfg))
            return 0xFF;
        if (s->ascii_key >= 0) {
            uint8_t key = (uint8_t)s->ascii_key;
            s->ascii_key = -1;
            s->cpu.EF[3] = true;  /* EF4 deasserted: no more key pending */
            return key;
        }
        return 0xFF;
    }
    return 0xFF;
}

static void vip_io_out(uint8_t port, uint8_t val, void *ud) {
    RcaVipState *s = ud;
    (void)val;
    if (port == 1) {
        /* OUT 1 on the COSMAC VIP disables the CDP1861 display. */
        if (s->cfg->vga == RCA_VGA_CDP1861)
            cdp1861_set_display(&s->vdc, false);
    } else if (s->cfg->vga == RCA_VGA_CDP1869 && port >= 3 && port <= 7) {
        cdp1869_out(&s->vis, port, s->cpu.memory_addr, val);
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
        s->cpu.EF[1] = !s->ef2_down;
        s->cpu.EF[2] = !rca_vip_has_vp601(s->cfg) || s->ascii_key < 0;
        s->cpu.EF[3] = !vip_ascii_pending(s) &&
                       (!rca_vip_has_keypad(s->cfg) || s->key_down < 0);
        cdp1802_step(&s->cpu);
    }
}

RcaVipState *rca_vip_create(const RcaConfig *cfg) {
    RcaVipState *s = calloc(1, sizeof(*s));
    if (!s) return NULL;

    s->cfg = cfg;
    cdp1802_init(&s->cpu, s->mem, VIP_MEM_SIZE);
    s->cpu.on_sync = vip_sync;
    s->cpu.q_out   = vip_q_out;
    s->cpu.io_in   = vip_io_in;
    s->cpu.io_out  = vip_io_out;
    s->cpu.mem_read = vip_mem_read;
    s->cpu.mem_write = vip_mem_write;
    s->cpu.io_ud   = s;
    s->cpu.panic   = vip_cpu_panic;
    s->cpu.panic_ud = s;

    s->monitor = jemu_monitor_create();

    cdp1861_init(&s->vdc, vip_dma_out, s);
    cdp1869_init(&s->vis);
    if (cfg->vnc_addr) {
        int w = (cfg->vga == RCA_VGA_CDP1869) ? CDP1869_VISIBLE_W : CDP1861_DISPLAY_W;
        int h = (cfg->vga == RCA_VGA_CDP1869) ? CDP1869_VISIBLE_H : CDP1861_DISPLAY_H;
        s->vnc = jemu_vnc_create(cfg->vnc_addr, w * cfg->display_scale,
                                 h * cfg->display_scale);
        if (cfg->vga == RCA_VGA_CDP1861)
            jemu_vnc_set_colors(s->vnc, 0xFFFFFFu, 0x100080u);
        else if (cfg->vga == RCA_VGA_CDP1869)
            jemu_vnc_set_palette(s->vnc, vip_vis_palette, 8);
    }

    s->key_down = -1;
    s->ascii_key = -1;

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
    cdp1869_reset(&s->vis);
    memset(s->vram, 0, sizeof(s->vram));
    s->draw_flag = true;
    s->key_down  = -1;
    s->ascii_key = -1;
    s->ef2_down  = false;
    memset(s->keys, 0, sizeof(s->keys));

    JemuMemory tmp = {.data = s->mem, .size = VIP_MEM_SIZE};
    for (int i = 0; i < cfg->n_roms; i++)
        jemu_mem_load_file(&tmp, cfg->roms[i].addr, cfg->roms[i].path, NULL);

    vip_apply_start_addr(s, cfg);
}

void rca_vip_destroy(RcaVipState *s) {
    if (!s) return;
    if (crash_state == s) crash_state = NULL;
    jemu_monitor_destroy(s->monitor);
    jemu_vnc_destroy(s->vnc);
    free(s);
}

/* ── SDL run loop ─────────────────────────────────────────────────────────── */

void rca_machine_run(RcaVipState *s, const RcaConfig *cfg) {
    vip_install_crash_handlers(s);

    RcaDisplay *display;
    switch (cfg->display_type) {
    case JEMU_DISPLAY_SDL:
        if (cfg->vga == RCA_VGA_CDP1869)
            display = rca_display_sdl_create_indexed("JEMU", CDP1869_VISIBLE_W,
                                                     CDP1869_VISIBLE_H,
                                                     cfg->display_scale,
                                                     vip_vis_palette, 8);
        else if (cfg->vga == RCA_VGA_NONE)
            display = rca_display_none_create();
        else
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

    /* SDL2 run loop — PAL (CDP1869) runs at 50 Hz / 4368 mcycles; NTSC at 60 Hz / 3668 */
    const bool is_pal = (cfg->vga == RCA_VGA_CDP1869);
    const Uint32 frame_ms = is_pal ? 20u : 16u;
    const int mcycles_per_frame = is_pal ? CDP1869_MCYCLES_PER_FRAME
                                         : CDP1861_MCYCLES_PER_FRAME;
    jemu_monitor_start(s->monitor);
    if (rca_vip_has_vp601(cfg))
        SDL_StartTextInput();

    bool quit = false;
    while (!quit) {
        Uint32 t0 = SDL_GetTicks();

        /* ── Input ── */
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_QUIT) { quit = true; break; }
            if (ev.type == SDL_TEXTINPUT && rca_vip_has_vp601(cfg)) {
                unsigned char ch = (unsigned char)ev.text.text[0];
                if (ch >= 'a' && ch <= 'z')
                    ch = (unsigned char)(ch - 32);
                if (ch >= 0x20 && ch <= 0x7E)
                    s->ascii_key = ch;
                continue;
            }
            if (ev.type == SDL_KEYDOWN && ev.key.keysym.sym == SDLK_ESCAPE) {
                quit = true; break;
            }
            if (ev.type == SDL_KEYDOWN || ev.type == SDL_KEYUP) {
                bool down = (ev.type == SDL_KEYDOWN);
                /* TAB acts as the VIP front-panel INPUT button (EF2). */
                if (ev.key.keysym.sym == SDLK_TAB)
                    s->ef2_down = down;

                if (down && rca_vip_has_vp601(cfg)) {
                    int ascii = rca_vp601_sdl_key_to_ascii(ev.key.keysym.sym, ev.key.keysym.mod);
                    if (ascii >= 0)
                        s->ascii_key = ascii;
                }

                if (rca_vip_has_keypad(cfg)) {
                    int k = rca_vip_keypad_keycode_to_hex(ev.key.keysym.sym);
                    if (k >= 0) {
                        if (down) {
                            s->keys[k] = 1;
                            s->key_down = k;
                        } else {
                            s->keys[k] = 0;
                            if (s->key_down == k) s->key_down = -1;
                        }
                    }
                }
            }
        }
        if (quit) break;

        /* Sync any held key to key_down */
        uint8_t vnc_keys[16];
        jemu_vnc_get_keys(s->vnc, vnc_keys);
        int vnc_ascii = rca_vp601_vnc_keysym_to_ascii(jemu_vnc_pop_keysym(s->vnc));
        if (vnc_ascii >= 0 && rca_vip_has_vp601(cfg))
            s->ascii_key = vnc_ascii;

        s->key_down = -1;
        if (rca_vip_has_keypad(cfg)) {
            for (int k = 0; k < 16; k++)
                if (s->keys[k] || vnc_keys[k]) { s->key_down = k; break; }
        }

        /* EF4 is active-low for both VIP hex keypad and VP601 data-ready. */
        s->cpu.EF[3] = !vip_ascii_pending(s) &&
                       (!rca_vip_has_keypad(cfg) || s->key_down < 0);

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

        /* ── Execute one frame worth of machine cycles ── */
        for (int i = 0; i < mcycles_per_frame; i++) {
            s->cpu.EF[1] = !s->ef2_down;
            s->cpu.EF[2] = !rca_vip_has_vp601(cfg) || s->ascii_key < 0;
            s->cpu.EF[3] = !vip_ascii_pending(s) &&
                           (!rca_vip_has_keypad(cfg) || s->key_down < 0);
            cdp1802_step(&s->cpu);
        }

        /* ── Render ── */
        if (cfg->vga == RCA_VGA_CDP1869) {
            cdp1869_render(&s->vis);
            rca_display_render(display, s->vis.bitmap,
                               CDP1869_VISIBLE_W, CDP1869_VISIBLE_H);
            jemu_vnc_update(s->vnc, s->vis.bitmap,
                            CDP1869_VISIBLE_W, CDP1869_VISIBLE_H);
        } else if (s->draw_flag && cfg->vga == RCA_VGA_CDP1861) {
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

    if (rca_vip_has_vp601(cfg))
        SDL_StopTextInput();
    jemu_monitor_stop(s->monitor);
    rca_display_destroy(display);
}
