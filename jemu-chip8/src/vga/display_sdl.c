#include "chip8.h"
#include <SDL2/SDL.h>
#include <stdlib.h>
#include <string.h>

/* Plain SDL2 display — no menu bar. The window is the game, nothing else. */

typedef struct {
    SDL_Window   *window;
    SDL_Renderer *renderer;
    SDL_Texture  *texture;
    int           scale;
} SdlCtx;

/* ── Minimal 8×8 bitmap font (row-major, MSB = leftmost pixel) ─────────── */

static const uint8_t font8x8[128][8] = {
    [' '] = {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    ['!'] = {0x18,0x18,0x18,0x18,0x18,0x00,0x18,0x00},
    ['('] = {0x0C,0x18,0x30,0x30,0x30,0x18,0x0C,0x00},
    [')'] = {0x30,0x18,0x0C,0x0C,0x0C,0x18,0x30,0x00},
    ['G'] = {0x3C,0x66,0x60,0x60,0x6E,0x66,0x3C,0x00},
    ['a'] = {0x00,0x00,0x3C,0x06,0x3E,0x66,0x3E,0x00},
    ['d'] = {0x06,0x06,0x3E,0x66,0x66,0x66,0x3E,0x00},
    ['e'] = {0x00,0x00,0x3C,0x66,0x7E,0x60,0x3C,0x00},
    ['g'] = {0x00,0x00,0x3E,0x66,0x66,0x3E,0x06,0x3C},
    ['h'] = {0x60,0x60,0x7C,0x66,0x66,0x66,0x66,0x00},
    ['i'] = {0x18,0x00,0x38,0x18,0x18,0x18,0x3C,0x00},
    ['l'] = {0x38,0x18,0x18,0x18,0x18,0x18,0x0E,0x00},
    ['n'] = {0x00,0x00,0x7C,0x66,0x66,0x66,0x66,0x00},
    ['o'] = {0x00,0x00,0x3C,0x66,0x66,0x66,0x3C,0x00},
    ['p'] = {0x00,0x00,0x7C,0x66,0x66,0x7C,0x60,0x60},
    ['s'] = {0x00,0x00,0x3E,0x60,0x3C,0x06,0x7C,0x00},
    ['t'] = {0x18,0x18,0x7E,0x18,0x18,0x18,0x0E,0x00},
    ['u'] = {0x00,0x00,0x66,0x66,0x66,0x66,0x3E,0x00},
    ['y'] = {0x00,0x00,0x66,0x66,0x66,0x3E,0x0C,0x38},
    ['z'] = {0x00,0x00,0x7E,0x0C,0x18,0x30,0x7E,0x00},
};

static void draw_text(SDL_Renderer *r, const char *s, int cx, int cy, int fps) {
    int len = (int)strlen(s);
    int tx  = cx - len * 8 * fps / 2;
    int ty  = cy - 4 * fps;
    SDL_SetRenderDrawColor(r, 0xAA, 0xAA, 0xAA, 0xFF);
    for (int ci = 0; ci < len; ci++) {
        unsigned ch = (unsigned char)s[ci];
        const uint8_t *g = (ch < 128) ? font8x8[ch] : font8x8[0];
        int x0 = tx + ci * 8 * fps;
        for (int row = 0; row < 8; row++)
            for (int col = 0; col < 8; col++)
                if (g[row] & (0x80 >> col)) {
                    SDL_Rect px = {x0 + col * fps, ty + row * fps, fps, fps};
                    SDL_RenderFillRect(r, &px);
                }
    }
}

static void sdl_render_placeholder(SdlCtx *c) {
    int pw, ph;
    SDL_RenderSetLogicalSize(c->renderer, 0, 0);
    SDL_GetRendererOutputSize(c->renderer, &pw, &ph);

    SDL_SetRenderDrawColor(c->renderer, 0, 0, 0, 255);
    SDL_RenderClear(c->renderer);
    draw_text(c->renderer, "Guest has not initialized display (yet!)",
              pw / 2, ph / 2, (pw >= 512) ? 2 : 1);
    SDL_RenderPresent(c->renderer);

    SDL_RenderSetLogicalSize(c->renderer,
                             CHIP8_DISPLAY_W * c->scale,
                             CHIP8_DISPLAY_H * c->scale);
}

static void sdl_render(void *ctx, const uint8_t *vram) {
    SdlCtx *c = ctx;

    void *pixels; int pitch;
    SDL_LockTexture(c->texture, NULL, &pixels, &pitch);
    uint32_t *px = pixels;
    for (int y = 0; y < CHIP8_DISPLAY_H; y++)
        for (int x = 0; x < CHIP8_DISPLAY_W; x++)
            px[y * (pitch / 4) + x] =
                vram[y * CHIP8_DISPLAY_W + x] ? 0xFFFFFFFF : 0xFF000000;
    SDL_UnlockTexture(c->texture);

    SDL_RenderClear(c->renderer);
    SDL_RenderCopy(c->renderer, c->texture, NULL, NULL);
    SDL_RenderPresent(c->renderer);
}

static void sdl_destroy(void *ctx) {
    SdlCtx *c = ctx;
    SDL_DestroyTexture(c->texture);
    SDL_DestroyRenderer(c->renderer);
    SDL_DestroyWindow(c->window);
    SDL_QuitSubSystem(SDL_INIT_VIDEO);
    free(c);
}

Chip8Display *chip8_display_sdl_create(int scale) {
    if (SDL_InitSubSystem(SDL_INIT_VIDEO) < 0) return NULL;

    SdlCtx *c = calloc(1, sizeof(*c));
    c->scale  = scale;
    c->window = SDL_CreateWindow(
        "JEMU",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        CHIP8_DISPLAY_W * scale, CHIP8_DISPLAY_H * scale,
        SDL_WINDOW_SHOWN);
    if (!c->window) { free(c); return NULL; }

    c->renderer = SDL_CreateRenderer(c->window, -1,
                      SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!c->renderer) { SDL_DestroyWindow(c->window); free(c); return NULL; }

    /* Logical size = coordinate space; SDL maps to physical pixels (HiDPI) */
    SDL_RenderSetLogicalSize(c->renderer, CHIP8_DISPLAY_W * scale, CHIP8_DISPLAY_H * scale);

    c->texture = SDL_CreateTexture(c->renderer,
                     SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING,
                     CHIP8_DISPLAY_W, CHIP8_DISPLAY_H);
    if (!c->texture) {
        SDL_DestroyRenderer(c->renderer);
        SDL_DestroyWindow(c->window);
        free(c);
        return NULL;
    }

    sdl_render_placeholder(c); /* show until first DRW opcode */

    Chip8Display *d = calloc(1, sizeof(*d));
    d->render  = sdl_render;
    d->destroy = sdl_destroy;
    d->run     = NULL; /* SDL2 does not own the main loop */
    d->ctx     = c;
    return d;
}

/* ── Headless backend (shared between SDL and GTK builds) ─────────────────── */

static void none_render(void *ctx, const uint8_t *vram) { (void)ctx; (void)vram; }
static void none_destroy(void *ctx) { free(ctx); }

Chip8Display *chip8_display_none_create(void) {
    Chip8Display *d = calloc(1, sizeof(*d));
    d->render  = none_render;
    d->destroy = none_destroy;
    d->run     = NULL;
    d->ctx     = calloc(1, 1);
    return d;
}

/* ── Public API ───────────────────────────────────────────────────────────── */

void chip8_display_render(Chip8Display *d, const uint8_t *vram) {
    d->render(d->ctx, vram);
}

void chip8_display_destroy(Chip8Display *d) {
    if (!d) return;
    d->destroy(d->ctx);
    free(d);
}
