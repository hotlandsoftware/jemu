#include "pecom.h"
#include "jemu/jemu.h"
#include "jemu/memory.h"
#include <SDL2/SDL.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ── Palette ─────────────────────────────────────────────────────────────── */

/*
 * CDP1869 8-color palette: bits [2]=R, [1]=B, [0]=G.
 * Pecom 32 BASIC uses white background (bkg=7) with black text (color=0).
 */
static uint32_t pecom_palette[8];

static void pecom_init_palette(void) {
    static bool done = false;
    if (done) return;
    static const uint32_t rgb[8] = {
        0xFF000000u, /* 0 = black   */
        0xFF00FF00u, /* 1 = green   */
        0xFF0000FFu, /* 2 = blue    */
        0xFF00FFFFu, /* 3 = cyan    */
        0xFFFF0000u, /* 4 = red     */
        0xFFFFFF00u, /* 5 = yellow  */
        0xFFFF00FFu, /* 6 = magenta */
        0xFFFFFFFFu, /* 7 = white   */
    };
    for (int i = 0; i < 8; i++)
        pecom_palette[i] = rgb[i];
    done = true;
}

/* ── Memory map ──────────────────────────────────────────────────────────── */

static uint8_t pecom_mem_read(uint16_t addr, void *ud) {
    RcaPecom32State *s = ud;

    /* ROM at 0x8000–0xBFFF */
    if (addr >= 0x8000u && addr < 0xC000u)
        return s->rom[addr - 0x8000u];

    /* Bootstrap: ROM mirrored at 0x0000–0x3FFF until first OUT 1 */
    if (s->boot_mirror && addr < 0x4000u)
        return s->rom[addr];

    /* VIS-1870 char RAM: 0xF400–0xF7FF */
    if (addr >= PECOM32_CRAM_BASE && addr < PECOM32_PRAM_BASE)
        return cdp1869_char_read(&s->vis, addr - PECOM32_CRAM_BASE);

    /* VIS-1870 page RAM: 0xF800–0xFFFF */
    if (addr >= PECOM32_PRAM_BASE)
        return cdp1869_page_read(&s->vis, addr - PECOM32_PRAM_BASE);

    /* RAM bank 2: 0xC000–0xF3FF */
    if (addr >= 0xC000u)
        return s->ram2[addr - 0xC000u];

    /* RAM bank 1: 0x0000–0x7FFF */
    return s->ram1[addr];
}

static void pecom_mem_write(uint16_t addr, uint8_t val, void *ud) {
    RcaPecom32State *s = ud;

    /* ROM and ROM mirror: read-only */
    if (addr >= 0x8000u && addr < 0xC000u) return;
    if (s->boot_mirror && addr < 0x4000u)  return;

    /* VIS-1870 char RAM: 0xF400–0xF7FF */
    if (addr >= PECOM32_CRAM_BASE && addr < PECOM32_PRAM_BASE) {
        cdp1869_char_write(&s->vis, addr - PECOM32_CRAM_BASE, val);
        return;
    }

    /* VIS-1870 page RAM: 0xF800–0xFFFF */
    if (addr >= PECOM32_PRAM_BASE) {
        cdp1869_page_write(&s->vis, addr - PECOM32_PRAM_BASE, val);
        return;
    }

    /* RAM bank 2: 0xC000–0xF3FF */
    if (addr >= 0xC000u) {
        s->ram2[addr - 0xC000u] = val;
        return;
    }

    /* RAM bank 1: 0x0000–0x7FFF */
    s->ram1[addr] = val;
}

/* ── I/O ─────────────────────────────────────────────────────────────────── */

/* Key-to-latch-code mapping for the Pecom 32 keyboard.
 * The ROM scans latch values 0–63, checking EF3 for each.
 * This table maps SDL keysyms to latch codes.  -1 = unmapped.
 *
 * NOTE: This mapping is a best-guess.  The real Pecom 32 keyboard matrix
 * may assign different latch codes.  Adjust as needed for the ROM. */
static const struct {
    uint32_t keysym;
    int      latch;
} pecom_keymap[] = {
    /* Row 0: digits */
    {'0',  0}, {'1',  1}, {'2',  2}, {'3',  3}, {'4',  4},
    {'5',  5}, {'6',  6}, {'7',  7}, {'8',  8}, {'9',  9},
    /* Row 1: QWERTY top */
    {'q', 10}, {'w', 11}, {'e', 12}, {'r', 13}, {'t', 14},
    {'y', 15}, {'u', 16}, {'i', 17}, {'o', 18}, {'p', 19},
    /* Row 2: QWERTY middle */
    {'a', 20}, {'s', 21}, {'d', 22}, {'f', 23}, {'g', 24},
    {'h', 25}, {'j', 26}, {'k', 27}, {'l', 28},
    /* Row 3: QWERTY bottom */
    {'z', 29}, {'x', 30}, {'c', 31}, {'v', 32}, {'b', 33},
    {'n', 34}, {'m', 35},
    /* Special keys */
    {' ',          36},  /* Space */
    {'\r',         37},  /* Enter (CR) */
    {'\b',         38},  /* Backspace */
    {',',          39},  {'.',          40},  {'/',  41},
    {';',          42},  {'\'',        43},
    {'[',          44},  {']',          45},
    {'-',          46},  {'=',          47},
    {'`',          48},  {'\\',        49},
    /* Cursor keys (SDLK values) */
    {SDLK_UP,       50},
    {SDLK_DOWN,     51},
    {SDLK_LEFT,     52},
    {SDLK_RIGHT,    53},
};
#define PECOM_N_KEYS (sizeof(pecom_keymap) / sizeof(pecom_keymap[0]))

static uint8_t pecom_io_in(uint8_t port, void *ud) {
    RcaPecom32State *s = ud;

    if (s->iogroup == 0 && port == 3) {
        /* Iogroup 0, INP 3: keyboard column data.
         * R[X] selects the key row via lower 6 address bits.
         * Standard pull-up: 0xFF = all released, a 0-bit = key pressed.
         * Stub: return 0xFF (no keys). */
        (void)s->cpu.memory_addr;
        return 0xFFu;
    }
    return 0xFFu;
}

static void pecom_io_out(uint8_t port, uint8_t val, void *ud) {
    RcaPecom32State *s = ud;

    if (port == 1) {
        /* OUT 1: iogroup selector (bit 1 = 1 → iogroup 2 / VIS-1870) */
        bool grp2 = (val & 0x02u) != 0;
        s->iogroup = grp2 ? 2 : 0;
        /* Any OUT 1 removes the boot ROM mirror */
        s->boot_mirror = false;
        return;
    }

    if (s->iogroup == 0) {
        /* Iogroup 0: keyboard latch on OUT 3.
         * The ROM writes a latch value (0–63) to select which key to check,
         * then reads EF3 to see if that key is pressed. */
        if (port == 3) {
            s->key_latch = val & 0x3Fu;  /* lower 6 bits */
            return;
        }
        /* Other OUT ports in iogroup 0 are unused by Pecom 32 ROM */
        return;
    }

    if (s->iogroup == 2 && port >= 3 && port <= 7) {
        cdp1869_out(&s->vis, port, s->cpu.memory_addr, val);
        return;
    }
}

static void pecom_q_out(uint8_t q, void *ud) {
    RcaPecom32State *s = ud;
    s->vis.q = q & 1u;
    rca_pcspk_set_gate(s->speaker, q);
}

/* ── Per-frame video timing ──────────────────────────────────────────────── */

static void pecom_video_timing(RcaPecom32State *s, unsigned frame_cycle) {
    /* Map CPU frame cycle → VIS-1870 scan line */
    unsigned vis_line =
        (frame_cycle * CDP1869_LINES_TOTAL) / PECOM32_MCYCLES_PER_FRAME;

    /* non_display: true during VBlank (lines 0–47 and 264–311) */
    bool nd = (vis_line < CDP1869_DISPLAY_START ||
               vis_line >= CDP1869_DISPLAY_END);
    s->vis.non_display = nd || s->vis.dispoff;

    /* EF1 = non_display: high during VBlank, low during active display */
    s->cpu.EF[0] = s->vis.non_display;

    /* EF2 = SHIFT (active-high) */
    s->cpu.EF[1] = s->key_shift;

    /* EF3 = keyboard latch status:
     *   ef.reverse = 1 → EF3=1 when key RELEASED, 0 when PRESSED.
     *   ROM uses BN3 (branch if EF3=1) to spin-wait for a key press. */
    {
        bool key_down = false;
        if (s->key_latch >= 0 && s->key_latch < 64)
            key_down = s->keys[s->key_latch];
        s->cpu.EF[2] = !key_down;  /* 1 = released, 0 = pressed */
    }

    /* EF4 = ESC (active-high) */
    s->cpu.EF[3] = s->key_esc;

    /* Single interrupt per frame at "line 2" (start of VBlank) */
    unsigned int_cycle =
        (2u * PECOM32_MCYCLES_PER_FRAME) / CDP1869_LINES_TOTAL;
    if (frame_cycle == int_cycle)
        cdp1802_request_irq(&s->cpu);
}

/* ── Machine lifecycle ───────────────────────────────────────────────────── */

RcaPecom32State *rca_pecom32_create(const RcaConfig *cfg) {
    pecom_init_palette();

    RcaPecom32State *s = calloc(1, sizeof(*s));
    if (!s) return NULL;

    s->cfg         = cfg;
    s->boot_mirror = true;
    s->iogroup     = 0;
    s->key_latch   = -1;  /* no latch selected yet */

    for (int i = 0; i < 64; i++)
        s->keys[i] = false;

    cdp1802_init(&s->cpu, NULL, 0);
    s->cpu.mem_read  = pecom_mem_read;
    s->cpu.mem_write = pecom_mem_write;
    s->cpu.io_in     = pecom_io_in;
    s->cpu.io_out    = pecom_io_out;
    s->cpu.q_out     = pecom_q_out;
    s->cpu.io_ud     = s;
    /* on_sync not used — timing driven by pecom_video_timing in the run loop */

    cdp1869_init(&s->vis);
    cdp1869_set_page_ram_mask(&s->vis, 0x3FFu);  /* 1 KB page RAM */
    cdp1869_set_char_stride(&s->vis, 16u);        /* 16 scan lines / char */
    cdp1869_set_block_cpu_access(&s->vis, true);  /* block during active display */

    s->monitor = jemu_monitor_create();

    if (cfg->sound_hw == RCA_SOUND_PCSPK && !cfg->vnc_addr) {
        s->speaker = rca_pcspk_create(250u);
        if (!s->speaker)
            fprintf(stderr, "jemu-rca: pecom32: failed to init audio\n");
    }

    if (cfg->vnc_addr) {
        s->vnc = jemu_vnc_create(cfg->vnc_addr,
                                 CDP1869_VISIBLE_W * cfg->display_scale,
                                 CDP1869_VISIBLE_H * cfg->display_scale);
        jemu_vnc_set_palette(s->vnc, pecom_palette,
                             (int)(sizeof(pecom_palette) / sizeof(pecom_palette[0])));
    }

    /* Load ROM — file is the raw 16 KB image, mapped at 0x8000 */
    for (int i = 0; i < cfg->n_roms; i++) {
        uint32_t off = cfg->roms[i].addr;
        JemuMemory tmp = {.data = s->rom, .size = PECOM32_ROM_SIZE};
        size_t len = 0;
        if (!jemu_mem_load_file(&tmp, off, cfg->roms[i].path, &len)) {
            fprintf(stderr, "jemu-rca: pecom32: failed to load '%s'\n",
                    cfg->roms[i].path);
            free(s);
            return NULL;
        }
        printf("jemu-rca: %zu bytes @ ROM+0x%04X  ← %s\n",
               len, off, cfg->roms[i].path);
    }

    return s;
}

void rca_pecom32_reset(RcaPecom32State *s, const RcaConfig *cfg) {
    cdp1802_reset(&s->cpu);
    cdp1869_reset(&s->vis);
    cdp1869_set_page_ram_mask(&s->vis, 0x3FFu);
    cdp1869_set_char_stride(&s->vis, 16u);
    cdp1869_set_block_cpu_access(&s->vis, true);
    rca_pcspk_set_gate(s->speaker, 0);

    s->boot_mirror = true;
    s->iogroup     = 0;
    s->key_shift   = false;
    s->key_ctrl    = false;
    s->key_esc     = false;
    s->key_latch   = -1;

    for (int i = 0; i < 64; i++)
        s->keys[i] = false;

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

/* ── Input polling ───────────────────────────────────────────────────────── */

static void pecom_poll_keys(RcaPecom32State *s, RcaDisplay *display) {
    if (!display) return;
    for (size_t i = 0; i < PECOM_N_KEYS; i++) {
        int latch = pecom_keymap[i].latch;
        if (latch >= 0 && latch < 64)
            s->keys[latch] = rca_display_key_down(display, pecom_keymap[i].keysym);
    }

    s->key_esc   = rca_display_key_down(display, RCA_KEY_ESCAPE);
    s->key_shift = rca_display_key_down(display, SDLK_LSHIFT)
                || rca_display_key_down(display, SDLK_RSHIFT);
}

static void pecom_poll_display(RcaPecom32State *s, RcaDisplay *display,
                               bool *quit) {
    rca_display_poll(display);
    if (rca_display_should_quit(display)) {
        *quit = true;
        return;
    }

    pecom_poll_keys(s, display);
}

static void pecom_poll_vnc(RcaPecom32State *s) {
    JemuVncKeyEvent ev;
    while (jemu_vnc_pop_key_event(s->vnc, &ev)) {
        /* Map VNC keysym to keyboard latch */
        for (size_t i = 0; i < PECOM_N_KEYS; i++) {
            if ((uint32_t)ev.keysym == pecom_keymap[i].keysym) {
                int latch = pecom_keymap[i].latch;
                if (latch >= 0 && latch < 64)
                    s->keys[latch] = ev.down;
                break;
            }
        }
        /* Modifier keys */
        switch (ev.keysym) {
        case 0xffe1: case 0xffe2: s->key_shift = ev.down; break; /* Shift */
        case 0xffe3: case 0xffe4: s->key_ctrl  = ev.down; break; /* Ctrl */
        case 0xff1b:               s->key_esc   = ev.down; break; /* Esc */
        default: break;
        }
    }
}

/* ── Run loop ────────────────────────────────────────────────────────────── */

void rca_pecom32_run(RcaPecom32State *s, const RcaConfig *cfg) {
    RcaDisplay *display = rca_display_create_indexed(
        cfg->display_type, "JEMU",
        CDP1869_VISIBLE_W, CDP1869_VISIBLE_H,
        cfg->display_scale,
        pecom_palette,
        (int)(sizeof(pecom_palette) / sizeof(pecom_palette[0])));

    if (!display) {
        fprintf(stderr, "jemu-rca: pecom32: failed to create display\n");
        return;
    }

    if (display->run) {
        display->run(display, s, cfg);
        rca_display_destroy(display);
        return;
    }

    /* PAL 50 Hz */
    const Uint32 frame_ms = 1000u / PECOM32_FRAME_HZ;

    jemu_monitor_start(s->monitor);
    SDL_StartTextInput();

    bool quit = false;
    while (!quit) {
        Uint32 t0 = SDL_GetTicks();

        if (cfg->display_type != JEMU_DISPLAY_NONE)
            pecom_poll_display(s, display, &quit);
        pecom_poll_vnc(s);

        JemuMonCmd cmd;
        while ((cmd = jemu_monitor_poll(s->monitor)) != JEMU_MON_NONE) {
            if      (cmd == JEMU_MON_QUIT)  quit = true;
            else if (cmd == JEMU_MON_RESET) rca_pecom32_reset(s, cfg);
            else if (cmd == JEMU_MON_CUSTOM) jemu_monitor_unknown_command(s->monitor);
        }
        if (quit) break;

        if (!jemu_monitor_is_paused(s->monitor)) {
            for (unsigned i = 0; i < PECOM32_MCYCLES_PER_FRAME; i++) {
                pecom_video_timing(s, i);
                cdp1802_step(&s->cpu);
            }

            /* Force VBlank at frame boundary so RAM is accessible */
            s->vis.non_display = true;
            s->cpu.EF[0] = true;

            if (s->vis.dirty) {
                cdp1869_render(&s->vis);
                rca_display_render(display, s->vis.bitmap,
                                   CDP1869_VISIBLE_W, CDP1869_VISIBLE_H);
                jemu_vnc_update(s->vnc, s->vis.bitmap,
                                CDP1869_VISIBLE_W, CDP1869_VISIBLE_H);
            }
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
