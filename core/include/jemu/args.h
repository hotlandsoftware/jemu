#pragma once
#include "display.h"
#include <stdbool.h>

/*
 * Universal argument parser shared by all jemu binaries.
 *
 * Each binary registers its supported devices and flags via JemuArgsDef.
 * jemu_args_parse() handles -M, -cpu, -vga, -display, -scale, -vnc, and -h.
 * Binary-specific flags (e.g. chip8's -hz) are returned unmodified in
 * rem_argv[0..(*rem_argc)-1]; rem_argv must be pre-allocated to argc entries.
 * Pass NULL/NULL for rem_argc/rem_argv if no binary-specific flags exist.
 */

typedef struct {
    const char *name;
    const char *desc;
} JemuDevDesc;

/* Bitmask helper: JEMU_DISP_F(JEMU_DISPLAY_SDL) | JEMU_DISP_F(...) */
#define JEMU_DISP_F(t) (1u << (unsigned)(t))

typedef struct {
    const char        *prog;            /* binary name shown in usage */
    const JemuDevDesc *machines;
    int                n_machines;
    const JemuDevDesc *cpus;            /* NULL / n_cpus=0 → -cpu not listed */
    int                n_cpus;
    const JemuDevDesc *vgas;            /* NULL / n_vgas=0 → -vga not listed */
    int                n_vgas;
    unsigned           display_mask;    /* bitfield of supported JemuDisplayType */
    bool               vnc_support;     /* whether -vnc is a valid option */
    const char        *extra_help;      /* optional binary-specific usage text */
} JemuArgsDef;

/*
 * Caller initialises JemuArgs with its own defaults before calling.
 * jemu_args_parse only writes fields that were explicitly given on the
 * command line.
 */
typedef struct {
    const char     *rom_path;
    const char     *machine;       /* NULL until -M given */
    const char     *cpu;           /* NULL until -cpu given */
    const char     *vga;           /* NULL until -vga given */
    JemuDisplayType display_type;
    int             display_scale;
    const char     *vnc_addr;      /* NULL = no VNC */
    bool            display_explicit; /* true if -display was given */
} JemuArgs;

/*
 * Parse argc/argv.
 * -h/-help/--help, -M ?, -cpu ?, -vga ?, -display ? all print and exit(0).
 * Returns false and prints an error message on invalid input.
 */
bool jemu_args_parse(int argc, char **argv,
                     const JemuArgsDef *def, JemuArgs *out,
                     int *rem_argc, char **rem_argv);
