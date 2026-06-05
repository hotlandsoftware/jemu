#include "cdp1869.h"
#include <string.h>

#define CH_WIDTH 6
#define CDB0 0
#define CDB1 1
#define CDB2 2
#define CDB3 3
#define CDB4 4
#define CDB5 5
#define CCB0 6
#define CCB1 7

void cdp1869_init(Cdp1869 *vis) {
    memset(vis, 0, sizeof(*vis));
    vis->non_display = true;
    vis->dirty = true;
}

void cdp1869_reset(Cdp1869 *vis) {
    memset(vis->page_ram, 0, sizeof(vis->page_ram));
    memset(vis->char_ram, 0, sizeof(vis->char_ram));
    memset(vis->pcb_ram, 0, sizeof(vis->pcb_ram));
    memset(vis->bitmap, 0, sizeof(vis->bitmap));
    vis->prd = false;
    vis->non_display = true;
    vis->dispoff = false;
    vis->fresvert = false;
    vis->freshorz = false;
    vis->cmem = false;
    vis->dblpage = false;
    vis->line16 = false;
    vis->line9 = false;
    vis->cfc = false;
    vis->col = 0;
    vis->bkg = 0;
    vis->pma = 0;
    vis->hma = 0;
    vis->q = 0;
    vis->pcb = 0;
    vis->dirty = true;
}

static int get_lines(const Cdp1869 *vis) {
    if (vis->line16 && !vis->dblpage) return 16;
    if (!vis->line9) return 9;
    return 8;
}

static uint16_t get_pmemsize(const Cdp1869 *vis, int cols, int rows) {
    int pmemsize = cols * rows;
    if (vis->dblpage) pmemsize *= 2;
    if (vis->line16) pmemsize *= 2;
    return (uint16_t)pmemsize;
}

static uint16_t get_pma(const Cdp1869 *vis) {
    return vis->dblpage ? vis->pma : (vis->pma & 0x3ffu);
}

static uint16_t char_addr(uint16_t pma, uint8_t cma, uint8_t pmd) {
    uint8_t column = (pma & 0x400u) ? 0xffu : pmd;
    return (uint16_t)(((column << 3) | (cma & 0x07u)) &
                      (CDP1869_CHAR_RAM_SIZE - 1u));
}

static uint8_t page_read_raw(Cdp1869 *vis, uint16_t offset) {
    uint16_t pma = vis->cmem ? get_pma(vis) : offset;
    return vis->page_ram[pma & (CDP1869_PAGE_RAM_SIZE - 1u)];
}

uint8_t cdp1869_char_read(Cdp1869 *vis, uint16_t offset) {
    if (!vis->non_display)
        return 0xff;
    uint8_t cma = (uint8_t)(offset & 0x0f);
    uint16_t pma = vis->cmem ? get_pma(vis) : offset;
    if (vis->dblpage) cma &= 0x07u;
    uint8_t pmd = page_read_raw(vis, pma);
    uint16_t addr = char_addr(pma, cma, pmd);
    vis->pcb = vis->pcb_ram[addr] & 1u;
    return vis->char_ram[addr];
}

void cdp1869_char_write(Cdp1869 *vis, uint16_t offset, uint8_t data) {
    if (!vis->non_display)
        return;
    uint8_t cma = (uint8_t)(offset & 0x0f);
    uint16_t pma = vis->cmem ? get_pma(vis) : offset;
    if (vis->dblpage) cma &= 0x07u;
    uint8_t pmd = page_read_raw(vis, pma);
    uint16_t addr = char_addr(pma, cma, pmd);
    vis->char_ram[addr] = data;
    vis->pcb_ram[addr] = vis->q & 1u;
    vis->dirty = true;
}

uint8_t cdp1869_page_read(Cdp1869 *vis, uint16_t offset) {
    if (!vis->non_display)
        return 0xff;
    return page_read_raw(vis, offset);
}

void cdp1869_page_write(Cdp1869 *vis, uint16_t offset, uint8_t data) {
    if (!vis->non_display)
        return;
    uint16_t pma = vis->cmem ? get_pma(vis) : offset;
    vis->page_ram[pma & (CDP1869_PAGE_RAM_SIZE - 1u)] = data;
    vis->dirty = true;
}

void cdp1869_out(Cdp1869 *vis, uint8_t port, uint16_t ma, uint8_t data) {
    switch (port) {
    case 3:
        vis->bkg = data & 0x07u;
        vis->cfc = (data & 0x08u) != 0;
        vis->dispoff = (data & 0x10u) != 0;
        vis->col = (data >> 5) & 0x03u;
        vis->freshorz = (data & 0x80u) != 0;
        break;
    case 4:
        break; /* sound registers only for now */
    case 5:
        vis->cmem = (ma & 0x0001u) != 0;
        vis->line9 = (ma & 0x0008u) != 0;
        vis->line16 = (ma & 0x0020u) != 0;
        vis->dblpage = (ma & 0x0040u) != 0;
        vis->fresvert = (ma & 0x0080u) != 0;
        if (vis->cmem) vis->pma = ma;
        else vis->pma = 0;
        break;
    case 6:
        vis->pma = ma & 0x07ffu;
        break;
    case 7:
        vis->hma = ma & 0x07fcu;
        break;
    default:
        break;
    }
    vis->dirty = true;
}

static uint8_t get_pen(const Cdp1869 *vis, int ccb0, int ccb1, int pcb) {
    int r = 0, g = 0, b = 0;
    switch (vis->col) {
    case 0: r = ccb0; b = ccb1; g = pcb; break;
    case 1: r = ccb0; b = pcb;  g = ccb1; break;
    default: r = pcb; b = ccb0; g = ccb1; break;
    }
    uint8_t color = (uint8_t)((r << 2) | (b << 1) | g);
    return color ? color : 0;
}

static void plot(Cdp1869 *vis, int x, int y, uint8_t color) {
    if (x < 0 || x >= CDP1869_VISIBLE_W || y < 0 || y >= CDP1869_VISIBLE_H)
        return;
    vis->bitmap[y * CDP1869_VISIBLE_W + x] = color;
}

static void draw_line(Cdp1869 *vis, int x, int y, uint8_t data, uint8_t color) {
    data <<= 2;
    for (int i = 0; i < CH_WIDTH; i++) {
        if (data & 0x80u) {
            plot(vis, x, y, color);
            if (!vis->fresvert) plot(vis, x, y + 1, color);
            if (!vis->freshorz) {
                plot(vis, x + 1, y, color);
                if (!vis->fresvert) plot(vis, x + 1, y + 1, color);
            }
        }
        if (!vis->freshorz) x++;
        x++;
        data <<= 1;
    }
}

static void draw_char(Cdp1869 *vis, int x, int y, uint16_t pma) {
    uint8_t pmd = page_read_raw(vis, pma);
    for (uint8_t cma = 0; cma < get_lines(vis); cma++) {
        uint16_t addr = char_addr(pma, cma, pmd);
        uint8_t data = vis->char_ram[addr];
        int ccb0 = (data >> CCB0) & 1u;
        int ccb1 = (data >> CCB1) & 1u;
        int pcb = vis->pcb_ram[addr] & 1u;
        draw_line(vis, x, y, data, get_pen(vis, ccb0, ccb1, pcb));
        y++;
        if (!vis->fresvert) y++;
    }
}

void cdp1869_render(Cdp1869 *vis) {
    memset(vis->bitmap, vis->bkg & 0x07u, sizeof(vis->bitmap));
    if (!vis->dispoff) {
        int width = vis->freshorz ? CH_WIDTH : CH_WIDTH * 2;
        int height = get_lines(vis);
        if (!vis->fresvert) height *= 2;
        int cols = vis->freshorz ? 40 : 20;
        int rows = CDP1869_VISIBLE_H / height;
        uint16_t pmemsize = get_pmemsize(vis, cols, rows);
        uint16_t addr = vis->hma;
        for (int sy = 0; sy < rows; sy++) {
            for (int sx = 0; sx < cols; sx++) {
                draw_char(vis, sx * width, sy * height, addr);
                addr++;
                if (addr == pmemsize) addr = 0;
            }
        }
    }
    vis->dirty = false;
}
