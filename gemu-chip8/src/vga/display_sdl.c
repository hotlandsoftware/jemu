#include "chip8.h"
#include "gemu/video.h"
#include <stdlib.h>

/* Plain SDL2 display — no menu bar. The window is the game, nothing else. */

typedef struct {
    GemuVideoSdl *video;
} SdlCtx;

static void sdl_render(void *ctx, const uint8_t *vram) {
    SdlCtx *c = ctx;
    gemu_video_sdl_present_mono(c->video, vram,
                                CHIP8_DISPLAY_W, CHIP8_DISPLAY_H,
                                0xFFFFFFFFu, 0xFF000000u);
}

static void sdl_destroy(void *ctx) {
    SdlCtx *c = ctx;
    gemu_video_sdl_destroy(c->video);
    free(c);
}

Chip8Display *chip8_display_sdl_create(int scale) {
    SdlCtx *c = calloc(1, sizeof(*c));
    if (!c) return NULL;
    c->video = gemu_video_sdl_create(&(GemuVideoSdlSpec){
        .title         = "GEMU",
        .width         = CHIP8_DISPLAY_W,
        .height        = CHIP8_DISPLAY_H,
        .window_width  = CHIP8_DISPLAY_W * scale,
        .window_height = CHIP8_DISPLAY_H * scale,
        .renderer      = GEMU_RENDERER_AUTO,
        .log_prefix    = "chip8",
    });
    if (!c->video) { free(c); return NULL; }

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
