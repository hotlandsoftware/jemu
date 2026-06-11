#include "rp2c02.h"
#include <string.h>
#include <stddef.h>

/* ── NES hardware palette (Nestopia/2C02 canonical) ─────────────────────── */

const uint32_t rp2c02_palette_rgb[64] = {
    0xFF666666, 0xFF002A88, 0xFF1412A7, 0xFF3B00A4,
    0xFF5C007E, 0xFF6E0040, 0xFF6C0600, 0xFF561D00,
    0xFF333500, 0xFF0B4800, 0xFF005200, 0xFF004F08,
    0xFF00404D, 0xFF000000, 0xFF000000, 0xFF000000,
    0xFFADADAD, 0xFF155FD9, 0xFF4240FF, 0xFF7527FE,
    0xFFA01ACC, 0xFFB71E7B, 0xFFB53120, 0xFF994E00,
    0xFF6B6D00, 0xFF388700, 0xFF0C9300, 0xFF008F32,
    0xFF007C8D, 0xFF000000, 0xFF000000, 0xFF000000,
    0xFFFFFEFF, 0xFF64B0FF, 0xFF9290FF, 0xFFC676FF,
    0xFFF36AFF, 0xFFFE6ECC, 0xFFFE8170, 0xFFEA9E22,
    0xFFBCBE00, 0xFF88D800, 0xFF5CE430, 0xFF45E082,
    0xFF48CDDE, 0xFF4F4F4F, 0xFF000000, 0xFF000000,
    0xFFFFFEFF, 0xFFC0DFFF, 0xFFD3D2FF, 0xFFE8C8FF,
    0xFFFBC2FF, 0xFFFEC4EA, 0xFFFECCC5, 0xFFF7D8A5,
    0xFFE4E594, 0xFFCFEF96, 0xFFBDF4AB, 0xFFB3F3CC,
    0xFFB5EBF2, 0xFFB8B8B8, 0xFF000000, 0xFF000000,
};

/* ── Nametable mirroring ─────────────────────────────────────────────────── */

/* Returns an index into ppu->vram[] for a PPU nametable address 0x2000–0x2FFF */
static uint16_t nt_mirror(const Rp2c02 *ppu, uint16_t addr) {
    /* Select nametable slot 0–3 from bits 11-10 */
    static const uint8_t tbl[5][4] = {
        {0, 0, 1, 1},   /* HORIZONTAL  */
        {0, 1, 0, 1},   /* VERTICAL    */
        {0, 0, 0, 0},   /* SINGLE_A    */
        {1, 1, 1, 1},   /* SINGLE_B    */
        {0, 1, 2, 3},   /* 4-SCREEN (only 2 KB available — wrap) */
    };
    uint8_t m = ppu->mirror < 5 ? ppu->mirror : 1;
    uint16_t nt   = (addr >> 10) & 3;
    uint16_t off  = addr & 0x3FF;
    return (uint16_t)((tbl[m][nt] & 1) * 0x400u + off);
}

/* ── Palette RAM ─────────────────────────────────────────────────────────── */

static uint8_t palette_rd(const Rp2c02 *ppu, uint8_t addr) {
    addr &= 0x1F;
    /* $3F10/$3F14/$3F18/$3F1C mirror $3F00/$3F04/$3F08/$3F0C */
    if ((addr & 0x13) == 0x10) addr &= ~0x10u;
    uint8_t v = ppu->palette[addr] & 0x3F;
    if (ppu->ppumask & PPUMASK_GRAY) v &= 0x30;
    return v;
}

static void palette_wr(Rp2c02 *ppu, uint8_t addr, uint8_t val) {
    addr &= 0x1F;
    if ((addr & 0x13) == 0x10) addr &= ~0x10u;
    ppu->palette[addr] = val & 0x3F;
}

/* ── PPU address-space bus ───────────────────────────────────────────────── */

static uint8_t ppu_bus_rd(Rp2c02 *ppu, uint16_t addr) {
    addr &= 0x3FFF;
    if (addr < 0x2000) {
        return ppu->chr_read ? ppu->chr_read(addr, ppu->chr_ud) : 0;
    } else if (addr < 0x3F00) {
        return ppu->vram[nt_mirror(ppu, addr & 0x2FFF)];
    } else {
        return palette_rd(ppu, (uint8_t)(addr & 0x1F));
    }
}

static void ppu_bus_wr(Rp2c02 *ppu, uint16_t addr, uint8_t val) {
    addr &= 0x3FFF;
    if (addr < 0x2000) {
        if (ppu->chr_write) ppu->chr_write(addr, val, ppu->chr_ud);
    } else if (addr < 0x3F00) {
        ppu->vram[nt_mirror(ppu, addr & 0x2FFF)] = val;
    } else {
        palette_wr(ppu, (uint8_t)(addr & 0x1F), val);
    }
}

/* ── CPU register interface ──────────────────────────────────────────────── */

uint8_t rp2c02_read(Rp2c02 *ppu, uint8_t reg) {
    uint8_t val = ppu->open_bus;
    switch (reg & 7) {
    case 2: /* PPUSTATUS */
        val = (ppu->ppustatus & 0xE0) | (ppu->open_bus & 0x1F);
        ppu->ppustatus &= ~PPUSTAT_VBLANK;  /* reading clears VBlank flag */
        ppu->w = false;
        /* Suppress NMI if read happens at the same dot VBlank was set */
        ppu->nmi_pending = false;
        break;
    case 4: /* OAMDATA */
        val = ppu->oam[ppu->oamaddr];
        break;
    case 7: /* PPUDATA */
        if ((ppu->v & 0x3FFF) < 0x3F00) {
            /* Buffered read: return old buffer, refill from bus */
            val = ppu->read_buf;
            ppu->read_buf = ppu_bus_rd(ppu, ppu->v);
        } else {
            /* Palette: no delay, but buffer gets the nametable byte underneath */
            val = palette_rd(ppu, (uint8_t)(ppu->v & 0x1F));
            ppu->read_buf = ppu->vram[nt_mirror(ppu, ppu->v & 0x2FFF)];
        }
        ppu->v += (ppu->ppuctrl & PPUCTRL_VRAM_INC) ? 32 : 1;
        ppu->v &= 0x7FFF;
        break;
    default:
        break;
    }
    ppu->open_bus = val;
    return val;
}

void rp2c02_write(Rp2c02 *ppu, uint8_t reg, uint8_t val) {
    ppu->open_bus = val;
    switch (reg & 7) {
    case 0: /* PPUCTRL */
        ppu->ppuctrl = val;
        /* t[11:10] = ctrl[1:0] (nametable select) */
        ppu->t = (ppu->t & 0xF3FF) | (uint16_t)((val & 3) << 10);
        /* If VBlank flag is still set and NMI just enabled, fire immediately */
        if ((ppu->ppustatus & PPUSTAT_VBLANK) && (val & PPUCTRL_NMI_EN))
            ppu->nmi_pending = true;
        break;
    case 1: /* PPUMASK */
        ppu->ppumask_pending = val;
        ppu->ppumask_delay   = 3; /* 2-5 PPU cycle hardware delay before taking effect */
        break;
    case 3: /* OAMADDR */
        ppu->oamaddr = val;
        break;
    case 4: /* OAMDATA */
        ppu->oam[ppu->oamaddr++] = val;
        break;
    case 5: /* PPUSCROLL */
        if (!ppu->w) {
            /* First write: fine X and coarse X into t */
            ppu->t = (ppu->t & 0xFFE0) | (val >> 3);
            ppu->x = val & 7;
        } else {
            /* Second write: coarse Y and fine Y into t */
            ppu->t = (ppu->t & 0x8C1F)
                   | (uint16_t)((val & 0xF8) << 2)   /* coarse Y → t[9:5] */
                   | (uint16_t)((val & 0x07) << 12);  /* fine Y   → t[14:12] */
        }
        ppu->w = !ppu->w;
        break;
    case 6: /* PPUADDR */
        if (!ppu->w) {
            /* First write: high 6 bits of address; clear bit 14 */
            ppu->t = (ppu->t & 0x00FF) | (uint16_t)((val & 0x3F) << 8);
        } else {
            /* Second write: low 8 bits; latch into v */
            ppu->t = (ppu->t & 0xFF00) | val;
            ppu->v = ppu->t;
        }
        ppu->w = !ppu->w;
        break;
    case 7: /* PPUDATA */
        ppu_bus_wr(ppu, ppu->v, val);
        ppu->v += (ppu->ppuctrl & PPUCTRL_VRAM_INC) ? 32 : 1;
        ppu->v &= 0x7FFF;
        break;
    }
}

void rp2c02_oam_dma(Rp2c02 *ppu, const uint8_t *page) {
    /* Machine must stall CPU 513/514 cycles; we just copy the data */
    memcpy(ppu->oam, page, 256);
}

/* ── Background rendering helpers ────────────────────────────────────────── */

static void incr_coarse_x(Rp2c02 *ppu) {
    if ((ppu->v & 0x001F) == 31) {
        ppu->v &= ~0x001Fu;
        ppu->v ^= 0x0400u;  /* flip horizontal nametable */
    } else {
        ppu->v++;
    }
}

static void incr_y(Rp2c02 *ppu) {
    if ((ppu->v & 0x7000) != 0x7000) {
        ppu->v += 0x1000;   /* increment fine Y */
    } else {
        ppu->v &= ~0x7000u; /* fine Y = 0 */
        uint16_t cy = (ppu->v >> 5) & 0x1F;
        if      (cy == 29) { cy = 0; ppu->v ^= 0x0800u; } /* flip vertical NT */
        else if (cy == 31)   cy = 0;  /* out-of-range wrap without NT flip */
        else                 cy++;
        ppu->v = (ppu->v & ~0x03E0u) | (cy << 5);
    }
}

static void copy_x_from_t(Rp2c02 *ppu) {
    /* v[4:0] = t[4:0]  (coarse X);  v[10] = t[10] (H nametable) */
    ppu->v = (ppu->v & ~0x041Fu) | (ppu->t & 0x041Fu);
}

static void copy_y_from_t(Rp2c02 *ppu) {
    /* v[14:11] = t[14:11] (fine Y, V NT);  v[9:5] = t[9:5] (coarse Y) */
    ppu->v = (ppu->v & ~0x7BE0u) | (ppu->t & 0x7BE0u);
}

static void bg_shift_tick(Rp2c02 *ppu) {
    ppu->bg_shift_lo <<= 1;
    ppu->bg_shift_hi  = (ppu->bg_shift_hi << 1) | 1; /* serial-in tied high on 2C02 */
    ppu->bg_attr_lo  <<= 1;
    ppu->bg_attr_hi  <<= 1;
}

static void bg_shift_reload(Rp2c02 *ppu) {
    ppu->bg_shift_lo = (ppu->bg_shift_lo & 0xFF00) | ppu->pt_lo_latch;
    ppu->bg_shift_hi = (ppu->bg_shift_hi & 0xFF00) | ppu->pt_hi_latch;
    ppu->bg_attr_lo  = (ppu->bg_attr_lo  & 0xFF00) | (ppu->at_latch_lo ? 0xFF : 0x00);
    ppu->bg_attr_hi  = (ppu->bg_attr_hi  & 0xFF00) | (ppu->at_latch_hi ? 0xFF : 0x00);
}

static uint8_t fetch_attr(Rp2c02 *ppu) {
    uint16_t at_addr = 0x23C0u
        | (ppu->v & 0x0C00u)
        | ((ppu->v >> 4) & 0x38u)
        | ((ppu->v >> 2) & 0x07u);
    uint8_t at = ppu_bus_rd(ppu, at_addr);
    /* Select the 2-bit field for this tile */
    uint8_t shift = (uint8_t)(((ppu->v >> 4) & 4u) | ((ppu->v >> 1) & 2u));
    return (at >> shift) & 3;
}

static void do_bg_fetches(Rp2c02 *ppu, int dot) {
    /* Each 8-dot group: NT(1), AT(3), PT-lo(5), PT-hi(7), reload+incrX(8) */
    switch (dot & 7) {
    case 1:
        bg_shift_reload(ppu);
        ppu->nt_latch = ppu_bus_rd(ppu, 0x2000u | (ppu->v & 0x0FFFu));
        break;
    case 3: {
        uint8_t attr = fetch_attr(ppu);
        ppu->at_latch_lo = attr & 1;
        ppu->at_latch_hi = (attr >> 1) & 1;
        break;
    }
    case 5: {
        uint16_t base = (ppu->ppuctrl & PPUCTRL_BG_PT) ? 0x1000u : 0x0000u;
        uint8_t  fy   = (uint8_t)((ppu->v >> 12) & 7);
        ppu->pt_lo_latch = ppu_bus_rd(ppu, base | ((uint16_t)ppu->nt_latch << 4) | fy);
        break;
    }
    case 7: {
        uint16_t base = (ppu->ppuctrl & PPUCTRL_BG_PT) ? 0x1000u : 0x0000u;
        uint8_t  fy   = (uint8_t)((ppu->v >> 12) & 7);
        ppu->pt_hi_latch = ppu_bus_rd(ppu, base | ((uint16_t)ppu->nt_latch << 4) | fy | 8u);
        incr_coarse_x(ppu);
        break;
    }
    }
}

/* ── Sprite evaluation (end of visible scanline, dot 257) ────────────────── */

static uint8_t bit_reverse(uint8_t b) {
    b = (uint8_t)(((b & 0xF0) >> 4) | ((b & 0x0F) << 4));
    b = (uint8_t)(((b & 0xCC) >> 2) | ((b & 0x33) << 2));
    b = (uint8_t)(((b & 0xAA) >> 1) | ((b & 0x55) << 1));
    return b;
}

static void evaluate_sprites(Rp2c02 *ppu) {
    int next_sl = ppu->scanline + 1;
    int spr_h   = (ppu->ppuctrl & PPUCTRL_SPR_8x16) ? 16 : 8;
    ppu->n_spr     = 0;
    ppu->spr0_next = false;

    for (int i = 0; i < 64 && ppu->n_spr < 9; i++) {
        int y = (int)ppu->oam[i * 4] + 1;
        if (next_sl < y || next_sl >= y + spr_h) continue;

        if (ppu->n_spr < 8) {
            memcpy(ppu->oam2 + ppu->n_spr * 4, ppu->oam + i * 4, 4);
            if (i == 0) ppu->spr0_next = true;
        }
        ppu->n_spr++;
    }

    if (ppu->n_spr > 8) {
        ppu->ppustatus |= PPUSTAT_SPR_OVF;
        ppu->n_spr = 8;
    }
}

static void load_sprite_shifters(Rp2c02 *ppu) {
    int sl = ppu->scanline;

    for (int i = 0; i < (int)ppu->n_spr; i++) {
        uint8_t spr_y  = ppu->oam2[i * 4 + 0];
        uint8_t tile   = ppu->oam2[i * 4 + 1];
        uint8_t attr   = ppu->oam2[i * 4 + 2];
        uint8_t spr_x  = ppu->oam2[i * 4 + 3];
        bool    flip_v = (attr >> 7) & 1;
        bool    flip_h = (attr >> 6) & 1;
        int     row    = sl + 1 - (int)spr_y - 1;  /* row within sprite */

        uint16_t pt_addr;
        if (ppu->ppuctrl & PPUCTRL_SPR_8x16) {
            uint16_t base = (tile & 1) ? 0x1000u : 0x0000u;
            uint8_t  t    = tile & 0xFEu;
            if (row >= 8) { row -= 8; t++; }
            if (flip_v) row ^= 7;
            pt_addr = base | ((uint16_t)t << 4) | (uint8_t)row;
        } else {
            uint16_t base = (ppu->ppuctrl & PPUCTRL_SPR_PT) ? 0x1000u : 0x0000u;
            if (flip_v) row ^= 7;
            pt_addr = base | ((uint16_t)tile << 4) | (uint8_t)row;
        }

        uint8_t lo = ppu->chr_read ? ppu->chr_read(pt_addr,     ppu->chr_ud) : 0;
        uint8_t hi = ppu->chr_read ? ppu->chr_read(pt_addr + 8, ppu->chr_ud) : 0;
        if (flip_h) { lo = bit_reverse(lo); hi = bit_reverse(hi); }

        ppu->spr_shift_lo[i] = lo;
        ppu->spr_shift_hi[i] = hi;
        ppu->spr_attr[i]     = attr;
        ppu->spr_x[i]        = spr_x;
    }
    /* Clear unused slots */
    for (int i = (int)ppu->n_spr; i < 8; i++) {
        ppu->spr_shift_lo[i] = 0;
        ppu->spr_shift_hi[i] = 0;
        ppu->spr_x[i] = 0xFF;
    }
    ppu->spr0_active = ppu->spr0_next;
}

/* ── Pixel output ────────────────────────────────────────────────────────── */

static uint8_t output_pixel(Rp2c02 *ppu, int x) {
    uint8_t bg_px = 0, bg_pal = 0;
    if ((ppu->ppumask & PPUMASK_SHOW_BG) &&
        (x >= 8 || (ppu->ppumask & PPUMASK_BG_LEFT))) {
        uint16_t bit = 0x8000u >> ppu->x;
        bg_px  = (uint8_t)(((ppu->bg_shift_lo & bit) ? 1 : 0) |
                            ((ppu->bg_shift_hi & bit) ? 2 : 0));
        bg_pal = (uint8_t)(((ppu->bg_attr_lo  & bit) ? 1 : 0) |
                            ((ppu->bg_attr_hi  & bit) ? 2 : 0));
    }

    uint8_t spr_px = 0, spr_pal = 0, spr_front = 0;
    bool    spr0_candidate = false;
    if ((ppu->ppumask & PPUMASK_SHOW_SPR) &&
        (x >= 8 || (ppu->ppumask & PPUMASK_SPR_LEFT))) {
        for (int i = 0; i < (int)ppu->n_spr; i++) {
            if (ppu->spr_x[i] != 0) continue;
            uint8_t lo = (ppu->spr_shift_lo[i] >> 7) & 1;
            uint8_t hi = (ppu->spr_shift_hi[i] >> 7) & 1;
            uint8_t px = (uint8_t)(lo | (hi << 1));
            if (px == 0) continue;
            if (spr_px == 0) {
                spr_px    = px;
                spr_pal   = (uint8_t)((ppu->spr_attr[i] & 3) + 4);
                spr_front = (uint8_t)(~(ppu->spr_attr[i] >> 5) & 1);
                if (i == 0 && ppu->spr0_active) spr0_candidate = true;
            }
        }
    }

    /* Sprite-0 hit: non-transparent BG and non-transparent sprite-0 at x<255 */
    if (spr0_candidate && bg_px && x < 255)
        ppu->ppustatus |= PPUSTAT_SPR0_HIT;

    /* Priority mux */
    uint8_t px, pal;
    if      (!bg_px && !spr_px) { px = 0; pal = 0; }
    else if (!bg_px)             { px = spr_px; pal = spr_pal; }
    else if (!spr_px)            { px = bg_px;  pal = bg_pal;  }
    else if (spr_front)          { px = spr_px; pal = spr_pal; }
    else                         { px = bg_px;  pal = bg_pal;  }

    uint8_t addr = px ? (uint8_t)((pal << 2) | px) : 0;
    return palette_rd(ppu, addr);
}

/* ── Tick ────────────────────────────────────────────────────────────────── */

void rp2c02_tick(Rp2c02 *ppu) {
    if (ppu->ppumask_delay > 0 && --ppu->ppumask_delay == 0)
        ppu->ppumask = ppu->ppumask_pending;
    int  sl         = ppu->scanline;
    int  dot        = ppu->dot;
    bool rendering  = (ppu->ppumask & (PPUMASK_SHOW_BG | PPUMASK_SHOW_SPR)) != 0;
    bool visible_sl = (sl >= 0 && sl < 240);
    bool prerender  = (sl == 261);
    bool active_sl  = visible_sl || prerender;

    /* ── VBlank flag & NMI ───────────────────────────────────────────── */
    if (sl == 241 && dot == 1) {
        ppu->ppustatus |= PPUSTAT_VBLANK;
        if (ppu->ppuctrl & PPUCTRL_NMI_EN)
            ppu->nmi_pending = true;
    }
    if (prerender && dot == 1) {
        ppu->ppustatus &= ~(PPUSTAT_VBLANK | PPUSTAT_SPR0_HIT | PPUSTAT_SPR_OVF);
        ppu->nmi_pending = false;
    }

    /* ── Background pipeline ─────────────────────────────────────────── */
    if (rendering && active_sl) {
        bool fetch_range = (dot >= 1 && dot <= 256) || (dot >= 321 && dot <= 336);
        if (fetch_range) {
            bg_shift_tick(ppu);
            do_bg_fetches(ppu, dot);
        }
        /* Sprite shift counters tick during visible pixels */
        if (dot >= 1 && dot <= 256 && visible_sl) {
            for (int i = 0; i < 8; i++) {
                if (ppu->spr_x[i] > 0) {
                    ppu->spr_x[i]--;
                } else {
                    ppu->spr_shift_lo[i] <<= 1;
                    ppu->spr_shift_hi[i] <<= 1;
                }
            }
        }
        if (dot == 256) incr_y(ppu);
        if (dot == 257) {
            copy_x_from_t(ppu);
            if (visible_sl) {
                evaluate_sprites(ppu);
                load_sprite_shifters(ppu);
            }
        }
        if (prerender && dot >= 280 && dot <= 304)
            copy_y_from_t(ppu);
    }

    /* ── Pixel output ────────────────────────────────────────────────── */
    if (visible_sl && dot >= 1 && dot <= 256) {
        uint8_t color = output_pixel(ppu, dot - 1);
        ppu->pixels[sl * RP2C02_WIDTH + (dot - 1)] = color;
    }

    /* ── Advance timing ──────────────────────────────────────────────── */
    ppu->dot++;
    /* NTSC: pre-render line is 340 dots on odd frames when rendering */
    if (ppu->dot == 341 ||
        (ppu->dot == 340 && prerender && ppu->odd_frame && rendering)) {
        ppu->dot = 0;
        ppu->scanline++;
        if (ppu->scanline == 262) {
            ppu->scanline = 0;
            ppu->odd_frame = !ppu->odd_frame;
            ppu->frame++;
            ppu->dirty = true;
        }
    }
}

/* ── Init / Reset ────────────────────────────────────────────────────────── */

void rp2c02_reset(Rp2c02 *ppu) {
    ppu->ppuctrl        = 0;
    ppu->ppumask        = 0;
    ppu->ppumask_delay  = 0;
    ppu->ppustatus &= PPUSTAT_VBLANK;  /* keep VBlank, clear the rest */
    ppu->v = ppu->t = 0;
    ppu->x = 0;
    ppu->w = false;
    ppu->dot = 0;
    ppu->scanline = 0;
    ppu->odd_frame = false;
    ppu->nmi_pending = false;
    ppu->read_buf = 0;
    ppu->open_bus = 0;
    memset(ppu->oam2, 0xFF, sizeof(ppu->oam2));
    ppu->n_spr = 0;
}

void rp2c02_init(Rp2c02 *ppu) {
    memset(ppu, 0, sizeof(*ppu));
    memset(ppu->oam2, 0xFF, sizeof(ppu->oam2));
    ppu->mirror = RP2C02_MIRROR_VERTICAL;
    /* Power-on state: scrolling registers indeterminate; simulate safe defaults */
}
