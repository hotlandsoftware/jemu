#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include "gemu/display.h"
#include "gemu/monitor.h"

typedef struct NesDisplay NesDisplay;

struct NesDisplay {
    void    (*render)      (void *ctx, const uint8_t *pixels, int w, int h);
    void    (*render_argb) (void *ctx, const uint32_t *pixels, int w, int h);
    void    (*poll)        (void *ctx);
    void    (*destroy)     (void *ctx);
    bool    (*should_quit) (void *ctx);
    uint8_t (*ctrl1)       (void *ctx);
    void    *ctx;
};

/* Backend constructors */
NesDisplay *nes_display_sdl_create(const char *title,
                                   const uint32_t *palette, int scale,
                                   GemuRendererType renderer);
#ifdef GEMU_GTK
NesDisplay *nes_display_gtk_create(const char *title,
                                   const uint32_t *palette, int scale,
                                   GemuMonitor *mon);
#endif

/* Factory: picks the right backend based on display type */
NesDisplay *nes_display_create(GemuDisplayType type, const char *title,
                               const uint32_t *palette, int scale,
                               GemuRendererType renderer, GemuMonitor *mon);

static inline void    nes_display_render(NesDisplay *d, const uint8_t *px, int w, int h)
                          { if (d) d->render(d->ctx, px, w, h); }
static inline void    nes_display_render_argb(NesDisplay *d, const uint32_t *px,
                                              int w, int h) {
    if (!d) return;
    if (d->render_argb) d->render_argb(d->ctx, px, w, h);
}
static inline bool    nes_display_has_argb(NesDisplay *d)
                          { return d && d->render_argb; }
static inline void    nes_display_poll(NesDisplay *d)
                          { if (d && d->poll) d->poll(d->ctx); }
static inline bool    nes_display_should_quit(NesDisplay *d)
                          { return d && d->should_quit(d->ctx); }
static inline uint8_t nes_display_ctrl1(NesDisplay *d)
                          { return d ? d->ctrl1(d->ctx) : 0; }
static inline void    nes_display_destroy(NesDisplay *d) {
    if (!d) return;
    d->destroy(d->ctx);
    free(d);
}
