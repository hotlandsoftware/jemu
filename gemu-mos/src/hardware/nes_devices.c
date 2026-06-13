#include "nes_devices.h"
#include <stdio.h>
#include <string.h>

static const NesDeviceDesc devices[] = {
    {"nes-controller", "NES Standard Controller", NES_DEVICE_CONTROLLER},
    {"zapper",         "NES Zapper (light gun)",  NES_DEVICE_ZAPPER},
};

const NesDeviceDesc *nes_device_list(int *count) {
    if (count) *count = (int)(sizeof(devices) / sizeof(devices[0]));
    return devices;
}

void nes_device_list_print(void) {
    int n = 0;
    const NesDeviceDesc *devs = nes_device_list(&n);
    int maxw = 0;
    for (int i = 0; i < n; i++) {
        int w = (int)strlen(devs[i].name);
        if (w > maxw) maxw = w;
    }
    printf("Available devices:\n");
    for (int i = 0; i < n; i++)
        printf("  %-*s  %s\n", maxw, devs[i].name, devs[i].desc);
}

const NesDeviceDesc *nes_device_find(const char *name) {
    int n = 0;
    const NesDeviceDesc *devs = nes_device_list(&n);
    for (int i = 0; i < n; i++)
        if (strcmp(name, devs[i].name) == 0)
            return &devs[i];
    return NULL;
}
