#pragma once
#include <stdint.h>

typedef struct JemuDevice JemuDevice;

typedef struct JemuDeviceOps {
    uint8_t (*read)(JemuDevice *dev, uint32_t reg);
    void    (*write)(JemuDevice *dev, uint32_t reg, uint8_t val);
    void    (*tick)(JemuDevice *dev, uint64_t cycles);  /* periodic update */
    void    (*destroy)(JemuDevice *dev);
} JemuDeviceOps;

struct JemuDevice {
    const JemuDeviceOps *ops;
    const char          *name;
    void                *priv;
};

static inline uint8_t jemu_dev_read(JemuDevice *dev, uint32_t reg) {
    return dev->ops->read(dev, reg);
}

static inline void jemu_dev_write(JemuDevice *dev, uint32_t reg, uint8_t val) {
    dev->ops->write(dev, reg, val);
}

static inline void jemu_dev_tick(JemuDevice *dev, uint64_t cycles) {
    if (dev->ops->tick) dev->ops->tick(dev, cycles);
}
