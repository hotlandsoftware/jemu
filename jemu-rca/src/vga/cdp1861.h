#pragma once
#include "cdp1802.h"
#include <stdint.h>
#include <stdbool.h>

typedef struct Cdp1861 {
    uint16_t line_counter;
    uint8_t  mcycle;         /* machine-cycle counter within the current scan line (0-13) */
    bool     display_on;
    uint16_t display_addr;   /* byte position within the framebuffer (0..1023) */

    /* DMA-out callback: called once per byte transferred from CPU memory to display */
    void   (*dma_out_cb)(uint8_t *byte, void *ud);
    void    *dma_out_ud;
} Cdp1861;

void cdp1861_init(Cdp1861 *vdc,
                  void (*dma_out_cb)(uint8_t *, void *), void *dma_ud);
void cdp1861_reset(Cdp1861 *vdc);

/* Called once per CPU machine cycle (from cpu->on_sync). */
void cdp1861_sync(Cdp1861 *vdc, Cdp1802 *cpu);

/* Toggle display enable — wired to CPU Q output on COSMAC VIP. */
static inline void cdp1861_set_display(Cdp1861 *vdc, bool on) {
    if (!on && vdc->display_on) vdc->display_addr = 0;
    vdc->display_on = on;
}
