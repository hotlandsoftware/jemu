#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define GEMU_SHA256_DIGEST_LEN 32

typedef struct {
    uint32_t state[8];
    uint64_t count;
    uint8_t  buf[64];
} GemuSha256Ctx;

void gemu_sha256_init(GemuSha256Ctx *ctx);
void gemu_sha256_update(GemuSha256Ctx *ctx, const uint8_t *data, size_t len);
void gemu_sha256_final(GemuSha256Ctx *ctx, uint8_t digest[GEMU_SHA256_DIGEST_LEN]);

/* Convenience: hash an entire file. Returns false on read error. */
bool gemu_sha256_file(const char *path, uint8_t digest[GEMU_SHA256_DIGEST_LEN]);

/* Convert raw digest to 64-char lowercase hex string (null-terminated at hex[64]). */
void gemu_sha256_hex(const uint8_t digest[GEMU_SHA256_DIGEST_LEN], char hex[65]);
