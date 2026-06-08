#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define JEMU_SHA256_DIGEST_LEN 32

typedef struct {
    uint32_t state[8];
    uint64_t count;
    uint8_t  buf[64];
} JemuSha256Ctx;

void jemu_sha256_init(JemuSha256Ctx *ctx);
void jemu_sha256_update(JemuSha256Ctx *ctx, const uint8_t *data, size_t len);
void jemu_sha256_final(JemuSha256Ctx *ctx, uint8_t digest[JEMU_SHA256_DIGEST_LEN]);

/* Convenience: hash an entire file. Returns false on read error. */
bool jemu_sha256_file(const char *path, uint8_t digest[JEMU_SHA256_DIGEST_LEN]);

/* Convert raw digest to 64-char lowercase hex string (null-terminated at hex[64]). */
void jemu_sha256_hex(const uint8_t digest[JEMU_SHA256_DIGEST_LEN], char hex[65]);
