#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "jemu/display.h"

typedef enum {
    COSMAC_MACHINE_GENERIC,
} CosmacMachineType;

typedef struct CosmacConfig {
    const char       *rom_path;
    CosmacMachineType machine;
    JemuDisplayType   display_type;
} CosmacConfig;
