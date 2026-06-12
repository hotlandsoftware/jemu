#include "nes_display.h"
#include "../hardware/nes.h"
#include <SDL2/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NES_SDL_DEFAULT_SCALE 2

typedef struct {
    SDL_Window     *window;
    SDL_Renderer   *renderer;
    SDL_Texture    *texture;
    uint32_t       *frame_argb;
    const uint32_t *palette;
    int             scale;
    bool            software;
    bool            quit;
    uint8_t         ctrl1;
} NesDisplaySdlCtx;

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

static void sdl_render(void *vctx, const uint8_t *pixels, int w, int h) {
    NesDisplaySdlCtx *c = vctx;
    for (int i = 0; i < w * h; i++)
        c->frame_argb[i] = c->palette[pixels[i] & 0x3F];
    SDL_UpdateTexture(c->texture, NULL, c->frame_argb, w * (int)sizeof(uint32_t));
    SDL_RenderClear(c->renderer);
    SDL_RenderCopy(c->renderer, c->texture, NULL, NULL);
    SDL_RenderPresent(c->renderer);
}

static void sdl_render_argb(void *vctx, const uint32_t *pixels, int w, int h) {
    NesDisplaySdlCtx *c = vctx;
    (void)h;
    SDL_UpdateTexture(c->texture, NULL, pixels, w * (int)sizeof(uint32_t));
    SDL_RenderClear(c->renderer);
    SDL_RenderCopy(c->renderer, c->texture, NULL, NULL);
    SDL_RenderPresent(c->renderer);
}

static void sdl_poll(void *vctx) {
    NesDisplaySdlCtx *c = vctx;
    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {
        if (ev.type == SDL_QUIT) { c->quit = true; continue; }
        if (ev.type != SDL_KEYDOWN && ev.type != SDL_KEYUP) continue;
        SDL_Keycode key = ev.key.keysym.sym;
        if (ev.type == SDL_KEYDOWN && key == SDLK_ESCAPE) { c->quit = true; continue; }
        uint8_t btn = sdl_key_to_btn(key);
        if (!btn) continue;
        if (ev.type == SDL_KEYDOWN) c->ctrl1 |=  btn;
        else                        c->ctrl1 &= ~btn;
    }
}

static void sdl_destroy(void *vctx) {
    NesDisplaySdlCtx *c = vctx;
    if (!c) return;
    if (c->texture)  SDL_DestroyTexture(c->texture);
    if (c->renderer) SDL_DestroyRenderer(c->renderer);
    if (c->window)   SDL_DestroyWindow(c->window);
    SDL_QuitSubSystem(SDL_INIT_VIDEO);
    free(c->frame_argb);
    free(c);
}

static bool    sdl_should_quit(void *vctx) { return ((NesDisplaySdlCtx *)vctx)->quit; }
static uint8_t sdl_ctrl1(void *vctx)       { return ((NesDisplaySdlCtx *)vctx)->ctrl1; }

NesDisplay *nes_display_sdl_create(const char *title,
                                   const uint32_t *palette, int scale,
                                   GemuRendererType renderer_mode) {
    if (scale <= 0) scale = NES_SDL_DEFAULT_SCALE;
    if (SDL_InitSubSystem(SDL_INIT_VIDEO) < 0) return NULL;

    NesDisplaySdlCtx *c = calloc(1, sizeof(*c));
    if (!c) { SDL_QuitSubSystem(SDL_INIT_VIDEO); return NULL; }
    c->palette = palette;
    c->scale   = scale;
    c->frame_argb = malloc((size_t)256 * 240 * sizeof(*c->frame_argb));
    if (!c->frame_argb) goto fail;

    c->window = SDL_CreateWindow(
        title ? title : "gemu-6502 NES",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        256 * scale, 240 * scale,
        SDL_WINDOW_SHOWN);
    if (!c->window) goto fail;

    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "nearest");

    if (renderer_mode == GEMU_RENDERER_SOFTWARE) {
        c->renderer = SDL_CreateRenderer(c->window, -1, SDL_RENDERER_SOFTWARE);
    } else {
        c->renderer = SDL_CreateRenderer(c->window, -1, SDL_RENDERER_ACCELERATED);
    }
    if (!c->renderer && renderer_mode == GEMU_RENDERER_AUTO)
        c->renderer = SDL_CreateRenderer(c->window, -1, SDL_RENDERER_SOFTWARE);
    if (!c->renderer) goto fail;

    {
        SDL_RendererInfo info;
        if (SDL_GetRendererInfo(c->renderer, &info) == 0) {
            c->software = (info.flags & SDL_RENDERER_SOFTWARE) != 0;
            if (c->software) {
                const char *why = renderer_mode == GEMU_RENDERER_SOFTWARE
                    ? "forced" : "auto-detected";
                fprintf(stderr, "nes: using SDL software renderer (%s: %s)\n",
                        why, info.name ? info.name : "unknown");
            }
        }
    }

    SDL_RenderSetLogicalSize(c->renderer, 256, 240);

    c->texture = SDL_CreateTexture(
        c->renderer, SDL_PIXELFORMAT_ARGB8888,
        SDL_TEXTUREACCESS_STREAMING, 256, 240);
    if (!c->texture) goto fail;

    SDL_SetRenderDrawColor(c->renderer, 0, 0, 0, 255);
    SDL_RenderClear(c->renderer);
    SDL_RenderPresent(c->renderer);

    NesDisplay *d = calloc(1, sizeof(*d));
    if (!d) goto fail;
    d->render      = sdl_render;
    d->render_argb = sdl_render_argb;
    d->poll        = sdl_poll;
    d->destroy     = sdl_destroy;
    d->should_quit = sdl_should_quit;
    d->ctrl1       = sdl_ctrl1;
    d->ctx         = c;
    return d;

fail:
    sdl_destroy(c);
    return NULL;
}

NesDisplay *nes_display_create(GemuDisplayType type, const char *title,
                               const uint32_t *palette, int scale,
                               GemuRendererType renderer, GemuMonitor *mon) {
    switch (type) {
    case GEMU_DISPLAY_SDL:
        (void)mon;
        return nes_display_sdl_create(title, palette, scale, renderer);
#ifdef GEMU_GTK
    case GEMU_DISPLAY_GTK:
        (void)renderer;
        return nes_display_gtk_create(title, palette, scale, mon);
#endif
    default:
        return NULL;
    }
}
