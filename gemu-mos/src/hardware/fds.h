#pragma once
#include <stdint.h>
#include <stdbool.h>

#define FDS_SIDE_BYTES      65500u   /* raw bytes per disk side in .fds file */
#define FDS_RAM_SIZE        0x8000u  /* 32 KB work RAM: CPU $6000–$DFFF       */
#define FDS_BIOS_SIZE       0x2000u  /* 8 KB BIOS ROM:  CPU $E000–$FFFF       */
#define FDS_CHR_SIZE        0x2000u  /* 8 KB CHR RAM:   PPU $0000–$1FFF       */
#define FDS_CYCLES_PER_BYTE 150u     /* CPU cycles between disk byte transfers */

typedef struct {
    uint8_t  bios[FDS_BIOS_SIZE];
    uint8_t  ram[FDS_RAM_SIZE];   /* CPU $6000–$DFFF */

    uint8_t *disk;        /* disk_sides * FDS_SIDE_BYTES bytes, or NULL */
    uint8_t  disk_sides;
    uint8_t  cur_side;
    bool     disk_inserted;

    /* Timer IRQ — $4020/$4021 latch, $4022 control */
    uint16_t timer_latch;
    uint16_t timer_ctr;
    bool     timer_enabled;
    bool     timer_repeat;
    bool     timer_pending;

    /* $4023 master I/O enable (bit 0) */
    bool     disk_io_en;

    /* $4025 drive control register (stored verbatim) */
    uint8_t  drive_ctrl;

    /* Disk transfer state */
    uint32_t disk_pos;       /* current byte offset within the current side */
    uint32_t cyc_acc;        /* cycles accumulated toward next byte */
    uint8_t  read_data;      /* last byte received from disk ($4031) */
    bool     transfer_flag;  /* byte ready; cleared by $4031 or $4030 read */
    bool     end_of_head;    /* disk_pos reached end of side */
} FdsState;

bool    fds_bios_load(FdsState *f, const char *path);
bool    fds_disk_load(FdsState *f, const char *path);
void    fds_disk_eject(FdsState *f);

/* Advance one CPU cycle.  Returns true while any IRQ condition is asserted. */
bool    fds_tick(FdsState *f);

uint8_t fds_reg_read (FdsState *f, uint16_t addr);
void    fds_reg_write(FdsState *f, uint16_t addr, uint8_t val);
