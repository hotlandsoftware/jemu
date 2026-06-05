#include "jemu/memory.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

JemuMemory *jemu_memory_create(size_t size) {
    JemuMemory *m = calloc(1, sizeof(*m));
    if (!m) return NULL;
    m->data = calloc(1, size);
    if (!m->data) { free(m); return NULL; }
    m->size = size;
    return m;
}

void jemu_memory_destroy(JemuMemory *m) {
    if (!m) return;
    free(m->data);
    free(m);
}

void jemu_mem_load(JemuMemory *m, uint32_t addr, const uint8_t *src, size_t len) {
    if (addr + len > m->size) len = m->size - addr;
    memcpy(m->data + addr, src, len);
}

bool jemu_mem_load_file(JemuMemory *m, uint32_t addr, const char *path, size_t *out_len) {
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
