#include "jemu/vnc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

struct JemuVncServer {
    int             listen_fd;
    pthread_t       thread;
    pthread_mutex_t lock;
    uint8_t        *fb;          /* vram snapshot (1 byte/pixel) */
    int             vram_w, vram_h;
    int             fb_w, fb_h;  /* scaled output dimensions */
    bool            running;
    uint8_t         keys[16];    /* CHIP-8 key state from VNC clients */
    uint32_t        queued_keysym;
};

/* X11 keysyms matching the SDL/GTK CHIP-8 keypad layout:
 *   Keypad  1 2 3 C  →  1 2 3 4
 *           4 5 6 D  →  Q W E R
 *           7 8 9 E  →  A S D F
 *           A 0 B F  →  Z X C V     */
static const uint32_t chip8_keysyms[16] = {
    0x78, /* 0 → x */  0x31, /* 1 → 1 */  0x32, /* 2 → 2 */  0x33, /* 3 → 3 */
    0x71, /* 4 → q */  0x77, /* 5 → w */  0x65, /* 6 → e */  0x61, /* 7 → a */
    0x73, /* 8 → s */  0x64, /* 9 → d */  0x7a, /* A → z */  0x63, /* B → c */
    0x34, /* C → 4 */  0x72, /* D → r */  0x66, /* E → f */  0x76, /* F → v */
};

/* ── I/O helpers ─────────────────────────────────────────────────────────── */

static bool recvall(int fd, void *buf, size_t n) {
    uint8_t *p = buf;
    while (n) { ssize_t r = recv(fd, p, n, 0); if (r <= 0) return false; p += r; n -= (size_t)r; }
    return true;
}
static bool sendall(int fd, const void *buf, size_t n) {
    const uint8_t *p = buf;
    while (n) { ssize_t r = send(fd, p, n, MSG_NOSIGNAL); if (r <= 0) return false; p += r; n -= (size_t)r; }
    return true;
}

/* ── Per-connection pixel format ─────────────────────────────────────────── */

typedef struct { int bytes_pp; bool big_endian; uint32_t white; } PixFmt;

static PixFmt default_fmt(void) {
    return (PixFmt){ 4, false, 0x00FFFFFF }; /* 32bpp LE, R@16 G@8 B@0 */
}

static PixFmt parse_pixfmt(const uint8_t *b) {
    /* b[0..15] = PixelFormat (bits-per-pixel at b[0], big-endian at b[2], ...) */
    uint16_t r_max, g_max, b_max;
    memcpy(&r_max, b + 4, 2); r_max = ntohs(r_max);
    memcpy(&g_max, b + 6, 2); g_max = ntohs(g_max);
    memcpy(&b_max, b + 8, 2); b_max = ntohs(b_max);
    PixFmt f;
    f.bytes_pp   = b[0] / 8; if (f.bytes_pp < 1) f.bytes_pp = 4;
    f.big_endian = (b[2] != 0);
    f.white = ((uint32_t)r_max << b[10]) |
              ((uint32_t)g_max << b[11]) |
              ((uint32_t)b_max << b[12]);
    return f;
}

static void write_pixel(uint8_t *dst, uint32_t val, int bpp, bool be) {
    if (be) { for (int i = bpp-1; i >= 0; i--) { dst[i] = val & 0xFF; val >>= 8; } }
    else    { for (int i = 0; i < bpp;  i++)   { dst[i] = val & 0xFF; val >>= 8; } }
}

/* ── RFB handshake ───────────────────────────────────────────────────────── */

static bool vnc_handshake(int fd) {
    if (!sendall(fd, "RFB 003.008\n", 12)) return false;
    char ver[13]; ver[12] = '\0';
    if (!recvall(fd, ver, 12)) return false;
    int major = 0, minor = 0;
    sscanf(ver, "RFB %d.%03d", &major, &minor);
    if (major == 3 && minor == 3) {
        uint32_t sec = htonl(1);
        if (!sendall(fd, &sec, 4)) return false;
    } else {
        uint8_t offer[2] = {1, 1};
        if (!sendall(fd, offer, 2)) return false;
        uint8_t chosen; if (!recvall(fd, &chosen, 1)) return false;
        if (minor >= 8) { uint32_t ok = 0; if (!sendall(fd, &ok, 4)) return false; }
    }
    uint8_t shared; return recvall(fd, &shared, 1);
}

static bool vnc_send_server_init(JemuVncServer *vnc, int fd) {
    uint8_t msg[24];
    uint16_t w = htons((uint16_t)vnc->fb_w), h = htons((uint16_t)vnc->fb_h);
    memcpy(msg, &w, 2); memcpy(msg+2, &h, 2);
    msg[4]=32; msg[5]=24; msg[6]=0; msg[7]=1;
    uint16_t mx = htons(255);
    memcpy(msg+8, &mx, 2); memcpy(msg+10, &mx, 2); memcpy(msg+12, &mx, 2);
    msg[14]=16; msg[15]=8; msg[16]=0; msg[17]=msg[18]=msg[19]=0;
    uint32_t nl = htonl(4); memcpy(msg+20, &nl, 4);
    return sendall(fd, msg, 24) && sendall(fd, "JEMU", 4);
}

/* ── FramebufferUpdate ───────────────────────────────────────────────────── */

static bool vnc_send_update(JemuVncServer *vnc, int fd, const PixFmt *fmt) {
    int npix = vnc->fb_w * vnc->fb_h;

    pthread_mutex_lock(&vnc->lock);
    int vw = vnc->vram_w, vh = vnc->vram_h;
    uint8_t *vsnap = (vw > 0 && vh > 0) ? malloc((size_t)(vw * vh)) : NULL;
    if (vsnap) memcpy(vsnap, vnc->fb, (size_t)(vw * vh));
    pthread_mutex_unlock(&vnc->lock);

    size_t out_sz = (size_t)(npix * fmt->bytes_pp);
    uint8_t *out  = calloc(out_sz, 1); /* black by default */
    if (!out) { free(vsnap); return false; }

    if (vsnap) {
        for (int y = 0; y < vnc->fb_h; y++) {
            for (int x = 0; x < vnc->fb_w; x++) {
                int sx = x * vw / vnc->fb_w, sy = y * vh / vnc->fb_h;
                uint32_t px = vsnap[sy * vw + sx] ? fmt->white : 0;
                write_pixel(out + (y * vnc->fb_w + x) * fmt->bytes_pp,
                            px, fmt->bytes_pp, fmt->big_endian);
            }
        }
        free(vsnap);
    }

    uint8_t hdr[4]  = {0, 0, 0, 1};
    uint8_t rect[12]; memset(rect, 0, 4);
    uint16_t rw = htons((uint16_t)vnc->fb_w), rh = htons((uint16_t)vnc->fb_h);
    memcpy(rect+4, &rw, 2); memcpy(rect+6, &rh, 2); memset(rect+8, 0, 4);

    bool ok = sendall(fd, hdr, 4) && sendall(fd, rect, 12) && sendall(fd, out, out_sz);
    free(out);
    return ok;
}

/* ── Client message loop ─────────────────────────────────────────────────── */

static void vnc_handle_client(JemuVncServer *vnc, int fd) {
    if (!vnc_handshake(fd) || !vnc_send_server_init(vnc, fd)) return;
    PixFmt fmt = default_fmt();
    while (vnc->running) {
        uint8_t type; if (!recvall(fd, &type, 1)) break;
        switch (type) {
        case 0: { /* SetPixelFormat */
            uint8_t b[19]; if (!recvall(fd, b, 19)) return;
            fmt = parse_pixfmt(b + 3); break; /* b[0..2]=pad, b[3..18]=PixelFormat */
        }
        case 2: { /* SetEncodings */
            uint8_t b[3]; if (!recvall(fd, b, 3)) return;
            uint16_t cnt; memcpy(&cnt, b+1, 2); cnt = ntohs(cnt);
            for (uint16_t i = 0; i < cnt; i++) { uint8_t e[4]; if (!recvall(fd, e, 4)) return; }
            break;
        }
        case 3: { /* FramebufferUpdateRequest */
            uint8_t b[9]; if (!recvall(fd, b, 9)) return;
            if (!vnc_send_update(vnc, fd, &fmt)) return;
            break;
        }
        case 4: { /* KeyEvent: 1 down-flag + 2 pad + 4 keysym */
            uint8_t b[7]; if (!recvall(fd, b, 7)) return;
            uint8_t down = b[0];
            uint32_t sym; memcpy(&sym, b+3, 4); sym = ntohl(sym);
            uint32_t raw_sym = sym;
            if (sym >= 0x41 && sym <= 0x5A) sym |= 0x20; /* normalize to lowercase */
            pthread_mutex_lock(&vnc->lock);
            if (down) vnc->queued_keysym = raw_sym;
            for (int k = 0; k < 16; k++)
                if (chip8_keysyms[k] == sym) { vnc->keys[k] = down; break; }
            pthread_mutex_unlock(&vnc->lock);
            break;
        }
        case 5: { uint8_t b[5]; if (!recvall(fd, b, 5)) return; break; } /* PointerEvent */
        case 6: { /* ClientCutText */
            uint8_t b[7]; if (!recvall(fd, b, 7)) return;
            uint32_t len; memcpy(&len, b+3, 4); len = ntohl(len);
            while (len) { uint8_t t[256]; size_t c = len<256?len:256; if (!recvall(fd,t,c)) return; len-=(uint32_t)c; }
            break;
        }
        case 150: { /* EnableContinuousUpdates (RealVNC/Tight) */
            uint8_t b[9]; if (!recvall(fd, b, 9)) return; break;
        }
        case 248: { /* ClientFence */
            uint8_t b[8]; if (!recvall(fd, b, 8)) return; /* 3 pad + 4 flags + 1 len */
            uint8_t plen = b[7];
            while (plen--) { uint8_t t; if (!recvall(fd, &t, 1)) return; }
            break;
        }
        default: return;
        }
    }
}

static void *vnc_thread(void *arg) {
    JemuVncServer *vnc = arg;
    while (vnc->running) {
        fd_set fds; FD_ZERO(&fds); FD_SET(vnc->listen_fd, &fds);
        struct timeval tv = {1, 0};
        if (select(vnc->listen_fd + 1, &fds, NULL, NULL, &tv) <= 0) continue;
        int cfd = accept(vnc->listen_fd, NULL, NULL);
        if (cfd < 0) continue;
        printf("vnc: client connected\n");
        vnc_handle_client(vnc, cfd);
        printf("vnc: client disconnected\n");
        close(cfd);
    }
    return NULL;
}

/* ── Address parsing ─────────────────────────────────────────────────────── */

static bool parse_vnc_addr(const char *s, char host[64], int *port) {
    const char *colon = strrchr(s, ':');
    if (!colon) return false;
    *port = 5900 + atoi(colon + 1);
    size_t hlen = (size_t)(colon - s);
    if      (hlen == 0)   { memcpy(host, "0.0.0.0", 8); }
    else if (hlen < 64)   { memcpy(host, s, hlen); host[hlen] = '\0'; }
    else                  { return false; }
    return true;
}

/* ── Public API ──────────────────────────────────────────────────────────── */

JemuVncServer *jemu_vnc_create(const char *addr, int fb_w, int fb_h) {
    char host[64]; int port;
    if (!parse_vnc_addr(addr, host, &port)) {
        fprintf(stderr, "vnc: bad address '%s' — use host:N or :N\n", addr); return NULL;
    }
    int lfd = socket(AF_INET, SOCK_STREAM, 0);
    if (lfd < 0) { perror("vnc: socket"); return NULL; }
    int opt = 1; setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    struct sockaddr_in sa = {0};
    sa.sin_family = AF_INET; sa.sin_port = htons((uint16_t)port);
    inet_pton(AF_INET, host, &sa.sin_addr);
    if (bind(lfd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        perror("vnc: bind"); close(lfd); return NULL;
    }
    listen(lfd, 1);
    JemuVncServer *vnc = calloc(1, sizeof(*vnc));
    vnc->listen_fd = lfd;
    vnc->fb_w = fb_w; vnc->fb_h = fb_h;
    vnc->running = true;
    pthread_mutex_init(&vnc->lock, NULL);
    pthread_create(&vnc->thread, NULL, vnc_thread, vnc);
    printf("vnc: listening on %s:%d (display :%d)\n", host, port, port - 5900);
    return vnc;
}

void jemu_vnc_destroy(JemuVncServer *vnc) {
    if (!vnc) return;
    vnc->running = false;
    pthread_join(vnc->thread, NULL);
    close(vnc->listen_fd);
    pthread_mutex_destroy(&vnc->lock);
    free(vnc->fb);
    free(vnc);
}

void jemu_vnc_get_keys(JemuVncServer *vnc, uint8_t keys[16]) {
    if (!vnc) { memset(keys, 0, 16); return; }
    pthread_mutex_lock(&vnc->lock);
    memcpy(keys, vnc->keys, 16);
    pthread_mutex_unlock(&vnc->lock);
}

uint32_t jemu_vnc_pop_keysym(JemuVncServer *vnc) {
    if (!vnc) return 0;
    pthread_mutex_lock(&vnc->lock);
    uint32_t sym = vnc->queued_keysym;
    vnc->queued_keysym = 0;
    pthread_mutex_unlock(&vnc->lock);
    return sym;
}

void jemu_vnc_update(JemuVncServer *vnc,
                     const uint8_t *vram, int vw, int vh) {
    if (!vnc) return;
    pthread_mutex_lock(&vnc->lock);
    if (vnc->vram_w != vw || vnc->vram_h != vh) {
        free(vnc->fb);
        vnc->fb = malloc((size_t)(vw * vh));
        vnc->vram_w = vw; vnc->vram_h = vh;
    }
    if (vnc->fb) memcpy(vnc->fb, vram, (size_t)(vw * vh));
    pthread_mutex_unlock(&vnc->lock);
}
