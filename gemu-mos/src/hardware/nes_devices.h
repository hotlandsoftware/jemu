#pragma once
#include "mos6502cfg.h"

typedef struct {
    const char    *name;
    const char    *desc;
    NesDeviceType  type;
} NesDeviceDesc;

const NesDeviceDesc *nes_device_list(int *count);
void                 nes_device_list_print(void);
const NesDeviceDesc *nes_device_find(const char *name);
