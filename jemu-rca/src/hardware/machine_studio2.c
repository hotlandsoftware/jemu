#include "studio2.h"
#include "jemu/memory.h"
#include <SDL2/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define STUDIO2_FRAME_HZ       60u

/* 300 Hz beeper as used in MAME's Studio II driver */
#define STUDIO2_BEEP_HZ  300u

/* ST2 cartridge file format */
#define ST2_HEADER_SIZE  0x100u
#define ST2_MAGIC        "RCA2"

/* ── Memory callbacks ────────────────────────────────────────────────────── */

static uint8_t studio2_mem_read(uint16_t addr, void *ud) {
    RcaStudio2State *s = ud;
    if (addr < 0x0400)
        return s->rom[addr];
    if (addr < 0x0800)
        return s->cart_loaded ? s->cart[addr - 0x0400] : s->rom[addr];
    if (addr >= 0x0C00 && addr < 0x1000 && s->cart_c00)
        return s->cart[0x0800 + (addr - 0x0C00)];
    return s->ram[addr & STUDIO2_RAM_MASK];
}

static void studio2_mem_write(uint16_t addr, uint8_t val, void *ud) {
    RcaStudio2State *s = ud;
    if (addr >= 0x0800)
        s->ram[addr & STUDIO2_RAM_MASK] = val;
}

/* ── DMA: CDP1861 → vram ─────────────────────────────────────────────────── */

static void studio2_dma_out(uint8_t *data, void *ud) {
    RcaStudio2State *s = ud;
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

/* ── CPU I/O callbacks ───────────────────────────────────────────────────── */

static uint8_t studio2_io_in(uint8_t port, void *ud) {
    RcaStudio2State *s = ud;
    if (port == 1) {
        cdp1861_set_display(&s->vdc, true);
        return 0xFF;
    }
    return 0xFF;
}

static void studio2_io_out(uint8_t port, uint8_t val, void *ud) {
    RcaStudio2State *s = ud;
    if (port == 1) {
        cdp1861_set_display(&s->vdc, false);
    } else if (port == 2) {
        s->keylatch = val & 0x0Fu;
    }
}

static void studio2_sync(void *ud) {
    RcaStudio2State *s = ud;
    cdp1861_sync(&s->vdc, &s->cpu);
}

static void studio2_q_out(uint8_t q, void *ud) {
    RcaStudio2State *s = ud;
    rca_pcspk_set_gate(s->speaker, q);
}

/* ── Cartridge loading ───────────────────────────────────────────────────── */

static bool studio2_load_cart(RcaStudio2State *s, const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "studio2: cannot open cartridge '%s'\n", path);
        return false;
    }

    fseek(f, 0, SEEK_END);
    long file_size = ftell(f);
    rewind(f);

    memset(s->cart, 0xFF, sizeof(s->cart));
    s->cart_c00 = false;

    /* Detect ST2 format by magic header */
    char magic[4];
    bool is_st2 = (file_size >= (long)ST2_HEADER_SIZE) &&
                  fread(magic, 1, 4, f) == 4 &&
                  memcmp(magic, ST2_MAGIC, 4) == 0;

    if (is_st2) {
        uint8_t header[ST2_HEADER_SIZE];
        rewind(f);
        if (fread(header, 1, ST2_HEADER_SIZE, f) < ST2_HEADER_SIZE) {
            fprintf(stderr, "studio2: truncated ST2 header in '%s'\n", path);
            fclose(f);
            return false;
        }
        uint8_t blocks = header[4];
        if (blocks < 2 || blocks > 11) {
            fprintf(stderr, "studio2: invalid ST2 block count %u in '%s'\n", blocks, path);
            fclose(f);
            return false;
        }
        uint8_t *pages = &header[64];
        for (int blk = 0; blk < blocks - 1; blk++) {
            uint16_t target = (uint16_t)pages[blk] << 8;
            if (target < 0x0400 || target > 0x0F00) {
                fprintf(stderr, "studio2: ST2 block %d has invalid target 0x%04X\n", blk, target);
                continue;
            }
            uint16_t offset;
            if (target >= 0x0C00)
                offset = (uint16_t)(0x0800 + (target - 0x0C00));
            else
                offset = (uint16_t)(target - 0x0400);
            if ((unsigned)offset + 0x100u > STUDIO2_CART_SIZE) continue;
            if (fread(&s->cart[offset], 1, 0x100, f) < 0x100)
                fprintf(stderr, "studio2: short ST2 block %d\n", blk);
            if (target >= 0x0C00)
                s->cart_c00 = true;
        }
    } else {
        /* Raw binary: load at 0x0400 */
        rewind(f);
        size_t limit = (file_size > 0x0400) ? 0x0400 : (size_t)file_size;
        size_t n = fread(s->cart, 1, limit, f);
        if (n == 0) {
            fprintf(stderr, "studio2: empty cartridge '%s'\n", path);
            fclose(f);
            return false;
        }
        /* Some homebrews extend into 0x0C00-0x0FFF */
        if (file_size > 0x0400) {
            size_t extra = fread(&s->cart[0x0800], 1, 0x0400, f);
            if (extra > 0) s->cart_c00 = true;
        }
    }

    fclose(f);
    s->cart_loaded = true;
    snprintf(s->cart_path, sizeof(s->cart_path), "%s", path);
    printf("studio2: cartridge '%s' loaded%s\n", path, s->cart_c00 ? " (+0xC00 bank)" : "");
    return true;
}

static void studio2_eject_cart(RcaStudio2State *s) {
    memset(s->cart, 0xFF, sizeof(s->cart));
    s->cart_loaded = false;
    s->cart_c00    = false;
    s->cart_path[0] = '\0';
    printf("studio2: cartridge ejected\n");
}

/* ── Lifecycle ───────────────────────────────────────────────────────────── */

RcaStudio2State *rca_studio2_create(const RcaConfig *cfg) {
    RcaStudio2State *s = calloc(1, sizeof(*s));
    if (!s) return NULL;
    s->cfg = cfg;

    cdp1802_init(&s->cpu, NULL, 0);
    s->cpu.io_in      = studio2_io_in;
    s->cpu.io_out     = studio2_io_out;
    s->cpu.mem_read   = studio2_mem_read;
    s->cpu.mem_write  = studio2_mem_write;
    s->cpu.on_sync    = studio2_sync;
    s->cpu.q_out      = studio2_q_out;
    s->cpu.io_ud      = s;

    cdp1861_init(&s->vdc, studio2_dma_out, s);

    memset(s->rom,  0xFF, sizeof(s->rom));
    memset(s->cart, 0xFF, sizeof(s->cart));

    /* Load built-in ROM(s) */
    JemuMemory tmp_rom = {.data = s->rom, .size = STUDIO2_ROM_SIZE};
    for (int i = 0; i < cfg->n_roms; i++) {
        size_t len = 0;
        if (!jemu_mem_load_file(&tmp_rom, cfg->roms[i].addr, cfg->roms[i].path, &len)) {
            fprintf(stderr, "studio2: failed to load '%s'\n", cfg->roms[i].path);
            rca_studio2_destroy(s);
            return NULL;
        }
        printf("jemu-rca: %zu bytes @ 0x%04X  <- %s\n", len, cfg->roms[i].addr, cfg->roms[i].path);
    }

    /* Insert cartridge if provided */
    if (cfg->cartridge_path)
        studio2_load_cart(s, cfg->cartridge_path);

    if (cfg->sound_hw != RCA_SOUND_NONE) {
        s->speaker = rca_pcspk_create(STUDIO2_BEEP_HZ);
        if (s->speaker)
            rca_pcspk_set_freq(s->speaker, STUDIO2_BEEP_HZ);
    }

    s->monitor = jemu_monitor_create();
    if (cfg->vnc_addr) {
        s->vnc = jemu_vnc_create(cfg->vnc_addr,
                                 CDP1861_DISPLAY_W * cfg->display_scale,
                                 CDP1861_DISPLAY_H * cfg->display_scale);
    }

    rca_studio2_reset(s, cfg);
    return s;
}

void rca_studio2_reset(RcaStudio2State *s, const RcaConfig *cfg) {
    (void)cfg;
    memset(s->ram, 0, sizeof(s->ram));
    cdp1802_reset(&s->cpu);
    cdp1861_reset(&s->vdc);
    s->keylatch = 0;
}

void rca_studio2_destroy(RcaStudio2State *s) {
    if (!s) return;
    rca_pcspk_destroy(s->speaker);
    jemu_monitor_destroy(s->monitor);
    jemu_vnc_destroy(s->vnc);
    free(s);
}

/* ── Input ───────────────────────────────────────────────────────────────── */

/* Player A: LALT/Q/W/E/A/S/D/Z/X/C → keys 0-9 */
static const SDL_Keycode keys_a_map[10] = {
    SDLK_LALT, SDLK_q, SDLK_w, SDLK_e,
    SDLK_a,    SDLK_s, SDLK_d,
    SDLK_z,    SDLK_x, SDLK_c,
};

/* Player B: numpad 0,7,8,9,4,5,6,1,2,3 → keys 0-9 */
static const SDL_Keycode keys_b_map[10] = {
    SDLK_KP_0, SDLK_KP_7, SDLK_KP_8, SDLK_KP_9,
    SDLK_KP_4, SDLK_KP_5, SDLK_KP_6,
    SDLK_KP_1, SDLK_KP_2, SDLK_KP_3,
};

static void studio2_poll_sdl(RcaStudio2State *s, bool *quit, bool *reset) {
    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {
        if (ev.type == SDL_QUIT) { *quit = true; return; }
        if (ev.type != SDL_KEYDOWN && ev.type != SDL_KEYUP) continue;

        bool down = ev.type == SDL_KEYDOWN;
        SDL_Keycode k = ev.key.keysym.sym;

        if (k == SDLK_F3 && down) { *reset = true; continue; }

        for (int i = 0; i < 10; i++) {
            if (k == keys_a_map[i]) { s->keys_a[i] = down; break; }
            if (k == keys_b_map[i]) { s->keys_b[i] = down; break; }
        }
    }
}

static void studio2_update_ef(RcaStudio2State *s) {
    uint8_t latch = s->keylatch;
    /* EF pins are active-low: TRUE = not pressed, FALSE = pressed */
    s->cpu.EF[2] = !(latch < 10 && s->keys_a[latch]); /* EF3 = Player A */
    s->cpu.EF[3] = !(latch < 10 && s->keys_b[latch]); /* EF4 = Player B */
}

/* ── Monitor custom commands ─────────────────────────────────────────────── */

static bool studio2_monitor_command(RcaStudio2State *s, const char *line,
                                    bool *reset) {
    char buf[256];
    snprintf(buf, sizeof(buf), "%s", line);
    char *verb = strtok(buf, " \t");
    if (!verb || strcasecmp(verb, "change") != 0) return false;

    char *what = strtok(NULL, " \t");
    if (!what || strcasecmp(what, "cartridge") != 0) return false;

    char *arg = strtok(NULL, " \t");
    if (!arg || strcasecmp(arg, "eject") == 0) {
        studio2_eject_cart(s);
    } else {
        if (studio2_load_cart(s, arg))
            *reset = true;
    }
    return true;
}

/* ── Main loop ───────────────────────────────────────────────────────────── */

static void studio2_poll_vnc(RcaStudio2State *s) {
    if (!s->vnc) return;
    JemuVncKeyEvent ev;
    while (jemu_vnc_pop_key_event(s->vnc, &ev)) {
        bool down = ev.down;
        uint32_t k = ev.keysym;
        for (int i = 0; i < 10; i++) {
            if ((SDL_Keycode)k == keys_a_map[i]) { s->keys_a[i] = down; break; }
            if ((SDL_Keycode)k == keys_b_map[i]) { s->keys_b[i] = down; break; }
        }
    }
}

void rca_studio2_run(RcaStudio2State *s, const RcaConfig *cfg) {
    jemu_monitor_start(s->monitor);

    bool is_sdl  = (cfg->display_type == JEMU_DISPLAY_SDL);
    bool is_none = (cfg->display_type == JEMU_DISPLAY_NONE);

    /* SDL window */
    SDL_Window   *win  = NULL;
    SDL_Renderer *ren  = NULL;
    SDL_Texture  *tex  = NULL;
    if (is_sdl) {
        if (SDL_InitSubSystem(SDL_INIT_VIDEO) < 0) {
            fprintf(stderr, "studio2: SDL_Init failed: %s\n", SDL_GetError());
            return;
        }
        int w = CDP1861_DISPLAY_W * cfg->display_scale;
        int h = CDP1861_DISPLAY_H * cfg->display_scale;
        win = SDL_CreateWindow("jemu-rca (Studio II)", SDL_WINDOWPOS_CENTERED,
                               SDL_WINDOWPOS_CENTERED, w, h, 0);
        ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED |
                                          SDL_RENDERER_PRESENTVSYNC);
        tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888,
                                SDL_TEXTUREACCESS_STREAMING, w, h);
    }

    const Uint32 frame_ms = 1000 / STUDIO2_FRAME_HZ;
    bool quit = false;

    while (!quit) {
        Uint32 t0 = SDL_GetTicks();
        bool reset = false;

        if (is_sdl) studio2_poll_sdl(s, &quit, &reset);
        studio2_poll_vnc(s);
        studio2_update_ef(s);

        /* Monitor commands */
        JemuMonCmd cmd = jemu_monitor_poll(s->monitor);
        if (cmd == JEMU_MON_QUIT)  { quit = true; continue; }
        if (cmd == JEMU_MON_RESET) reset = true;
        if (cmd == JEMU_MON_CUSTOM) {
            if (!studio2_monitor_command(s, jemu_monitor_command_text(s->monitor), &reset))
                jemu_monitor_unknown_command(s->monitor);
        }
        if (reset) {
            rca_studio2_reset(s, cfg);
            continue;
        }

        if (!jemu_monitor_is_paused(s->monitor)) {
            for (unsigned i = 0; i < CDP1861_MCYCLES_PER_FRAME; i++) {
                studio2_update_ef(s);
                cdp1802_step(&s->cpu);
            }
        }

        /* Render */
        if (s->draw_flag) {
            s->draw_flag = false;
            if (is_sdl) {
                uint32_t *pixels; int pitch;
                SDL_LockTexture(tex, NULL, (void **)&pixels, &pitch);
                int scale = cfg->display_scale;
                for (int y = 0; y < CDP1861_DISPLAY_H; y++) {
                    for (int x = 0; x < CDP1861_DISPLAY_W; x++) {
                        uint32_t col = s->vram[y * CDP1861_DISPLAY_W + x] ?
                                       0xFFFFFFFFu : 0xFF000000u;
                        for (int sy = 0; sy < scale; sy++)
                            for (int sx = 0; sx < scale; sx++)
                                pixels[(y * scale + sy) * (pitch / 4) +
                                       (x * scale + sx)] = col;
                    }
                }
                SDL_UnlockTexture(tex);
                SDL_RenderCopy(ren, tex, NULL, NULL);
                SDL_RenderPresent(ren);
            }
            if (s->vnc) {
                static uint8_t px[CDP1861_DISPLAY_W * CDP1861_DISPLAY_H];
                for (int i = 0; i < CDP1861_DISPLAY_W * CDP1861_DISPLAY_H; i++)
                    px[i] = s->vram[i] ? 1u : 0u;
                jemu_vnc_update(s->vnc, px,
                                CDP1861_DISPLAY_W, CDP1861_DISPLAY_H);
            }
        }

        if (!is_none) {
            Uint32 elapsed = SDL_GetTicks() - t0;
            if (elapsed < frame_ms)
                SDL_Delay(frame_ms - elapsed);
        }
    }

    jemu_monitor_stop(s->monitor);
    if (is_sdl) {
        SDL_DestroyTexture(tex);
        SDL_DestroyRenderer(ren);
        SDL_DestroyWindow(win);
        SDL_QuitSubSystem(SDL_INIT_VIDEO);
    }
}
