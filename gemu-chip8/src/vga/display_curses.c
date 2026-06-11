#define _XOPEN_SOURCE_EXTENDED 1
#define _POSIX_C_SOURCE 199309L
#include "chip8.h"
#include <ncurses.h>
#include <locale.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <wchar.h>

/* Same physical layout as SDL/GTK backends */
static const int key_map[CHIP8_NUM_KEYS] = {
    'x', '1', '2', '3', 'q', 'w', 'e', 'a',
    's', 'd', 'z', 'c', '4', 'r', 'f', 'v',
};

static void curses_run(Chip8Display *d, Chip8State *s, const Chip8Config *cfg) {
    (void)d;
    setlocale(LC_ALL, "");
    initscr();
    cbreak();
    noecho();
    keypad(stdscr, TRUE);
    nodelay(stdscr, TRUE);
    curs_set(0);

    const int insns_frame = cfg->cpu_hz / CHIP8_TIMER_HZ;
    struct timespec ts = {0, 1000000000 / CHIP8_TIMER_HZ};
    bool quit = false;

    while (!quit) {
        /* Input — clear state, set bits for any key currently in the buffer */
        memcpy(s->keys_prev, s->keys, CHIP8_NUM_KEYS);
        memset(s->keys, 0, CHIP8_NUM_KEYS);
        int ch;
        while ((ch = getch()) != ERR) {
            if (ch == 27) { quit = true; break; } /* ESC to quit */
            for (int k = 0; k < CHIP8_NUM_KEYS; k++)
                if (ch == key_map[k]) s->keys[k] = 1;
        }
        if (quit) break;

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

        /* Execute */
        int budget = insns_frame;
        while (budget > 0 && !s->wait_key) {
            GemuTb *tb = gemu_tb_lookup(&s->tb_cache, s->PC);
            if (!tb) tb = chip8_translate_block(s, s->PC);
            if (!tb) break;
            chip8_execute_tb(s, tb);
            budget -= (int)tb->n_insns;
        }

        if (s->delay > 0) s->delay--;
        if (s->sound > 0) s->sound--;

        /* Render — two CHIP-8 rows per terminal row via half-block chars.
         * Uses ncursesw add_wch so each glyph occupies exactly one column. */
        if (s->draw_flag) {
            /* space ▀ ▄ █ indexed by (top<<1)|bot */
            static const wchar_t blk[4] = { L' ', L'▄', L'▀', L'█' };
            for (int y = 0; y < CHIP8_DISPLAY_H; y += 2) {
                move(y / 2, 0);
                for (int x = 0; x < CHIP8_DISPLAY_W; x++) {
                    int top = s->vram[y       * CHIP8_DISPLAY_W + x] ? 1 : 0;
                    int bot = s->vram[(y + 1) * CHIP8_DISPLAY_W + x] ? 1 : 0;
                    wchar_t ws[2] = { blk[(top << 1) | bot], L'\0' };
                    cchar_t cc;
                    setcchar(&cc, ws, A_NORMAL, 0, NULL);
                    add_wch(&cc);
                }
            }
            refresh();
            gemu_vnc_update(s->vnc, s->vram, CHIP8_DISPLAY_W, CHIP8_DISPLAY_H);
            s->draw_flag = false;
        }

        nanosleep(&ts, NULL);
    }

    endwin();

    printf("gemu-chip8: %llu instructions executed, %llu TB hits, %llu misses\n",
           (unsigned long long)s->insn_count,
           (unsigned long long)s->tb_hits,
           (unsigned long long)s->tb_misses);
}

static void curses_render(void *ctx, const uint8_t *vram) { (void)ctx; (void)vram; }
static void curses_destroy(void *ctx) { free(ctx); }

Chip8Display *chip8_display_curses_create(void) {
    Chip8Display *d = calloc(1, sizeof(*d));
    d->render  = curses_render;
    d->destroy = curses_destroy;
    d->run     = curses_run;
    d->ctx     = calloc(1, 1);
    return d;
}
