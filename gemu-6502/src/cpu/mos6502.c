#include "mos6502.h"
#include <stdio.h>
#include <string.h>

/* ── Memory access helpers ───────────────────────────────────────────────── */

static inline uint8_t rd(Mos6502 *c, uint16_t a) {
    c->cycle_count++;
    return c->mem_read(a, c->mem_ud);
}
static inline void wr(Mos6502 *c, uint16_t a, uint8_t v) {
    c->cycle_count++;
    c->mem_write(a, v, c->mem_ud);
}
/* Zero-page 16-bit read (wraps within page — critical for JMP ind bug etc.) */
static inline uint16_t rd16zp(Mos6502 *c, uint8_t a) {
    uint8_t lo = rd(c, (uint16_t)a);
    uint8_t hi = rd(c, (uint16_t)((uint8_t)(a + 1)));
    return (uint16_t)(lo | ((uint16_t)hi << 8));
}
static inline uint8_t fetch(Mos6502 *c) { return rd(c, c->PC++); }

/* ── Stack ───────────────────────────────────────────────────────────────── */

static inline void push(Mos6502 *c, uint8_t v) {
    wr(c, 0x0100u | c->SP, v);
    c->SP--;
}
static inline uint8_t pop(Mos6502 *c) {
    c->SP++;
    return rd(c, 0x0100u | c->SP);
}

/* ── Status flag helpers ─────────────────────────────────────────────────── */

static inline void set_nz(Mos6502 *c, uint8_t v) {
    c->P = (c->P & ~(MOS6502_P_N | MOS6502_P_Z))
         | (v & 0x80u   ? MOS6502_P_N : 0u)
         | (v == 0u     ? MOS6502_P_Z : 0u);
}

/* ── Addressing mode helpers ─────────────────────────────────────────────── */

static inline uint16_t am_zp(Mos6502 *c) {
    return (uint16_t)fetch(c);
}
static inline uint16_t am_zpx(Mos6502 *c) {
    uint8_t z = fetch(c);
    rd(c, z);                             /* dummy read of unindexed addr */
    return (uint16_t)((uint8_t)(z + c->X));
}
static inline uint16_t am_zpy(Mos6502 *c) {
    uint8_t z = fetch(c);
    rd(c, z);
    return (uint16_t)((uint8_t)(z + c->Y));
}
static inline uint16_t am_abs(Mos6502 *c) {
    uint8_t lo = fetch(c), hi = fetch(c);
    return (uint16_t)(lo | ((uint16_t)hi << 8));
}
static inline uint16_t am_abx(Mos6502 *c, int *cross) {
    uint8_t lo = fetch(c), hi = fetch(c);
    uint16_t base = (uint16_t)(lo | ((uint16_t)hi << 8));
    uint16_t ea   = base + c->X;
    if (cross) *cross = ((ea ^ base) >> 8) & 1;
    return ea;
}
static inline uint16_t am_aby(Mos6502 *c, int *cross) {
    uint8_t lo = fetch(c), hi = fetch(c);
    uint16_t base = (uint16_t)(lo | ((uint16_t)hi << 8));
    uint16_t ea   = base + c->Y;
    if (cross) *cross = ((ea ^ base) >> 8) & 1;
    return ea;
}
static inline uint16_t am_izx(Mos6502 *c) {
    uint8_t z = fetch(c);
    rd(c, z);                             /* dummy read */
    return rd16zp(c, (uint8_t)(z + c->X));
}
static inline uint16_t am_izy(Mos6502 *c, int *cross) {
    uint8_t  z    = fetch(c);
    uint16_t base = rd16zp(c, z);
    uint16_t ea   = base + c->Y;
    if (cross) *cross = ((ea ^ base) >> 8) & 1;
    return ea;
}

/* ── ALU operations ──────────────────────────────────────────────────────── */

static void do_adc(Mos6502 *c, uint8_t val) {
    uint8_t carry = (c->P & MOS6502_P_C) ? 1u : 0u;
    if ((c->P & MOS6502_P_D) && !c->decimal_disable) {
        /* NMOS 6502 BCD: N/V/Z set from binary intermediate */
        uint16_t bin = (uint16_t)c->A + val + carry;
        uint8_t  v   = (uint8_t)(~(c->A ^ val) & (c->A ^ (uint8_t)bin) & 0x80u);
        set_nz(c, (uint8_t)bin);
        if (v) c->P |= MOS6502_P_V; else c->P &= ~MOS6502_P_V;
        uint8_t al = (c->A & 0xFu) + (val & 0xFu) + carry;
        if (al > 9u) al = (uint8_t)(al + 6u);
        uint8_t ah = (c->A >> 4) + (val >> 4) + (al > 0xFu ? 1u : 0u);
        if (ah > 9u) ah = (uint8_t)(ah + 6u);
        if (ah > 0xFu) c->P |= MOS6502_P_C; else c->P &= ~MOS6502_P_C;
        c->A = (uint8_t)((ah << 4) | (al & 0xFu));
    } else {
        uint16_t r = (uint16_t)c->A + val + carry;
        uint8_t  result = (uint8_t)r;
        uint8_t  v = (uint8_t)(~(c->A ^ val) & (c->A ^ result) & 0x80u);
        set_nz(c, result);
        if (r > 0xFFu) c->P |= MOS6502_P_C; else c->P &= ~MOS6502_P_C;
        if (v)         c->P |= MOS6502_P_V; else c->P &= ~MOS6502_P_V;
        c->A = result;
    }
}

static void do_sbc(Mos6502 *c, uint8_t val) {
    uint8_t carry = (c->P & MOS6502_P_C) ? 1u : 0u;
    if ((c->P & MOS6502_P_D) && !c->decimal_disable) {
        uint16_t bin = (uint16_t)c->A - val - (1u - carry);
        uint8_t  v   = (uint8_t)((c->A ^ val) & (c->A ^ (uint8_t)bin) & 0x80u);
        set_nz(c, (uint8_t)bin);
        if (v) c->P |= MOS6502_P_V; else c->P &= ~MOS6502_P_V;
        int16_t al = (int16_t)(c->A & 0xFu) - (val & 0xFu) - (int16_t)(1u - carry);
        if (al < 0) al -= 6;
        int16_t ah = (int16_t)(c->A >> 4) - (val >> 4) - (al < 0 ? 1 : 0);
        if (ah < 0) ah -= 6;
        if (!(bin & 0x100u)) c->P |= MOS6502_P_C; else c->P &= ~MOS6502_P_C;
        c->A = (uint8_t)(((ah & 0xFu) << 4) | ((uint8_t)al & 0xFu));
    } else {
        do_adc(c, (uint8_t)~val);
    }
}

static void do_cmp(Mos6502 *c, uint8_t reg, uint8_t val) {
    uint16_t r = (uint16_t)reg - val;
    set_nz(c, (uint8_t)r);
    if (!(r & 0x100u)) c->P |= MOS6502_P_C; else c->P &= ~MOS6502_P_C;
}

static void do_bit(Mos6502 *c, uint8_t val) {
    if (val & MOS6502_P_N) c->P |= MOS6502_P_N; else c->P &= ~MOS6502_P_N;
    if (val & MOS6502_P_V) c->P |= MOS6502_P_V; else c->P &= ~MOS6502_P_V;
    if (!(c->A & val))     c->P |= MOS6502_P_Z; else c->P &= ~MOS6502_P_Z;
}

static uint8_t do_asl(Mos6502 *c, uint8_t v) {
    if (v & 0x80u) c->P |= MOS6502_P_C; else c->P &= ~MOS6502_P_C;
    uint8_t r = (uint8_t)(v << 1);
    set_nz(c, r);
    return r;
}
static uint8_t do_lsr(Mos6502 *c, uint8_t v) {
    if (v & 0x01u) c->P |= MOS6502_P_C; else c->P &= ~MOS6502_P_C;
    uint8_t r = v >> 1;
    set_nz(c, r);
    return r;
}
static uint8_t do_rol(Mos6502 *c, uint8_t v) {
    uint8_t old_c = (c->P & MOS6502_P_C) ? 1u : 0u;
    if (v & 0x80u) c->P |= MOS6502_P_C; else c->P &= ~MOS6502_P_C;
    uint8_t r = (uint8_t)((v << 1) | old_c);
    set_nz(c, r);
    return r;
}
static uint8_t do_ror(Mos6502 *c, uint8_t v) {
    uint8_t old_c = (c->P & MOS6502_P_C) ? 0x80u : 0u;
    if (v & 0x01u) c->P |= MOS6502_P_C; else c->P &= ~MOS6502_P_C;
    uint8_t r = (uint8_t)((v >> 1) | old_c);
    set_nz(c, r);
    return r;
}

static void do_branch(Mos6502 *c, bool cond) {
    int8_t  off = (int8_t)fetch(c);   /* always fetch offset (1 cycle) */
    if (!cond) return;
    c->cycle_count++;                  /* +1: branch taken */
    uint16_t old = c->PC;
    c->PC = (uint16_t)(c->PC + off);
    if ((c->PC ^ old) & 0xFF00u)
        c->cycle_count++;              /* +1: page crossed */
}

/* ── Interrupt dispatch ───────────────────────────────────────────────────── */

static void service_irq(Mos6502 *c, uint16_t vector, bool brk) {
    rd(c, c->PC);                           /* dummy fetch */
    push(c, (uint8_t)(c->PC >> 8));
    push(c, (uint8_t)(c->PC & 0xFFu));
    push(c, (c->P & ~MOS6502_P_B) | (brk ? MOS6502_P_B : 0u) | MOS6502_P_U);
    c->P |= MOS6502_P_I;
    uint8_t lo = rd(c, vector);
    uint8_t hi = rd(c, (uint16_t)(vector + 1));
    c->PC = (uint16_t)(lo | ((uint16_t)hi << 8));
}

/* ── Init / Reset ────────────────────────────────────────────────────────── */

void mos6502_init(Mos6502 *cpu) {
    memset(cpu, 0, sizeof(*cpu));
    cpu->P = MOS6502_P_U | MOS6502_P_I;
    cpu->SP = 0xFDu;
}

void mos6502_reset(Mos6502 *cpu) {
    cpu->P  |= MOS6502_P_I;
    cpu->SP  = (uint8_t)(cpu->SP - 3);    /* 3 phantom stack writes */
    cpu->cycle_count += 5;                 /* reset takes 7 cycles; 2 already counted */
    uint8_t lo = rd(cpu, 0xFFFCu);
    uint8_t hi = rd(cpu, 0xFFFDu);
    cpu->PC = (uint16_t)(lo | ((uint16_t)hi << 8));
}

/* ── Main step ───────────────────────────────────────────────────────────── */

void mos6502_step(Mos6502 *c) {
    /* NMI takes priority over IRQ */
    if (c->nmi) {
        c->nmi = false;
        service_irq(c, 0xFFFAu, false);
        c->insn_count++;
        return;
    }
    if (c->irq && !(c->P & MOS6502_P_I)) {
        service_irq(c, 0xFFFEu, false);
        c->insn_count++;
        return;
    }

    uint8_t op = fetch(c);
    c->insn_count++;

    switch (op) {

    /* ── BRK ──────────────────────────────────────────────────────────── */
    case 0x00:  /* BRK */
        fetch(c);                              /* skip the padding byte */
        service_irq(c, 0xFFFEu, true);
        break;

    /* ── ORA ──────────────────────────────────────────────────────────── */
    case 0x09: c->A |= fetch(c);           set_nz(c, c->A); break; /* ORA imm */
    case 0x05: c->A |= rd(c, am_zp(c));   set_nz(c, c->A); break; /* ORA zp  */
    case 0x15: c->A |= rd(c, am_zpx(c));  set_nz(c, c->A); break; /* ORA zpx */
    case 0x0D: c->A |= rd(c, am_abs(c));  set_nz(c, c->A); break; /* ORA abs */
    case 0x1D: { int x; uint16_t ea = am_abx(c, &x); if (x) c->cycle_count++;
                 c->A |= rd(c, ea); set_nz(c, c->A); break; }      /* ORA abx */
    case 0x19: { int x; uint16_t ea = am_aby(c, &x); if (x) c->cycle_count++;
                 c->A |= rd(c, ea); set_nz(c, c->A); break; }      /* ORA aby */
    case 0x01: c->A |= rd(c, am_izx(c));  set_nz(c, c->A); break; /* ORA izx */
    case 0x11: { int x; uint16_t ea = am_izy(c, &x); if (x) c->cycle_count++;
                 c->A |= rd(c, ea); set_nz(c, c->A); break; }      /* ORA izy */

    /* ── AND ──────────────────────────────────────────────────────────── */
    case 0x29: c->A &= fetch(c);           set_nz(c, c->A); break;
    case 0x25: c->A &= rd(c, am_zp(c));   set_nz(c, c->A); break;
    case 0x35: c->A &= rd(c, am_zpx(c));  set_nz(c, c->A); break;
    case 0x2D: c->A &= rd(c, am_abs(c));  set_nz(c, c->A); break;
    case 0x3D: { int x; uint16_t ea = am_abx(c, &x); if (x) c->cycle_count++;
                 c->A &= rd(c, ea); set_nz(c, c->A); break; }
    case 0x39: { int x; uint16_t ea = am_aby(c, &x); if (x) c->cycle_count++;
                 c->A &= rd(c, ea); set_nz(c, c->A); break; }
    case 0x21: c->A &= rd(c, am_izx(c));  set_nz(c, c->A); break;
    case 0x31: { int x; uint16_t ea = am_izy(c, &x); if (x) c->cycle_count++;
                 c->A &= rd(c, ea); set_nz(c, c->A); break; }

    /* ── EOR ──────────────────────────────────────────────────────────── */
    case 0x49: c->A ^= fetch(c);           set_nz(c, c->A); break;
    case 0x45: c->A ^= rd(c, am_zp(c));   set_nz(c, c->A); break;
    case 0x55: c->A ^= rd(c, am_zpx(c));  set_nz(c, c->A); break;
    case 0x4D: c->A ^= rd(c, am_abs(c));  set_nz(c, c->A); break;
    case 0x5D: { int x; uint16_t ea = am_abx(c, &x); if (x) c->cycle_count++;
                 c->A ^= rd(c, ea); set_nz(c, c->A); break; }
    case 0x59: { int x; uint16_t ea = am_aby(c, &x); if (x) c->cycle_count++;
                 c->A ^= rd(c, ea); set_nz(c, c->A); break; }
    case 0x41: c->A ^= rd(c, am_izx(c));  set_nz(c, c->A); break;
    case 0x51: { int x; uint16_t ea = am_izy(c, &x); if (x) c->cycle_count++;
                 c->A ^= rd(c, ea); set_nz(c, c->A); break; }

    /* ── ADC ──────────────────────────────────────────────────────────── */
    case 0x69: do_adc(c, fetch(c));           break;
    case 0x65: do_adc(c, rd(c, am_zp(c)));   break;
    case 0x75: do_adc(c, rd(c, am_zpx(c)));  break;
    case 0x6D: do_adc(c, rd(c, am_abs(c)));  break;
    case 0x7D: { int x; uint16_t ea = am_abx(c, &x); if (x) c->cycle_count++;
                 do_adc(c, rd(c, ea)); break; }
    case 0x79: { int x; uint16_t ea = am_aby(c, &x); if (x) c->cycle_count++;
                 do_adc(c, rd(c, ea)); break; }
    case 0x61: do_adc(c, rd(c, am_izx(c)));  break;
    case 0x71: { int x; uint16_t ea = am_izy(c, &x); if (x) c->cycle_count++;
                 do_adc(c, rd(c, ea)); break; }

    /* ── SBC ──────────────────────────────────────────────────────────── */
    case 0xE9: do_sbc(c, fetch(c));           break;
    case 0xE5: do_sbc(c, rd(c, am_zp(c)));   break;
    case 0xF5: do_sbc(c, rd(c, am_zpx(c)));  break;
    case 0xED: do_sbc(c, rd(c, am_abs(c)));  break;
    case 0xFD: { int x; uint16_t ea = am_abx(c, &x); if (x) c->cycle_count++;
                 do_sbc(c, rd(c, ea)); break; }
    case 0xF9: { int x; uint16_t ea = am_aby(c, &x); if (x) c->cycle_count++;
                 do_sbc(c, rd(c, ea)); break; }
    case 0xE1: do_sbc(c, rd(c, am_izx(c)));  break;
    case 0xF1: { int x; uint16_t ea = am_izy(c, &x); if (x) c->cycle_count++;
                 do_sbc(c, rd(c, ea)); break; }

    /* ── CMP ──────────────────────────────────────────────────────────── */
    case 0xC9: do_cmp(c, c->A, fetch(c));           break;
    case 0xC5: do_cmp(c, c->A, rd(c, am_zp(c)));   break;
    case 0xD5: do_cmp(c, c->A, rd(c, am_zpx(c)));  break;
    case 0xCD: do_cmp(c, c->A, rd(c, am_abs(c)));  break;
    case 0xDD: { int x; uint16_t ea = am_abx(c, &x); if (x) c->cycle_count++;
                 do_cmp(c, c->A, rd(c, ea)); break; }
    case 0xD9: { int x; uint16_t ea = am_aby(c, &x); if (x) c->cycle_count++;
                 do_cmp(c, c->A, rd(c, ea)); break; }
    case 0xC1: do_cmp(c, c->A, rd(c, am_izx(c)));  break;
    case 0xD1: { int x; uint16_t ea = am_izy(c, &x); if (x) c->cycle_count++;
                 do_cmp(c, c->A, rd(c, ea)); break; }

    /* ── CPX / CPY ────────────────────────────────────────────────────── */
    case 0xE0: do_cmp(c, c->X, fetch(c));          break; /* CPX imm */
    case 0xE4: do_cmp(c, c->X, rd(c, am_zp(c)));  break; /* CPX zp  */
    case 0xEC: do_cmp(c, c->X, rd(c, am_abs(c))); break; /* CPX abs */
    case 0xC0: do_cmp(c, c->Y, fetch(c));          break; /* CPY imm */
    case 0xC4: do_cmp(c, c->Y, rd(c, am_zp(c)));  break; /* CPY zp  */
    case 0xCC: do_cmp(c, c->Y, rd(c, am_abs(c))); break; /* CPY abs */

    /* ── BIT ──────────────────────────────────────────────────────────── */
    case 0x24: do_bit(c, rd(c, am_zp(c)));  break;
    case 0x2C: do_bit(c, rd(c, am_abs(c))); break;

    /* ── ASL ──────────────────────────────────────────────────────────── */
    case 0x0A: rd(c, c->PC); c->A = do_asl(c, c->A); break; /* acc */
    case 0x06: { uint16_t ea = am_zp(c);  uint8_t v = rd(c, ea); wr(c, ea, v); wr(c, ea, do_asl(c, v)); break; }
    case 0x16: { uint16_t ea = am_zpx(c); uint8_t v = rd(c, ea); wr(c, ea, v); wr(c, ea, do_asl(c, v)); break; }
    case 0x0E: { uint16_t ea = am_abs(c); uint8_t v = rd(c, ea); wr(c, ea, v); wr(c, ea, do_asl(c, v)); break; }
    case 0x1E: { int x; uint16_t ea = am_abx(c, &x); c->cycle_count++;
                 uint8_t v = rd(c, ea); wr(c, ea, v); wr(c, ea, do_asl(c, v)); break; }

    /* ── LSR ──────────────────────────────────────────────────────────── */
    case 0x4A: rd(c, c->PC); c->A = do_lsr(c, c->A); break;
    case 0x46: { uint16_t ea = am_zp(c);  uint8_t v = rd(c, ea); wr(c, ea, v); wr(c, ea, do_lsr(c, v)); break; }
    case 0x56: { uint16_t ea = am_zpx(c); uint8_t v = rd(c, ea); wr(c, ea, v); wr(c, ea, do_lsr(c, v)); break; }
    case 0x4E: { uint16_t ea = am_abs(c); uint8_t v = rd(c, ea); wr(c, ea, v); wr(c, ea, do_lsr(c, v)); break; }
    case 0x5E: { int x; uint16_t ea = am_abx(c, &x); c->cycle_count++;
                 uint8_t v = rd(c, ea); wr(c, ea, v); wr(c, ea, do_lsr(c, v)); break; }

    /* ── ROL ──────────────────────────────────────────────────────────── */
    case 0x2A: rd(c, c->PC); c->A = do_rol(c, c->A); break;
    case 0x26: { uint16_t ea = am_zp(c);  uint8_t v = rd(c, ea); wr(c, ea, v); wr(c, ea, do_rol(c, v)); break; }
    case 0x36: { uint16_t ea = am_zpx(c); uint8_t v = rd(c, ea); wr(c, ea, v); wr(c, ea, do_rol(c, v)); break; }
    case 0x2E: { uint16_t ea = am_abs(c); uint8_t v = rd(c, ea); wr(c, ea, v); wr(c, ea, do_rol(c, v)); break; }
    case 0x3E: { int x; uint16_t ea = am_abx(c, &x); c->cycle_count++;
                 uint8_t v = rd(c, ea); wr(c, ea, v); wr(c, ea, do_rol(c, v)); break; }

    /* ── ROR ──────────────────────────────────────────────────────────── */
    case 0x6A: rd(c, c->PC); c->A = do_ror(c, c->A); break;
    case 0x66: { uint16_t ea = am_zp(c);  uint8_t v = rd(c, ea); wr(c, ea, v); wr(c, ea, do_ror(c, v)); break; }
    case 0x76: { uint16_t ea = am_zpx(c); uint8_t v = rd(c, ea); wr(c, ea, v); wr(c, ea, do_ror(c, v)); break; }
    case 0x6E: { uint16_t ea = am_abs(c); uint8_t v = rd(c, ea); wr(c, ea, v); wr(c, ea, do_ror(c, v)); break; }
    case 0x7E: { int x; uint16_t ea = am_abx(c, &x); c->cycle_count++;
                 uint8_t v = rd(c, ea); wr(c, ea, v); wr(c, ea, do_ror(c, v)); break; }

    /* ── INC / DEC memory ────────────────────────────────────────────── */
    case 0xE6: { uint16_t ea = am_zp(c);  uint8_t v = (uint8_t)(rd(c, ea) + 1); wr(c, ea, v-1); wr(c, ea, v); set_nz(c, v); break; }
    case 0xF6: { uint16_t ea = am_zpx(c); uint8_t v = (uint8_t)(rd(c, ea) + 1); wr(c, ea, v-1); wr(c, ea, v); set_nz(c, v); break; }
    case 0xEE: { uint16_t ea = am_abs(c); uint8_t v = (uint8_t)(rd(c, ea) + 1); wr(c, ea, v-1); wr(c, ea, v); set_nz(c, v); break; }
    case 0xFE: { int x; uint16_t ea = am_abx(c, &x); c->cycle_count++;
                 uint8_t v = (uint8_t)(rd(c, ea) + 1); wr(c, ea, v-1); wr(c, ea, v); set_nz(c, v); break; }
    case 0xC6: { uint16_t ea = am_zp(c);  uint8_t v = (uint8_t)(rd(c, ea) - 1); wr(c, ea, v+1); wr(c, ea, v); set_nz(c, v); break; }
    case 0xD6: { uint16_t ea = am_zpx(c); uint8_t v = (uint8_t)(rd(c, ea) - 1); wr(c, ea, v+1); wr(c, ea, v); set_nz(c, v); break; }
    case 0xCE: { uint16_t ea = am_abs(c); uint8_t v = (uint8_t)(rd(c, ea) - 1); wr(c, ea, v+1); wr(c, ea, v); set_nz(c, v); break; }
    case 0xDE: { int x; uint16_t ea = am_abx(c, &x); c->cycle_count++;
                 uint8_t v = (uint8_t)(rd(c, ea) - 1); wr(c, ea, v+1); wr(c, ea, v); set_nz(c, v); break; }

    /* ── INX / DEX / INY / DEY ───────────────────────────────────────── */
    case 0xE8: rd(c, c->PC); c->X++; set_nz(c, c->X); break; /* INX */
    case 0xCA: rd(c, c->PC); c->X--; set_nz(c, c->X); break; /* DEX */
    case 0xC8: rd(c, c->PC); c->Y++; set_nz(c, c->Y); break; /* INY */
    case 0x88: rd(c, c->PC); c->Y--; set_nz(c, c->Y); break; /* DEY */

    /* ── LDA ──────────────────────────────────────────────────────────── */
    case 0xA9: c->A = fetch(c);           set_nz(c, c->A); break;
    case 0xA5: c->A = rd(c, am_zp(c));   set_nz(c, c->A); break;
    case 0xB5: c->A = rd(c, am_zpx(c));  set_nz(c, c->A); break;
    case 0xAD: c->A = rd(c, am_abs(c));  set_nz(c, c->A); break;
    case 0xBD: { int x; uint16_t ea = am_abx(c, &x); if (x) c->cycle_count++;
                 c->A = rd(c, ea); set_nz(c, c->A); break; }
    case 0xB9: { int x; uint16_t ea = am_aby(c, &x); if (x) c->cycle_count++;
                 c->A = rd(c, ea); set_nz(c, c->A); break; }
    case 0xA1: c->A = rd(c, am_izx(c));  set_nz(c, c->A); break;
    case 0xB1: { int x; uint16_t ea = am_izy(c, &x); if (x) c->cycle_count++;
                 c->A = rd(c, ea); set_nz(c, c->A); break; }

    /* ── LDX ──────────────────────────────────────────────────────────── */
    case 0xA2: c->X = fetch(c);           set_nz(c, c->X); break;
    case 0xA6: c->X = rd(c, am_zp(c));   set_nz(c, c->X); break;
    case 0xB6: c->X = rd(c, am_zpy(c));  set_nz(c, c->X); break;
    case 0xAE: c->X = rd(c, am_abs(c));  set_nz(c, c->X); break;
    case 0xBE: { int x; uint16_t ea = am_aby(c, &x); if (x) c->cycle_count++;
                 c->X = rd(c, ea); set_nz(c, c->X); break; }

    /* ── LDY ──────────────────────────────────────────────────────────── */
    case 0xA0: c->Y = fetch(c);           set_nz(c, c->Y); break;
    case 0xA4: c->Y = rd(c, am_zp(c));   set_nz(c, c->Y); break;
    case 0xB4: c->Y = rd(c, am_zpx(c));  set_nz(c, c->Y); break;
    case 0xAC: c->Y = rd(c, am_abs(c));  set_nz(c, c->Y); break;
    case 0xBC: { int x; uint16_t ea = am_abx(c, &x); if (x) c->cycle_count++;
                 c->Y = rd(c, ea); set_nz(c, c->Y); break; }

    /* ── STA ──────────────────────────────────────────────────────────── */
    case 0x85: wr(c, am_zp(c),  c->A); break;
    case 0x95: wr(c, am_zpx(c), c->A); break;
    case 0x8D: wr(c, am_abs(c), c->A); break;
    case 0x9D: { int x; uint16_t ea = am_abx(c, &x); c->cycle_count++; wr(c, ea, c->A); break; }
    case 0x99: { int x; uint16_t ea = am_aby(c, &x); c->cycle_count++; wr(c, ea, c->A); break; }
    case 0x81: wr(c, am_izx(c), c->A); break;
    case 0x91: { int x; uint16_t ea = am_izy(c, &x); c->cycle_count++; wr(c, ea, c->A); break; }

    /* ── STX / STY ────────────────────────────────────────────────────── */
    case 0x86: wr(c, am_zp(c),  c->X); break; /* STX zp  */
    case 0x96: wr(c, am_zpy(c), c->X); break; /* STX zpy */
    case 0x8E: wr(c, am_abs(c), c->X); break; /* STX abs */
    case 0x84: wr(c, am_zp(c),  c->Y); break; /* STY zp  */
    case 0x94: wr(c, am_zpx(c), c->Y); break; /* STY zpx */
    case 0x8C: wr(c, am_abs(c), c->Y); break; /* STY abs */

    /* ── Register transfers ───────────────────────────────────────────── */
    case 0xAA: rd(c, c->PC); c->X = c->A;  set_nz(c, c->X); break; /* TAX */
    case 0xA8: rd(c, c->PC); c->Y = c->A;  set_nz(c, c->Y); break; /* TAY */
    case 0xBA: rd(c, c->PC); c->X = c->SP; set_nz(c, c->X); break; /* TSX */
    case 0x8A: rd(c, c->PC); c->A = c->X;  set_nz(c, c->A); break; /* TXA */
    case 0x9A: rd(c, c->PC); c->SP = c->X;                  break; /* TXS — no flag change */
    case 0x98: rd(c, c->PC); c->A = c->Y;  set_nz(c, c->A); break; /* TYA */

    /* ── Stack ────────────────────────────────────────────────────────── */
    case 0x48: rd(c, c->PC); push(c, c->A); break;                              /* PHA */
    case 0x08: rd(c, c->PC); push(c, c->P | MOS6502_P_B | MOS6502_P_U); break; /* PHP */
    case 0x68: rd(c, c->PC); rd(c, 0x0100u | c->SP); c->A = pop(c); set_nz(c, c->A); break; /* PLA */
    case 0x28: rd(c, c->PC); rd(c, 0x0100u | c->SP); /* PLP */
               c->P = (pop(c) & ~MOS6502_P_B) | MOS6502_P_U; break;

    /* ── Jumps / Calls / Returns ─────────────────────────────────────── */
    case 0x4C: c->PC = am_abs(c); break; /* JMP abs — 3 cycles */
    case 0x6C: {  /* JMP ind — 5 cycles; implements NMOS page-wrap bug */
        uint16_t ptr = am_abs(c);
        uint8_t lo = rd(c, ptr);
        uint8_t hi = rd(c, (uint16_t)((ptr & 0xFF00u) | ((ptr + 1) & 0x00FFu)));
        c->PC = (uint16_t)(lo | ((uint16_t)hi << 8));
        break;
    }
    case 0x20: {  /* JSR abs — 6 cycles */
        uint8_t lo = fetch(c);
        rd(c, 0x0100u | c->SP);          /* dummy stack read */
        push(c, (uint8_t)(c->PC >> 8));
        push(c, (uint8_t)(c->PC & 0xFFu));
        uint8_t hi = fetch(c);
        c->PC = (uint16_t)(lo | ((uint16_t)hi << 8));
        break;
    }
    case 0x60: {  /* RTS — 6 cycles */
        rd(c, c->PC);                    /* dummy */
        rd(c, 0x0100u | c->SP);          /* dummy SP read */
        uint8_t lo = pop(c);
        uint8_t hi = pop(c);
        c->PC = (uint16_t)(lo | ((uint16_t)hi << 8));
        rd(c, c->PC++);                  /* increment past JSR's saved PC */
        break;
    }
    case 0x40: {  /* RTI — 6 cycles */
        rd(c, c->PC);
        rd(c, 0x0100u | c->SP);
        c->P  = (pop(c) & ~MOS6502_P_B) | MOS6502_P_U;
        uint8_t lo = pop(c);
        uint8_t hi = pop(c);
        c->PC = (uint16_t)(lo | ((uint16_t)hi << 8));
        break;
    }

    /* ── Branches ─────────────────────────────────────────────────────── */
    case 0x90: do_branch(c, !(c->P & MOS6502_P_C)); break; /* BCC */
    case 0xB0: do_branch(c,  (c->P & MOS6502_P_C)); break; /* BCS */
    case 0xF0: do_branch(c,  (c->P & MOS6502_P_Z)); break; /* BEQ */
    case 0xD0: do_branch(c, !(c->P & MOS6502_P_Z)); break; /* BNE */
    case 0x30: do_branch(c,  (c->P & MOS6502_P_N)); break; /* BMI */
    case 0x10: do_branch(c, !(c->P & MOS6502_P_N)); break; /* BPL */
    case 0x70: do_branch(c,  (c->P & MOS6502_P_V)); break; /* BVS */
    case 0x50: do_branch(c, !(c->P & MOS6502_P_V)); break; /* BVC */

    /* ── Flag operations ─────────────────────────────────────────────── */
    case 0x18: rd(c, c->PC); c->P &= ~MOS6502_P_C; break; /* CLC */
    case 0x38: rd(c, c->PC); c->P |=  MOS6502_P_C; break; /* SEC */
    case 0x58: rd(c, c->PC); c->P &= ~MOS6502_P_I; break; /* CLI */
    case 0x78: rd(c, c->PC); c->P |=  MOS6502_P_I; break; /* SEI */
    case 0xB8: rd(c, c->PC); c->P &= ~MOS6502_P_V; break; /* CLV */
    case 0xD8: rd(c, c->PC); c->P &= ~MOS6502_P_D; break; /* CLD */
    case 0xF8: rd(c, c->PC); c->P |=  MOS6502_P_D; break; /* SED */

    /* ── NOP ──────────────────────────────────────────────────────────── */
    case 0xEA: rd(c, c->PC); break; /* NOP — 2 cycles */

    default:
        fprintf(stderr, "mos6502: illegal opcode 0x%02X at 0x%04X\n", op, (unsigned)(c->PC - 1));
        rd(c, c->PC);  /* consume 1 extra cycle like a 1-byte NOP */
        break;
    }
}
