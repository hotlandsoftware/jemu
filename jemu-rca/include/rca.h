#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "jemu/display.h"

typedef enum {
    RCA_MACHINE_GENERIC,
    RCA_MACHINE_COSMAC_VIP,
} RcaMachineType;

typedef enum {
    RCA_CPU_NONE,
    RCA_CPU_CDP1802,
} RcaCpuType;

typedef enum {
    RCA_VGA_NONE,
    RCA_VGA_CDP1861,
} RcaVgaType;

#define RCA_MAX_ROM_LOADS 8

typedef struct {
    const char *path;
    uint32_t    addr;
} RcaRomLoad;

typedef struct RcaConfig {
    RcaRomLoad      roms[RCA_MAX_ROM_LOADS];
    int             n_roms;
    RcaMachineType  machine;
    RcaCpuType      cpu;
    RcaVgaType      vga;
    JemuDisplayType display_type;
    int             display_scale;
} RcaConfig;
