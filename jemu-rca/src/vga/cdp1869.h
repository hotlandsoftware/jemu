#pragma once

#include <stdbool.h>
#include <stdint.h>

#define CDP1869_SCREEN_W 360
#define CDP1869_SCREEN_H 312
#define CDP1869_VISIBLE_W 240
#define CDP1869_VISIBLE_H 216
#define CDP1869_PAGE_RAM_SIZE 0x800
#define CDP1869_CHAR_RAM_SIZE 0x800

typedef struct Cdp1869 {
    uint8_t  page_ram[CDP1869_PAGE_RAM_SIZE];
    uint8_t  char_ram[CDP1869_CHAR_RAM_SIZE];
    uint8_t  pcb_ram[CDP1869_CHAR_RAM_SIZE];
    uint8_t  bitmap[CDP1869_VISIBLE_W * CDP1869_VISIBLE_H];

    bool     prd;
    bool     non_display;
    bool     dispoff;
    bool     fresvert;
    bool     freshorz;
    bool     cmem;
    bool     dblpage;
    bool     line16;
    bool     line9;
    bool     cfc;
    uint8_t  col;
    uint8_t  bkg;
    uint16_t pma;
    uint16_t hma;
    uint8_t  q;
    uint8_t  pcb;
    bool     dirty;
} Cdp1869;

void    cdp1869_init(Cdp1869 *vis);
void    cdp1869_reset(Cdp1869 *vis);
uint8_t cdp1869_char_read(Cdp1869 *vis, uint16_t offset);
void    cdp1869_char_write(Cdp1869 *vis, uint16_t offset, uint8_t data);
uint8_t cdp1869_page_read(Cdp1869 *vis, uint16_t offset);
void    cdp1869_page_write(Cdp1869 *vis, uint16_t offset, uint8_t data);
void    cdp1869_out(Cdp1869 *vis, uint8_t port, uint16_t ma, uint8_t data);
void    cdp1869_render(Cdp1869 *vis);
