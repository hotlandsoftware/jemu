#pragma once
#include "mos6502cfg.h"
#include "../cpu/mos6502.h"
#include "../vga/rp2c02.h"
#include "../vga/nes_sdl.h"
#include "../audio/apu2a03.h"
#include "jemu/vnc.h"
#include "jemu/monitor.h"
#include <stdint.h>
#include <stdbool.h>

/* ── NES controller buttons (shift-register order: A comes out first) ─────── */
#define NES_BTN_A      0x01u
#define NES_BTN_B      0x02u
#define NES_BTN_SELECT 0x04u
#define NES_BTN_START  0x08u
#define NES_BTN_UP     0x10u
#define NES_BTN_DOWN   0x20u
#define NES_BTN_LEFT   0x40u
#define NES_BTN_RIGHT  0x80u

typedef struct {
    uint8_t  prg_banks;   /* × 16 KB */
    uint8_t  chr_banks;   /* × 8 KB  (0 = CHR RAM) */
    uint8_t  mapper;
    uint8_t  mirror;      /* RP2C02_MIRROR_* */
    bool     has_battery;
} NesCart;

typedef struct NesState {
    Mos6502  cpu;
    Rp2c02   ppu;
    const MosConfig *cfg;

    uint8_t  ram[0x800];  /* 2 KB CPU-side work RAM */

    /* Cartridge */
    NesCart  cart;
    uint8_t *prg;         /* PRG ROM (heap) */
    uint8_t *chr;         /* CHR ROM or CHR RAM (heap) */
    bool     chr_is_ram;

    Apu2a03  apu;

    /* OAM DMA state */
    bool     dma_pending;
    uint8_t  dma_page;

    /* Controllers */
    uint8_t  ctrl_state[2]; /* live button bits */
    uint8_t  ctrl_shift[2]; /* serial shift register */
    bool     ctrl_strobe;

    NesDisplay    *display;  /* SDL window (NULL if headless) */
    JemuVncServer *vnc;
    JemuMonitor   *monitor;
} NesState;

NesState *nes_create (const MosConfig *cfg);
void      nes_run    (NesState *s, const MosConfig *cfg);
void      nes_destroy(NesState *s);
