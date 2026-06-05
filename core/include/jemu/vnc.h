#pragma once
#include <stdint.h>
#include <stdbool.h>

typedef struct JemuVncServer JemuVncServer;

/* addr: "host:display" or ":display"  (port = 5900 + display)
 * fb_w/fb_h: framebuffer dimensions in pixels                */
JemuVncServer *jemu_vnc_create(const char *addr, int fb_w, int fb_h);
void           jemu_vnc_destroy(JemuVncServer *vnc);

/* Called whenever the guest's display changes.
 * vram: 1 byte/pixel (0=black, non-zero=white), vw*vh bytes  */
void           jemu_vnc_update(JemuVncServer *vnc,
                               const uint8_t *vram, int vw, int vh);

/* Copy the current VNC key state into keys[16] (CHIP-8 layout).
 * Safe to call from any thread.                               */
void           jemu_vnc_get_keys(JemuVncServer *vnc, uint8_t keys[16]);
