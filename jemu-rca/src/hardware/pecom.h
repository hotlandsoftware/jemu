#pragma once
#include "cdp1802.h"
#include "cdp1861.h"
#include "rca.h"
#include "devices/pcspk.h"
#include "jemu/monitor.h"
#include "jemu/vnc.h"
#include "vga/rca_display.h"
#include <stdlib.h>

/* ── Pecom 32 machine state ──────────────────────────────────────────────── */

#define PECOM32_ROM_SIZE       0x4000u   /* 16 KB */
/*
 * Two 16 KB RAM banks:
 *   Bank 0: addresses 0x4000–0x7FFF  →  ram[0x0000–0x3FFF]
 *   Bank 1: addresses 0xC000–0xFFFF  →  ram[0x4000–0x7FFF]
 * ROM is visible (read-only) at 0x0000–0x3FFF and mirrored at 0x8000–0xBFFF.
 */
#define PECOM32_RAM_SIZE       0x8000u   /* 2 × 16 KB = 32 KB */

typedef struct RcaPecom32State {
    Cdp1802          cpu;
    Cdp1861          vdc;
    const RcaConfig *cfg;

    uint8_t  rom[PECOM32_ROM_SIZE];
    uint8_t  ram[PECOM32_RAM_SIZE];

    /* Pixel framebuffer: 1 byte per pixel (0 or 1), 64 × 128 */
    uint8_t  vram[CDP1861_DISPLAY_W * CDP1861_DISPLAY_H];
    bool     draw_flag;

    /* Keyboard: pending ASCII character, -1 = none */
    int      ascii_key;

    JemuMonitor  *monitor;
    JemuVncServer *vnc;
    RcaPcSpeaker *speaker;
} RcaPecom32State;

/* ── Public API ──────────────────────────────────────────────────────────── */

RcaPecom32State *rca_pecom32_create(const RcaConfig *cfg);
void             rca_pecom32_reset(RcaPecom32State *s, const RcaConfig *cfg);
void             rca_pecom32_destroy(RcaPecom32State *s);
void             rca_pecom32_run(RcaPecom32State *s, const RcaConfig *cfg);
