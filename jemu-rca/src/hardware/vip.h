#pragma once
#include "cdp1802.h"
#include "cdp1861.h"
#include "cdp1869.h"
#include "rca.h"
#include "devices/pcspk.h"
#include "jemu/monitor.h"
#include "jemu/vnc.h"
#include <stdlib.h>

/* ── COSMAC VIP machine state ────────────────────────────────────────────── */

#define VIP_MEM_SIZE 65536u

typedef struct RcaVipState {
    Cdp1802  cpu;
    Cdp1861  vdc;
    Cdp1869  vis;
    const RcaConfig *cfg;
    uint8_t  mem[VIP_MEM_SIZE];
    JemuMonitor *monitor;
    JemuVncServer *vnc;
    RcaPcSpeaker *speaker;

    /* Pixel framebuffer: 1 byte per pixel (0 or 1), 64 wide × 128 tall */
    uint8_t  vram[CDP1861_DISPLAY_W * CDP1861_DISPLAY_H];
    bool     draw_flag;

    /* Hex keypad state */
    uint8_t  keys[16];    /* 1 = pressed */
    int      key_down;    /* most-recent key held, -1 = none */
    int      ascii_key;   /* pending ASCII key, -1 = none */

    /* Simple front-panel inputs used by VIP loaders/monitors. */
    bool     ef2_down;
} RcaVipState;

/* ── RCA display abstraction (mirrors chip8's Chip8Display) ──────────────── */

typedef struct RcaDisplay RcaDisplay;
struct RcaDisplay {
    /* render() is called each frame when draw_flag is set */
    void (*render)(void *ctx, const uint8_t *vram, int w, int h);
    void (*destroy)(void *ctx);
    /* run() owns the main loop; NULL means SDL2 loop in machine.c */
    void (*run)(RcaDisplay *d, RcaVipState *s, const RcaConfig *cfg);
    void  *ctx;
};

/* ── Public API ──────────────────────────────────────────────────────────── */

RcaVipState *rca_vip_create(const RcaConfig *cfg);
void         rca_vip_reset(RcaVipState *s, const RcaConfig *cfg);
void         rca_vip_destroy(RcaVipState *s);
void         rca_machine_run(RcaVipState *s, const RcaConfig *cfg);

RcaDisplay  *rca_display_sdl_create(int scale);
RcaDisplay  *rca_display_sdl_create_indexed(const char *title, int w, int h,
                                            int scale,
                                            const uint32_t *palette,
                                            int n_colors);
RcaDisplay  *rca_display_curses_create(void);
void         rca_display_curses_poll_vip(RcaDisplay *d, RcaVipState *s, bool *quit);
RcaDisplay  *rca_display_none_create(void);

static inline void rca_display_render(RcaDisplay *d, const uint8_t *vram, int w, int h) {
    d->render(d->ctx, vram, w, h);
}
static inline void rca_display_destroy(RcaDisplay *d) {
    if (!d) return;
    d->destroy(d->ctx);
    free(d);
}
