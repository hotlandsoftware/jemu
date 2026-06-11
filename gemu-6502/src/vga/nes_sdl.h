#pragma once
#include <stdint.h>
#include <stdbool.h>

typedef struct NesDisplay NesDisplay;

/* Create an SDL window for NES output.
 * palette: 64-entry ARGB table (rp2c02_palette_rgb).
 * scale: window multiplier; pass 0 for the default (2). */
NesDisplay *nes_display_create(const char *title,
                               const uint32_t *palette,
                               int scale);
void        nes_display_destroy(NesDisplay *d);

/* Push one frame.  pixels: 1 byte/pixel NES palette index, w×h. */
void        nes_display_render(NesDisplay *d,
                               const uint8_t *pixels, int w, int h);

/* Poll SDL events.  Call once per frame. */
void        nes_display_poll(NesDisplay *d);

bool        nes_display_should_quit(NesDisplay *d);

/* Current player 1 controller byte (NES_BTN_* bits). */
uint8_t     nes_display_ctrl1(NesDisplay *d);
