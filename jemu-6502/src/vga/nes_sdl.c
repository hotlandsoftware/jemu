#include "nes_sdl.h"
#include "../hardware/nes.h"
#include <SDL2/SDL.h>
#include <stdlib.h>
#include <string.h>

#define NES_SDL_DEFAULT_SCALE 2

struct NesDisplay {
    SDL_Window   *window;
    SDL_Renderer *renderer;
    SDL_Texture  *texture;
    const uint32_t *palette;
    int           scale;
    bool          quit;
    uint8_t       ctrl1;   /* current player 1 button state */
};

/* ── SDL keycode → NES button ──────────────────────────────────────────── */

static uint8_t sdl_key_to_btn(SDL_Keycode key) {
    switch (key) {
    case SDLK_z:           return NES_BTN_A;
    case SDLK_x:           return NES_BTN_B;
    case SDLK_RETURN:
    case SDLK_KP_ENTER:    return NES_BTN_START;
    case SDLK_RSHIFT:      return NES_BTN_SELECT;
    case SDLK_UP:          return NES_BTN_UP;
    case SDLK_DOWN:        return NES_BTN_DOWN;
    case SDLK_LEFT:        return NES_BTN_LEFT;
    case SDLK_RIGHT:       return NES_BTN_RIGHT;
    default:               return 0;
    }
}

/* ── Public API ─────────────────────────────────────────────────────────── */

NesDisplay *nes_display_create(const char *title, const uint32_t *palette,
                               int scale) {
    if (scale <= 0) scale = NES_SDL_DEFAULT_SCALE;

    if (SDL_InitSubSystem(SDL_INIT_VIDEO) < 0) return NULL;

    NesDisplay *d = calloc(1, sizeof(*d));
    if (!d) { SDL_QuitSubSystem(SDL_INIT_VIDEO); return NULL; }

    d->palette = palette;
    d->scale   = scale;

    d->window = SDL_CreateWindow(
        title ? title : "jemu-6502 NES",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        256 * scale, 240 * scale,
        SDL_WINDOW_SHOWN);
    if (!d->window) goto fail;

    d->renderer = SDL_CreateRenderer(
        d->window, -1,
        SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!d->renderer) goto fail;

    SDL_RenderSetLogicalSize(d->renderer, 256 * scale, 240 * scale);

    d->texture = SDL_CreateTexture(
        d->renderer, SDL_PIXELFORMAT_ARGB8888,
        SDL_TEXTUREACCESS_STREAMING, 256, 240);
    if (!d->texture) goto fail;

    SDL_SetRenderDrawColor(d->renderer, 0, 0, 0, 255);
    SDL_RenderClear(d->renderer);
    SDL_RenderPresent(d->renderer);
    return d;

fail:
    if (d->texture)  SDL_DestroyTexture(d->texture);
    if (d->renderer) SDL_DestroyRenderer(d->renderer);
    if (d->window)   SDL_DestroyWindow(d->window);
    free(d);
    SDL_QuitSubSystem(SDL_INIT_VIDEO);
    return NULL;
}

void nes_display_destroy(NesDisplay *d) {
    if (!d) return;
    SDL_DestroyTexture(d->texture);
    SDL_DestroyRenderer(d->renderer);
    SDL_DestroyWindow(d->window);
    SDL_QuitSubSystem(SDL_INIT_VIDEO);
    free(d);
}

void nes_display_render(NesDisplay *d, const uint8_t *pixels, int w, int h) {
    void *raw; int pitch;
    SDL_LockTexture(d->texture, NULL, &raw, &pitch);
    uint32_t *out = raw;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
            out[y * (pitch / 4) + x] = d->palette[pixels[y * w + x] & 0x3F];
    SDL_UnlockTexture(d->texture);
    SDL_RenderClear(d->renderer);
    SDL_RenderCopy(d->renderer, d->texture, NULL, NULL);
    SDL_RenderPresent(d->renderer);  /* vsync here keeps us at ~60 FPS */
}

void nes_display_poll(NesDisplay *d) {
    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {
        if (ev.type == SDL_QUIT) {
            d->quit = true;
            continue;
        }
        if (ev.type != SDL_KEYDOWN && ev.type != SDL_KEYUP) continue;

        SDL_Keycode key = ev.key.keysym.sym;
        if (ev.type == SDL_KEYDOWN && key == SDLK_ESCAPE) {
            d->quit = true;
            continue;
        }
        uint8_t btn = sdl_key_to_btn(key);
        if (!btn) continue;
        if (ev.type == SDL_KEYDOWN) d->ctrl1 |=  btn;
        else                        d->ctrl1 &= ~btn;
    }
}

bool    nes_display_should_quit(NesDisplay *d) { return d->quit; }
uint8_t nes_display_ctrl1      (NesDisplay *d) { return d->ctrl1; }
