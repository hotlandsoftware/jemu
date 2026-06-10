#pragma once
#include "cdp1802.h"
#include "cdp1869.h"
#include "rca.h"
#include "devices/pcspk.h"
#include "jemu/monitor.h"
#include "jemu/vnc.h"
#include "vga/rca_display.h"
#include <stdlib.h>

/* ── Pecom 32 machine state ──────────────────────────────────────────────── */

/*
 * Memory map:
 *   0x0000–0x7FFF  RAM bank 1 (32 KB)         — ram1[]
 *   0x8000–0xBFFF  ROM (16 KB, read-only)      — rom[]
 *   0xC000–0xF3FF  RAM bank 2 (13 KB)          — ram2[]
 *   0xF400–0xF7FF  VIS-1870 char RAM (1 KB)    — cdp1869 char RAM
 *   0xF800–0xFFFF  VIS-1870 page RAM (2 KB)    — cdp1869 page RAM
 *
 * Bootstrap: ROM also appears at 0x0000–0x3FFF on reset until OUT 1 fires.
 */
#define PECOM32_ROM_SIZE      0x4000u   /* 16 KB */
#define PECOM32_RAM1_SIZE     0x8000u   /* 32 KB: 0x0000–0x7FFF */
#define PECOM32_RAM2_SIZE     0x4000u   /* 16 KB: 0xC000–0xFFFF (VIS RAM at top) */
#define PECOM32_CRAM_BASE     0xF400u   /* VIS-1870 char RAM start */
#define PECOM32_PRAM_BASE     0xF800u   /* VIS-1870 page RAM start */

#define PECOM32_FRAME_HZ      50u
/* CDP1869 PAL timing: 312 lines × 14 machine cycles per line */
#define PECOM32_MCYCLES_PER_FRAME  CDP1869_MCYCLES_PER_FRAME

typedef struct RcaPecom32State {
    Cdp1802          cpu;
    Cdp1869          vis;
    const RcaConfig *cfg;

    uint8_t  rom[PECOM32_ROM_SIZE];
    uint8_t  ram1[PECOM32_RAM1_SIZE];
    uint8_t  ram2[PECOM32_RAM2_SIZE];

    bool     boot_mirror;   /* ROM mirrored at 0x0000 until first OUT 1 */
    uint8_t  iogroup;       /* current I/O group: 0=keyboard, 2=VIS-1870 */

    /* ── Keyboard matrix ────────────────────────────────────────────── */
    /* The Pecom 32 uses a matrix keyboard scanned via IN 3.
     * The IN 3 instruction uses the memory address bus (R[X] & 0x3F) as the
     * row selector; the returned byte has bits 0–1 set for each pressed key
     * in that row (active-high: 1 = pressed).
     * EF1 = CTRL (AND with VIS display), EF2 = SHIFT, EF3 = CAPS (pol=rev),
     * EF4 = ESC. */
    uint8_t  keys[64];      /* key matrix: keys[row] bitmask, bits 0/1 per key */

    bool     key_shift;
    bool     key_ctrl;
    bool     key_esc;
    bool     caps_locked;

    JemuMonitor  *monitor;
    JemuVncServer *vnc;
    RcaPcSpeaker *speaker;
} RcaPecom32State;

/* ── Public API ──────────────────────────────────────────────────────────── */

RcaPecom32State *rca_pecom32_create(const RcaConfig *cfg);
void             rca_pecom32_reset(RcaPecom32State *s, const RcaConfig *cfg);
void             rca_pecom32_destroy(RcaPecom32State *s);
void             rca_pecom32_run(RcaPecom32State *s, const RcaConfig *cfg);
