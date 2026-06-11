#include "rca_display.h"
#include "cdp1802.h"
#include <SDL2/SDL.h>
#include <stdbool.h>
#include <stdlib.h>

#define VIP_DEFAULT_SCALE 4
#define RCA_SDL_MAX_KEYS 128

typedef struct {
    SDL_Window   *window;
    SDL_Renderer *renderer;
    SDL_Texture  *texture;
    int           scale;
    const uint32_t *palette;
    int           n_colors;
    uint32_t      pixel_on;
    uint32_t      pixel_off;
    bool          quit;
    struct {
        uint32_t keysym;
        bool     down;
    } keys[RCA_SDL_MAX_KEYS];
    int           n_keys;
    uint32_t      queued_keysym;
} VipSdlCtx;

/* Match Emma02's COSMAC VIP presentation: white pixels on deep blue. */
#define PIXEL_ON  0xFFFFFFFFu
#define PIXEL_OFF 0xFF100080u

static void sdl_render(void *ctx, const uint8_t *vram, int w, int h) {
    VipSdlCtx *c = ctx;
    void *pixels; int pitch;
    SDL_LockTexture(c->texture, NULL, &pixels, &pitch);
    uint32_t *px = pixels;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
            if (c->palette && c->n_colors > 0) {
                uint8_t idx = vram[y * w + x];
                if (idx >= c->n_colors) idx = (uint8_t)(c->n_colors - 1);
                px[y * (pitch / 4) + x] = c->palette[idx];
            } else {
                px[y * (pitch / 4) + x] = vram[y * w + x] ? c->pixel_on : c->pixel_off;
            }
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

static uint32_t sdl_key_to_rca(SDL_Keycode key) {
    if (key >= 0x20 && key <= 0x7e)
        return (uint32_t)key;
    switch (key) {
    case SDLK_ESCAPE: return RCA_KEY_ESCAPE;
    case SDLK_TAB:    return RCA_KEY_TAB;
    case SDLK_F2:     return RCA_KEY_F2;
    case SDLK_F3:     return RCA_KEY_F3;
    case SDLK_LEFT:   return RCA_KEY_LEFT;
    case SDLK_RIGHT:  return RCA_KEY_RIGHT;
    case SDLK_LALT:   return RCA_KEY_LALT;
    case SDLK_KP_0:   return RCA_KEY_KP_0;
    case SDLK_KP_1:   return RCA_KEY_KP_1;
    case SDLK_KP_2:   return RCA_KEY_KP_2;
    case SDLK_KP_3:   return RCA_KEY_KP_3;
    case SDLK_KP_4:   return RCA_KEY_KP_4;
    case SDLK_KP_5:   return RCA_KEY_KP_5;
    case SDLK_KP_6:   return RCA_KEY_KP_6;
    case SDLK_KP_7:   return RCA_KEY_KP_7;
    case SDLK_KP_8:   return RCA_KEY_KP_8;
    case SDLK_KP_9:   return RCA_KEY_KP_9;
    case SDLK_RETURN: return '\r';
    case SDLK_BACKSPACE: return '\b';
    default:          return 0;
    }
}

static int find_key(VipSdlCtx *c, uint32_t keysym) {
    for (int i = 0; i < c->n_keys; i++)
        if (c->keys[i].keysym == keysym)
            return i;
    return -1;
}

static void set_key(VipSdlCtx *c, uint32_t keysym, bool down) {
    if (keysym >= 'A' && keysym <= 'Z')
        keysym += 'a' - 'A';
    int idx = find_key(c, keysym);
    if (idx < 0) {
        if (c->n_keys >= RCA_SDL_MAX_KEYS)
            return;
        idx = c->n_keys++;
        c->keys[idx].keysym = keysym;
    }
    c->keys[idx].down = down;
    if (down)
        c->queued_keysym = keysym;
}

static void sdl_poll(void *ctx) {
    VipSdlCtx *c = ctx;
    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {
        if (ev.type == SDL_QUIT) {
            c->quit = true;
            continue;
        }
        if (ev.type == SDL_TEXTINPUT) {
            unsigned char ch = (unsigned char)ev.text.text[0];
            if (ch >= 0x20 && ch <= 0x7e)
                c->queued_keysym = ch;
            continue;
        }
        if (ev.type != SDL_KEYDOWN && ev.type != SDL_KEYUP)
            continue;

        bool down = ev.type == SDL_KEYDOWN;
        uint32_t sym = sdl_key_to_rca(ev.key.keysym.sym);
        if (!sym)
            continue;
        if (down && sym == RCA_KEY_ESCAPE)
            c->quit = true;
        set_key(c, sym, down);
    }
}

static bool sdl_should_quit(void *ctx) {
    return ((VipSdlCtx *)ctx)->quit;
}

static bool sdl_key_down(void *ctx, uint32_t keysym) {
    VipSdlCtx *c = ctx;
    if (keysym >= 'A' && keysym <= 'Z')
        keysym += 'a' - 'A';
    int idx = find_key(c, keysym);
    return idx >= 0 && c->keys[idx].down;
}

static uint32_t sdl_pop_keysym(void *ctx) {
    VipSdlCtx *c = ctx;
    uint32_t keysym = c->queued_keysym;
    c->queued_keysym = 0;
    return keysym;
}

RcaDisplay *rca_display_sdl_create(int scale) {
    return rca_display_sdl_create_mono("GEMU", CDP1861_DISPLAY_W,
                                       CDP1861_DISPLAY_H, scale,
                                       PIXEL_ON, PIXEL_OFF);
}

static RcaDisplay *sdl_create_common(const char *title, int w, int h,
                                     int scale, const uint32_t *palette,
                                     int n_colors, uint32_t on,
                                     uint32_t off) {
    if (scale <= 0) scale = VIP_DEFAULT_SCALE;
    if (SDL_InitSubSystem(SDL_INIT_VIDEO) < 0) return NULL;

    VipSdlCtx *c = calloc(1, sizeof(*c));
    c->scale = scale;
    c->palette = palette;
    c->n_colors = n_colors;
    c->pixel_on = on;
    c->pixel_off = off;
    c->window = SDL_CreateWindow(title ? title : "GEMU", SDL_WINDOWPOS_CENTERED,
                                 SDL_WINDOWPOS_CENTERED,
                                 w * scale, h * scale, SDL_WINDOW_SHOWN);
    if (!c->window) { free(c); SDL_QuitSubSystem(SDL_INIT_VIDEO); return NULL; }

    c->renderer = SDL_CreateRenderer(c->window, -1,
                      SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!c->renderer) { SDL_DestroyWindow(c->window); free(c); return NULL; }

    SDL_RenderSetLogicalSize(c->renderer, w * scale, h * scale);
    c->texture = SDL_CreateTexture(c->renderer, SDL_PIXELFORMAT_ARGB8888,
                                   SDL_TEXTUREACCESS_STREAMING, w, h);
    if (!c->texture) {
        SDL_DestroyRenderer(c->renderer);
        SDL_DestroyWindow(c->window);
        free(c);
        return NULL;
    }

    SDL_SetRenderDrawColor(c->renderer, 0, 0, 0, 255);
    SDL_RenderClear(c->renderer);
    SDL_RenderPresent(c->renderer);

    RcaDisplay *d = calloc(1, sizeof(*d));
    d->render = sdl_render;
    d->destroy = sdl_destroy;
    d->run = NULL;
    d->poll = sdl_poll;
    d->should_quit = sdl_should_quit;
    d->key_down = sdl_key_down;
    d->pop_keysym = sdl_pop_keysym;
    d->ctx = c;
    return d;
}

RcaDisplay *rca_display_sdl_create_mono(const char *title, int w, int h,
                                        int scale, uint32_t on,
                                        uint32_t off) {
    return sdl_create_common(title, w, h, scale, NULL, 0, on, off);
}

RcaDisplay *rca_display_sdl_create_indexed(const char *title, int w, int h,
                                           int scale,
                                           const uint32_t *palette,
                                           int n_colors) {
    return sdl_create_common(title, w, h, scale, palette, n_colors,
                             PIXEL_ON, PIXEL_OFF);
}

RcaDisplay *rca_display_create_mono(GemuDisplayType type, const char *title,
                                    int w, int h, int scale,
                                    uint32_t on, uint32_t off,
                                    GemuMonitor *mon) {
    switch (type) {
    case GEMU_DISPLAY_SDL:
        return rca_display_sdl_create_mono(title, w, h, scale, on, off);
#ifdef GEMU_GTK
    case GEMU_DISPLAY_GTK:
        return rca_display_gtk_create_mono(title, w, h, scale, on, off, mon);
#endif
    case GEMU_DISPLAY_CURSES:
        return rca_display_curses_create();
    default:
        return rca_display_none_create();
    }
}

RcaDisplay *rca_display_create_indexed(GemuDisplayType type, const char *title,
                                       int w, int h, int scale,
                                       const uint32_t *palette, int n_colors,
                                       GemuMonitor *mon) {
    switch (type) {
    case GEMU_DISPLAY_SDL:
        return rca_display_sdl_create_indexed(title, w, h, scale,
                                              palette, n_colors);
#ifdef GEMU_GTK
    case GEMU_DISPLAY_GTK:
        return rca_display_gtk_create_indexed(title, w, h, scale,
                                              palette, n_colors, mon);
#endif
    case GEMU_DISPLAY_CURSES:
        return rca_display_curses_create();
    default:
        return rca_display_none_create();
    }
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
