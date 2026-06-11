#include "gemu/memory.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

GemuMemory *gemu_memory_create(size_t size) {
    GemuMemory *m = calloc(1, sizeof(*m));
    if (!m) return NULL;
    m->data = calloc(1, size);
    if (!m->data) { free(m); return NULL; }
    m->size = size;
    return m;
}

void gemu_memory_destroy(GemuMemory *m) {
    if (!m) return;
    free(m->data);
    free(m);
}

void gemu_mem_load(GemuMemory *m, uint32_t addr, const uint8_t *src, size_t len) {
    if (addr + len > m->size) len = m->size - addr;
    memcpy(m->data + addr, src, len);
}

bool gemu_mem_load_file(GemuMemory *m, uint32_t addr, const char *path, size_t *out_len) {
    FILE *f = fopen(path, "rb");
    if (!f) return false;

    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    rewind(f);

    if (sz < 0 || (size_t)(addr + sz) > m->size) {
        fclose(f);
        return false;
    }

    size_t read = fread(m->data + addr, 1, (size_t)sz, f);
    fclose(f);

    if (out_len) *out_len = read;
    return read == (size_t)sz;
}
