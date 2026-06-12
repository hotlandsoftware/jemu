#pragma once
#include <stdint.h>
#include <stdbool.h>

#define RP2C02_WIDTH          256
#define RP2C02_HEIGHT         240
#define RP2C02_LINES_TOTAL    262   /* NTSC: 240 visible + 1 post + 20 vblank + 1 pre */
#define RP2C02_DOTS_PER_LINE  341

/* PPUCTRL (0x2000) bits */
#define PPUCTRL_NT_SELECT  0x03u  /* base nametable bits 0-1 */
#define PPUCTRL_VRAM_INC   0x04u  /* 0=+1 across, 1=+32 down */
#define PPUCTRL_SPR_PT     0x08u  /* sprite pattern table (8×8 only) */
#define PPUCTRL_BG_PT      0x10u  /* background pattern table */
#define PPUCTRL_SPR_8x16   0x20u  /* sprite size: 0=8×8, 1=8×16 */
#define PPUCTRL_NMI_EN     0x80u  /* generate NMI at start of VBlank */

/* PPUMASK (0x2001) bits */
#define PPUMASK_GRAY       0x01u
#define PPUMASK_BG_LEFT    0x02u  /* show BG in leftmost 8 pixels */
#define PPUMASK_SPR_LEFT   0x04u  /* show sprites in leftmost 8 pixels */
#define PPUMASK_SHOW_BG    0x08u
#define PPUMASK_SHOW_SPR   0x10u

/* PPUSTATUS (0x2002) bits */
#define PPUSTAT_SPR_OVF    0x20u
#define PPUSTAT_SPR0_HIT   0x40u
#define PPUSTAT_VBLANK     0x80u

/* Nametable mirroring modes */
#define RP2C02_MIRROR_HORIZONTAL  0  /* screens stacked vertically (vertical scroll) */
#define RP2C02_MIRROR_VERTICAL    1  /* screens side by side (horizontal scroll) */
#define RP2C02_MIRROR_SINGLE_A    2
#define RP2C02_MIRROR_SINGLE_B    3
#define RP2C02_MIRROR_4SCREEN     4

typedef struct Rp2c02 {
    /* ── Internal memory ──────────────────────────────────────────────── */
    uint8_t  vram[0x800];   /* 2 KB: two 1 KB nametables */
    uint8_t  oam[256];      /* 64 sprites × 4 bytes */
    uint8_t  palette[32];   /* palette RAM */

    /* ── Loopy scroll registers ───────────────────────────────────────── */
    uint16_t v;             /* current VRAM address (15 bits) */
    uint16_t t;             /* temporary VRAM address / scroll latch */
    uint8_t  x;             /* fine X scroll (3 bits) */
    bool     w;             /* write toggle (false=first, true=second) */

    /* ── CPU-visible registers ────────────────────────────────────────── */
    uint8_t  ppuctrl;
    uint8_t  ppumask;
    uint8_t  ppumask_pending; /* buffered write; takes effect after ppumask_delay ticks */
    int      ppumask_delay;   /* PPU cycles until ppumask_pending is applied; 0 = idle */
    uint8_t  ppustatus;
    uint8_t  oamaddr;
    uint8_t  read_buf;      /* PPUDATA read buffer (delayed by one read) */
    uint8_t  open_bus;      /* last value written to PPU (partial bus latch) */

    /* ── Timing ───────────────────────────────────────────────────────── */
    int      scanline;      /* 0–261 (261 = pre-render) */
    int      dot;           /* 0–340 */
    uint64_t frame;
    bool     odd_frame;     /* NTSC odd-frame skip dot at pre-render line */

    /* ── Status flags ─────────────────────────────────────────────────── */
    bool     nmi_pending;   /* CPU should take NMI on next step */

    /* ── Background pipeline ──────────────────────────────────────────── */
    uint8_t  nt_latch;
    uint8_t  at_latch;
    uint8_t  pt_lo_latch;
    uint8_t  pt_hi_latch;
    uint16_t bg_shift_lo;
    uint16_t bg_shift_hi;
    uint16_t bg_attr_lo;    /* replicated attribute bit 0 across 8 pixel positions */
    uint16_t bg_attr_hi;    /* replicated attribute bit 1 across 8 pixel positions */
    uint8_t  at_latch_lo;   /* loaded into bg_attr on reload */
    uint8_t  at_latch_hi;

    /* ── Sprite pipeline ──────────────────────────────────────────────── */
    uint8_t  oam2[32];      /* secondary OAM: up to 8 sprites × 4 bytes */
    uint8_t  n_spr;         /* sprites found for current scanline */
    bool     spr0_next;     /* sprite-0 is in oam2 */
    bool     spr0_active;   /* sprite-0 is rendering this scanline */
    uint8_t  spr_shift_lo[8];
    uint8_t  spr_shift_hi[8];
    uint8_t  spr_attr[8];
    uint8_t  spr_x[8];      /* X position counters, count down to 0 */

    /* ── Output framebuffer ───────────────────────────────────────────── */
    uint8_t  pixels[RP2C02_WIDTH * RP2C02_HEIGHT]; /* NES palette indices (0–63) */
    bool     dirty;         /* true when a new frame is complete */

    /* ── Mirroring ────────────────────────────────────────────────────── */
    uint8_t  mirror;        /* RP2C02_MIRROR_* */

    /* ── CHR bus (set by cartridge) ───────────────────────────────────── */
    uint8_t (*chr_read) (uint16_t addr, void *ud);
    void    (*chr_write)(uint16_t addr, uint8_t val, void *ud);
    void    *chr_ud;

    /* ── Mapper scanline IRQ hook (MMC3 etc.) ──────────────────────────── */
    void (*irq_scanline)(void *ud); /* fired at dot 260 of each rendered scanline */
    void *irq_ud;
} Rp2c02;

/* NES hardware palette: 64 entries → 0xAARRGGBB (AA=0xFF always) */
extern const uint32_t rp2c02_palette_rgb[64];

void    rp2c02_init (Rp2c02 *ppu);
void    rp2c02_reset(Rp2c02 *ppu);

/* CPU register access: addr 0x2000–0x2007 */
uint8_t rp2c02_read (Rp2c02 *ppu, uint8_t reg);
void    rp2c02_write(Rp2c02 *ppu, uint8_t reg, uint8_t val);

/* Step one PPU clock.  Call 3× per CPU cycle for NTSC timing. */
void    rp2c02_tick (Rp2c02 *ppu);

/* OAM DMA: bulk-copy 256 bytes into OAM (machine handles the CPU stall) */
void    rp2c02_oam_dma(Rp2c02 *ppu, const uint8_t *page);
