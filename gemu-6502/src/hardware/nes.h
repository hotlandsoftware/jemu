#pragma once
#include "mos6502cfg.h"
#include "../cpu/mos6502.h"
#include "../vga/rp2c02.h"
#include "../vga/nes_display.h"
#include "../audio/apu2a03.h"
#include "gemu/vnc.h"
#include "gemu/monitor.h"
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

    /* Mapper 1 (MMC1/SxROM) */
    uint8_t  mmc1_shift;         /* 5-bit serial shift register */
    uint8_t  mmc1_shift_count;   /* bits received so far (0–4) */
    uint8_t  mmc1_ctrl;          /* internal register: $8000–$9FFF */
    uint8_t  mmc1_chr0;          /* internal register: $A000–$BFFF */
    uint8_t  mmc1_chr1;          /* internal register: $C000–$DFFF */
    uint8_t  mmc1_prg;           /* internal register: $E000–$FFFF */
    uint32_t prg_offsets[2];     /* byte offsets into prg[]: [0]=$8000, [1]=$C000 */
    uint32_t chr_offsets[2];     /* byte offsets into chr[]: [0]=PPU$0000, [1]=PPU$1000 */
    uint8_t  prg_ram[0x2000];    /* 8 KB SRAM at $6000–$7FFF (shared mapper 1/4) */

    /* Mapper 4 (MMC3/TxROM) */
    uint8_t  mmc3_bank_sel;       /* bank select: bits[2:0]=target, [6]=PRG mode, [7]=CHR inv */
    uint8_t  mmc3_regs[8];        /* R0–R7 */
    uint8_t  mmc3_irq_latch;      /* reload value for IRQ counter */
    uint8_t  mmc3_irq_counter;    /* scanline IRQ counter */
    bool     mmc3_irq_reload;     /* counter reloads from latch on next clock */
    bool     mmc3_irq_enabled;
    uint32_t mmc3_prg_offsets[4]; /* 4× 8KB windows: $8000, $A000, $C000, $E000 */
    uint32_t mmc3_chr_offsets[8]; /* 8× 1KB windows: PPU $0000–$1FFF */

    Apu2a03  apu;

    /* OAM DMA state */
    bool     dma_pending;
    uint8_t  dma_page;

    /* Controllers */
    uint8_t  ctrl_state[2]; /* live button bits */
    uint8_t  ctrl_shift[2]; /* serial shift register */
    bool     ctrl_strobe;

    char           cart_path_buf[512]; /* path of currently loaded cartridge */
    bool           battery_autosave;  /* user opted in to save SRAM on exit */
    char           sav_path[512];     /* ~/.gemu/<game>.sav or AppData equivalent */

    NesDisplay    *display;  /* SDL window (NULL if headless) */
    GemuVncServer *vnc;
    GemuMonitor   *monitor;
} NesState;

NesState *nes_create (const MosConfig *cfg);
void      nes_run    (NesState *s, const MosConfig *cfg);
void      nes_destroy(NesState *s);
