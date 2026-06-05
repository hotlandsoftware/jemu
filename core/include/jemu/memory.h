#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

typedef struct JemuMemory {
    uint8_t *data;
    size_t   size;
} JemuMemory;

JemuMemory *jemu_memory_create(size_t size);
void        jemu_memory_destroy(JemuMemory *mem);

static inline uint8_t jemu_mem_read8(const JemuMemory *m, uint32_t addr) {
    return m->data[addr & (m->size - 1)];
}

static inline uint16_t jemu_mem_read16be(const JemuMemory *m, uint32_t addr) {
    addr &= (uint32_t)(m->size - 1);
    return (uint16_t)((m->data[addr] << 8) | m->data[addr + 1]);
}

static inline void jemu_mem_write8(JemuMemory *m, uint32_t addr, uint8_t val) {
    m->data[addr & (m->size - 1)] = val;
}

void jemu_mem_load(JemuMemory *mem, uint32_t addr, const uint8_t *src, size_t len);
bool jemu_mem_load_file(JemuMemory *mem, uint32_t addr, const char *path, size_t *out_len);
