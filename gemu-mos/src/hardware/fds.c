#include "fds.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── BIOS / disk loading ─────────────────────────────────────────────────── */

bool fds_bios_load(FdsState *f, const char *path) {
    FILE *fp = fopen(path, "rb");
    if (!fp) { fprintf(stderr, "fds: cannot open BIOS '%s'\n", path); return false; }
    size_t n = fread(f->bios, 1, FDS_BIOS_SIZE, fp);
    fclose(fp);
    if (n != FDS_BIOS_SIZE) {
        fprintf(stderr, "fds: BIOS '%s' is %zu bytes, expected %u\n",
                path, n, FDS_BIOS_SIZE);
        return false;
    }
    printf("fds: loaded BIOS '%s'\n", path);
    return true;
}

bool fds_disk_load(FdsState *f, const char *path) {
    FILE *fp = fopen(path, "rb");
    if (!fp) { fprintf(stderr, "fds: cannot open disk '%s'\n", path); return false; }

    /* Detect fwNES header: magic "FDS\x1a" */
    uint8_t hdr[16];
    size_t  hdr_n = fread(hdr, 1, 16, fp);
    uint8_t sides;

    if (hdr_n == 16 && hdr[0] == 'F' && hdr[1] == 'D' && hdr[2] == 'S' && hdr[3] == 0x1A) {
        sides = hdr[4] ? hdr[4] : 1;
    } else {
        /* Raw format — no header; infer from file size */
        fseek(fp, 0, SEEK_END);
        long sz = ftell(fp);
        rewind(fp);
        if (sz <= 0 || (sz % (long)FDS_SIDE_BYTES) != 0) {
            fprintf(stderr, "fds: '%s' is not a valid .fds image (size %ld)\n", path, sz);
            fclose(fp); return false;
        }
        sides = (uint8_t)(sz / (long)FDS_SIDE_BYTES);
    }

    uint8_t *data = malloc((size_t)sides * FDS_SIDE_BYTES);
    if (!data) { fclose(fp); return false; }

    /* For raw format rewind is already done; for fwNES we're positioned after header */
    size_t read = fread(data, 1, (size_t)sides * FDS_SIDE_BYTES, fp);
    fclose(fp);
    if (read < (size_t)sides * FDS_SIDE_BYTES) {
        fprintf(stderr, "fds: disk '%s' truncated (%zu/%u bytes)\n",
                path, read, (unsigned)(sides * FDS_SIDE_BYTES));
        free(data); return false;
    }

    free(f->disk);
    f->disk          = data;
    f->disk_sides    = sides;
    f->cur_side      = 0;
    f->disk_pos      = 0;
    f->cyc_acc       = 0;
    f->transfer_flag = false;
    f->end_of_head   = false;
    f->disk_inserted = true;

    printf("fds: loaded '%s' — %u side%s\n", path, sides, sides == 1 ? "" : "s");
    return true;
}

void fds_disk_eject(FdsState *f) {
    free(f->disk);
    f->disk          = NULL;
    f->disk_sides    = 0;
    f->disk_inserted = false;
    f->disk_pos      = 0;
    f->cyc_acc       = 0;
    f->transfer_flag = false;
    f->end_of_head   = false;
    printf("fds: disk ejected\n");
}

/* ── Per-cycle tick ──────────────────────────────────────────────────────── */

bool fds_tick(FdsState *f) {
    /* ---- Timer IRQ ---- */
    if (f->timer_enabled && f->timer_ctr > 0) {
        if (--f->timer_ctr == 0) {
            f->timer_pending = true;
            if (f->timer_repeat)
                f->timer_ctr = f->timer_latch;
            else
                f->timer_enabled = false;
        }
    }

    /* ---- Disk transfer ----
     * Active when: disk inserted, motor on ($4025 bit 0), not in reset
     * ($4025 bit 1 = 0), read mode ($4025 bit 2), not past end of side. */
    bool motor_on  = (f->drive_ctrl & 0x01) != 0;
    bool not_reset = (f->drive_ctrl & 0x02) == 0;
    bool read_mode = (f->drive_ctrl & 0x04) != 0;

    if (f->disk_inserted && motor_on && not_reset && read_mode && !f->end_of_head) {
        if (++f->cyc_acc >= FDS_CYCLES_PER_BYTE) {
            f->cyc_acc = 0;
            if (f->disk_pos < FDS_SIDE_BYTES) {
                f->read_data    = f->disk[f->cur_side * FDS_SIDE_BYTES + f->disk_pos++];
                f->transfer_flag = true;
            } else {
                f->end_of_head = true;
            }
        }
    }

    return f->timer_pending
        || (f->transfer_flag && (f->drive_ctrl & 0x80) != 0);
}

/* ── Register I/O ────────────────────────────────────────────────────────── */

uint8_t fds_reg_read(FdsState *f, uint16_t addr) {
    switch (addr) {
    case 0x4030: {
        /* Bit 7: byte transfer flag; bit 6: end-of-head; bit 3: mirroring; bit 0: timer IRQ.
         * Reading here acknowledges both timer and transfer IRQs. */
        uint8_t v = 0;
        if (f->timer_pending)              v |= 0x01;
        if (f->drive_ctrl & 0x08)          v |= 0x08;  /* mirror state read-back */
        if (f->end_of_head)                v |= 0x40;
        if (f->transfer_flag)              v |= 0x80;
        f->timer_pending  = false;
        f->transfer_flag  = false;
        return v;
    }
    case 0x4031:
        /* Read data; clears transfer flag */
        f->transfer_flag = false;
        return f->read_data;
    case 0x4032: {
        /* Bit 0: no disk (1=no disk); bit 1: motor not ready (1=not ready); bit 2: write protect */
        uint8_t v = 0;
        if (!f->disk_inserted)          v |= 0x01;
        if ((f->drive_ctrl & 0x01) == 0) v |= 0x02;  /* motor off → not ready */
        return v;
    }
    case 0x4033:
        return 0x80;   /* bit 7: battery OK */
    default:
        return 0;
    }
}

void fds_reg_write(FdsState *f, uint16_t addr, uint8_t val) {
    switch (addr) {
    case 0x4020:
        f->timer_latch = (f->timer_latch & 0xFF00u) | val;
        break;
    case 0x4021:
        f->timer_latch = (f->timer_latch & 0x00FFu) | ((uint16_t)val << 8);
        break;
    case 0x4022:
        f->timer_repeat  = (val & 0x01) != 0;
        f->timer_enabled = (val & 0x02) != 0;
        if (f->timer_enabled)
            f->timer_ctr = f->timer_latch;
        else
            f->timer_pending = false;
        break;
    case 0x4023:
        f->disk_io_en = (val & 0x01) != 0;
        break;
    case 0x4024:
        /* Write data register — disk write not yet implemented */
        f->transfer_flag = false;
        break;
    case 0x4025:
        /* Bit 1 = head reset: when set, return disk head to start of side */
        if (val & 0x02) {
            f->disk_pos      = 0;
            f->cyc_acc       = 0;
            f->transfer_flag = false;
            f->end_of_head   = false;
        }
        f->drive_ctrl = val;
        break;
    case 0x4026:
        /* External connector output — ignored */
        break;
    }
}
