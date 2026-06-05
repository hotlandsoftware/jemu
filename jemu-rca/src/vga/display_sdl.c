#include "vip.h"
#include <SDL2/SDL.h>
#include <stdlib.h>

#define VIP_DEFAULT_SCALE 4

typedef struct {
    SDL_Window   *window;
    SDL_Renderer *renderer;
    SDL_Texture  *texture;
    int           scale;
} VipSdlCtx;

/* Phosphor green on black — classic COSMAC VIP look */
#define PIXEL_ON  0xFF33FF33u
#define PIXEL_OFF 0xFF000000u

static void sdl_render(void *ctx, const uint8_t *vram, int w, int h) {
    VipSdlCtx *c = ctx;
    void *pixels; int pitch;
    SDL_LockTexture(c->texture, NULL, &pixels, &pitch);
    uint32_t *px = pixels;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
            px[y * (pitch / 4) + x] = vram[y * w + x] ? PIXEL_ON : PIXEL_OFF;
    SDL_UnlockTexture(c->texture);
    SDL_RenderClear(c->renderer);
    SDL_RenderCopy(c->renderer, c->texture, NULL, NULL);
    SDL_RenderPresent(c->renderer);
}

static void sdl_destroy(void *ctx) {
    VipSdlCtx *c = ctx;
    SDL_DestroyTexture(c->texture);
    SDL_DestroyRenderer(c->renderer);
    SDL_DestroyWindow(c->window);
    SDL_QuitSubSystem(SDL_INIT_VIDEO);
    free(c);
}

RcaDisplay *rca_display_sdl_create(int scale) {
    if (scale <= 0) scale = VIP_DEFAULT_SCALE;
    if (SDL_InitSubSystem(SDL_INIT_VIDEO) < 0) return NULL;

    VipSdlCtx *c = calloc(1, sizeof(*c));
    c->scale  = scale;
    c->window = SDL_CreateWindow(
        "JEMU — COSMAC VIP",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        CDP1861_DISPLAY_W * scale, CDP1861_DISPLAY_H * scale,
        SDL_WINDOW_SHOWN);
    if (!c->window) { free(c); SDL_QuitSubSystem(SDL_INIT_VIDEO); return NULL; }

    c->renderer = SDL_CreateRenderer(c->window, -1,
                      SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!c->renderer) { SDL_DestroyWindow(c->window); free(c); return NULL; }

    SDL_RenderSetLogicalSize(c->renderer,
                             CDP1861_DISPLAY_W * scale,
                             CDP1861_DISPLAY_H * scale);

    c->texture = SDL_CreateTexture(c->renderer,
                     SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING,
                     CDP1861_DISPLAY_W, CDP1861_DISPLAY_H);
    if (!c->texture) {
        SDL_DestroyRenderer(c->renderer);
        SDL_DestroyWindow(c->window);
        free(c);
        return NULL;
    }

    /* Black screen until first DMA fills the framebuffer */
    SDL_SetRenderDrawColor(c->renderer, 0, 0, 0, 255);
    SDL_RenderClear(c->renderer);
    SDL_RenderPresent(c->renderer);

    RcaDisplay *d = calloc(1, sizeof(*d));
    d->render  = sdl_render;
    d->destroy = sdl_destroy;
    d->run     = NULL;
    d->ctx     = c;
    return d;
}

static void none_render(void *ctx, const uint8_t *v, int w, int h) {
    (void)ctx; (void)v; (void)w; (void)h;
}
static void none_destroy(void *ctx) { free(ctx); }

RcaDisplay *rca_display_none_create(void) {
    RcaDisplay *d = calloc(1, sizeof(*d));
    d->render  = none_render;
    d->destroy = none_destroy;
    d->run     = NULL;
    d->ctx     = calloc(1, 1);
    return d;
}
