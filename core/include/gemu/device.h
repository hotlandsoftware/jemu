#pragma once
#include <stdint.h>

typedef struct GemuDevice GemuDevice;

typedef struct GemuDeviceOps {
    uint8_t (*read)(GemuDevice *dev, uint32_t reg);
    void    (*write)(GemuDevice *dev, uint32_t reg, uint8_t val);
    void    (*tick)(GemuDevice *dev, uint64_t cycles);  /* periodic update */
    void    (*destroy)(GemuDevice *dev);
} GemuDeviceOps;

struct GemuDevice {
    const GemuDeviceOps *ops;
    const char          *name;
    void                *priv;
};

static inline uint8_t gemu_dev_read(GemuDevice *dev, uint32_t reg) {
    return dev->ops->read(dev, reg);
}

static inline void gemu_dev_write(GemuDevice *dev, uint32_t reg, uint8_t val) {
    dev->ops->write(dev, reg, val);
}

static inline void gemu_dev_tick(GemuDevice *dev, uint64_t cycles) {
    if (dev->ops->tick) dev->ops->tick(dev, cycles);
}
