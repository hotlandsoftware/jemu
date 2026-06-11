#include "generic.h"
#include "jemu/memory.h"
#include <SDL2/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── Memory callbacks ────────────────────────────────────────────────────── */

static uint8_t generic_mem_read(uint16_t addr, void *ud) {
    MosGenericState *s = ud;
    return s->mem[addr];
}

static void generic_mem_write(uint16_t addr, uint8_t val, void *ud) {
    MosGenericState *s = ud;

    /* Debug output port: write a byte to stdout (useful for test ROMs) */
    if (addr == 0xF001u) {
        putchar(val);
        fflush(stdout);
        return;
    }

    if (!s->rom[addr])
        s->mem[addr] = val;
}

/* ── ROM loading ─────────────────────────────────────────────────────────── */

static bool load_roms(MosGenericState *s, const MosConfig *cfg) {
    for (int i = 0; i < cfg->n_roms; i++) {
        uint32_t off = cfg->roms[i].addr & 0xFFFFu;
        JemuMemory tmp = {.data = s->mem + off, .size = 0x10000u - off};
        size_t len = 0;
        if (!jemu_mem_load_file(&tmp, 0, cfg->roms[i].path, &len)) {
            fprintf(stderr, "jemu-6502: failed to load '%s'\n", cfg->roms[i].path);
            return false;
        }
        memset(s->rom + off, 1, len);
        printf("jemu-6502: %zu bytes @ 0x%04X  ← %s\n",
               len, (unsigned)off, cfg->roms[i].path);
    }
    return true;
}

/* ── Machine lifecycle ───────────────────────────────────────────────────── */

MosGenericState *mos_generic_create(const MosConfig *cfg) {
    MosGenericState *s = calloc(1, sizeof(*s));
    if (!s) return NULL;

    s->cfg = cfg;

    mos6502_init(&s->cpu);   /* zeroes the struct — must come before assigning callbacks */

    s->cpu.mem_read       = generic_mem_read;
    s->cpu.mem_write      = generic_mem_write;
    s->cpu.mem_ud         = s;
    s->cpu.decimal_disable = (cfg->cpu == MOS_CPU_2A03);

    s->monitor = jemu_monitor_create();

    if (cfg->vnc_addr) {
        /* Placeholder 320×200 framebuffer; machines with real video will override */
        s->vnc = jemu_vnc_create(cfg->vnc_addr, 320, 200);
        if (!s->vnc)
            fprintf(stderr, "jemu-6502: failed to start VNC server at %s\n",
                    cfg->vnc_addr);
    }

    if (!load_roms(s, cfg)) {
        jemu_vnc_destroy(s->vnc);
        jemu_monitor_destroy(s->monitor);
        free(s);
        return NULL;
    }

    mos6502_reset(&s->cpu);

    if (cfg->has_start_addr)
        s->cpu.PC = cfg->start_addr;

    return s;
}

void mos_generic_reset(MosGenericState *s, const MosConfig *cfg) {
    memset(s->rom, 0, sizeof(s->rom));
    load_roms(s, cfg);
    mos6502_reset(&s->cpu);
    if (cfg->has_start_addr)
        s->cpu.PC = cfg->start_addr;
}

void mos_generic_destroy(MosGenericState *s) {
    jemu_monitor_destroy(s->monitor);
    jemu_vnc_destroy(s->vnc);
    free(s);
}

/* ── Run loop ────────────────────────────────────────────────────────────── */

#define GENERIC_HZ                1000000u
#define GENERIC_FRAME_HZ          60u
#define GENERIC_CYCLES_PER_FRAME  (GENERIC_HZ / GENERIC_FRAME_HZ)
#define GENERIC_FRAME_MS          (1000u / GENERIC_FRAME_HZ)

void mos_generic_run(MosGenericState *s, const MosConfig *cfg) {
    jemu_monitor_start(s->monitor);

    bool quit = false;
    while (!quit) {
        Uint32 t0 = SDL_GetTicks();

        /* SDL quit (headless still needs the event pump for clean exit) */
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_QUIT) quit = true;
        }

        /* Monitor commands */
        JemuMonCmd cmd;
        while ((cmd = jemu_monitor_poll(s->monitor)) != JEMU_MON_NONE) {
            if      (cmd == JEMU_MON_QUIT)  { quit = true; break; }
            else if (cmd == JEMU_MON_RESET) mos_generic_reset(s, cfg);
            else if (cmd == JEMU_MON_CUSTOM) jemu_monitor_unknown_command(s->monitor);
        }
        if (quit) break;

        /* VNC key events (machines that add keyboard hardware poll s->vnc here) */
        if (s->vnc) {
            JemuVncKeyEvent ev_key;
            while (jemu_vnc_pop_key_event(s->vnc, &ev_key)) { /* discard for now */ }
        }

        if (!jemu_monitor_is_paused(s->monitor)) {
            uint64_t target = s->cpu.cycle_count + GENERIC_CYCLES_PER_FRAME;
            while (s->cpu.cycle_count < target)
                mos6502_step(&s->cpu);
        }

        Uint32 elapsed = SDL_GetTicks() - t0;
        if (elapsed < GENERIC_FRAME_MS)
            SDL_Delay(GENERIC_FRAME_MS - elapsed);
    }

    printf("jemu-6502: %llu cycles, %llu instructions\n",
           (unsigned long long)s->cpu.cycle_count,
           (unsigned long long)s->cpu.insn_count);

    jemu_monitor_stop(s->monitor);
    (void)cfg;
}
