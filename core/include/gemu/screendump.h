#pragma once
#include <stdbool.h>
#include <stdint.h>

/*
 * Screendump helpers.  All functions take a packed 24-bit RGB buffer
 * (3 bytes per pixel, row-major, top-to-bottom).
 *
 * gemu_screendump() dispatches to PPM or PNG based on the file extension:
 *   *.png  → PNG (uncompressed deflate — valid but larger than gzip)
 *   anything else → PPM (P6 binary, trivial to open everywhere)
 */
bool gemu_screendump    (const char *path, const uint8_t *rgb, int w, int h);
bool gemu_screendump_ppm(const char *path, const uint8_t *rgb, int w, int h);
bool gemu_screendump_png(const char *path, const uint8_t *rgb, int w, int h);
