#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

typedef struct GemuMemory {
    uint8_t *data;
    size_t   size;
} GemuMemory;

GemuMemory *gemu_memory_create(size_t size);
void        gemu_memory_destroy(GemuMemory *mem);

static inline uint8_t gemu_mem_read8(const GemuMemory *m, uint32_t addr) {
    return m->data[addr & (m->size - 1)];
}

static inline uint16_t gemu_mem_read16be(const GemuMemory *m, uint32_t addr) {
    addr &= (uint32_t)(m->size - 1);
    return (uint16_t)((m->data[addr] << 8) | m->data[addr + 1]);
}

static inline void gemu_mem_write8(GemuMemory *m, uint32_t addr, uint8_t val) {
    m->data[addr & (m->size - 1)] = val;
}

void gemu_mem_load(GemuMemory *mem, uint32_t addr, const uint8_t *src, size_t len);
bool gemu_mem_load_file(GemuMemory *mem, uint32_t addr, const char *path, size_t *out_len);
