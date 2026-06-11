#pragma once
#include "cpu/mos6502.h"
#include "mos6502cfg.h"
#include "jemu/vnc.h"
#include "jemu/monitor.h"
#include <stdint.h>
#include <stdbool.h>

/*
 * Generic 6502 machine: flat 64 KB address space.
 *   RAM: 0x0000–0xFFFF (all writable by default)
 *   ROM: overlaid at load time — those ranges become read-only
 *
 * A debug output port at 0xF001 (write) prints a byte to stdout.
 * Reading any address always returns the RAM byte.
 */

typedef struct MosGenericState {
    Mos6502          cpu;
    const MosConfig *cfg;
    uint8_t          mem[0x10000];  /* flat 64 KB */
    uint8_t          rom[0x10000];  /* 1 = read-only at that address */

    JemuVncServer   *vnc;
    JemuMonitor     *monitor;
} MosGenericState;

MosGenericState *mos_generic_create (const MosConfig *cfg);
void             mos_generic_reset  (MosGenericState *s, const MosConfig *cfg);
void             mos_generic_destroy(MosGenericState *s);
void             mos_generic_run    (MosGenericState *s, const MosConfig *cfg);
