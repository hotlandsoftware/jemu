#include "cdp1861.h"
#include <string.h>

void cdp1861_init(Cdp1861 *vdc,
                  void (*dma_out_cb)(uint8_t *, void *), void *dma_ud) {
    memset(vdc, 0, sizeof(*vdc));
    vdc->dma_out_cb = dma_out_cb;
    vdc->dma_out_ud = dma_ud;
}

void cdp1861_reset(Cdp1861 *vdc) {
    vdc->line_counter  = 0;
    vdc->mcycle        = 0;
    vdc->display_addr  = 0;
    vdc->display_on    = false;
}

void cdp1861_sync(Cdp1861 *vdc, Cdp1802 *cpu) {
    if (vdc->display_on) {
        uint16_t lc = vdc->line_counter;
        uint8_t  mc = vdc->mcycle;

        /* Issue DMA-out request at machine cycle 2 of each active line */
        if (lc >= CDP1861_FIRST_LINE && lc <= CDP1861_LAST_LINE && mc == 2)
            cdp1802_request_dma_out(cpu, 8, vdc->dma_out_cb, vdc->dma_out_ud);

        /* Interrupt at start of line 62 (one line before active display begins) */
        if (lc == 62 && mc == 0)
            cdp1802_request_irq(cpu);

        /* EF1 (index 0) is the display blanking signal (EFX) */
        if (mc == 0) {
            bool efx = (lc >= 60 && lc <= 63) || (lc >= 188 && lc <= 191);
            cpu->EF[0] = efx;
        }
    }

    if (++vdc->mcycle == CDP1861_MCYCLES_PER_LINE) {
        vdc->mcycle = 0;
        if (++vdc->line_counter == CDP1861_LINES_TOTAL)
            vdc->line_counter = 0;
    }
}
