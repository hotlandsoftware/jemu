#pragma once
#include <stdbool.h>

/*
 * JEMU monitor — QEMU-style interactive console on stdin.
 *
 * A background thread reads lines from stdin and enqueues commands.
 * The emulator's run loop drains the queue each frame via jemu_monitor_poll().
 * Only active when stdin is a TTY; silently inert when piped or redirected.
 */

typedef enum {
    JEMU_MON_NONE = 0,
    JEMU_MON_RESET,
    JEMU_MON_QUIT,
    JEMU_MON_STOP,  /* halt emulation  */
    JEMU_MON_CONT,  /* resume          */
    JEMU_MON_STEP,  /* one instruction */
    JEMU_MON_CUSTOM,/* machine-specific command */
} JemuMonCmd;

typedef struct JemuMonitor JemuMonitor;

JemuMonitor *jemu_monitor_create(void);
void         jemu_monitor_destroy(JemuMonitor *mon);

/* Start the stdin reader thread (no-op if not a TTY). */
void         jemu_monitor_start(JemuMonitor *mon);
/* Signal and join the reader thread. */
void         jemu_monitor_stop(JemuMonitor *mon);

/* Dequeue one command (NONE if empty). Also updates the paused flag
 * for STOP/CONT/STEP commands so callers don't have to. */
JemuMonCmd   jemu_monitor_poll(JemuMonitor *mon);
const char  *jemu_monitor_command_text(const JemuMonitor *mon);
void         jemu_monitor_unknown_command(const JemuMonitor *mon);

bool         jemu_monitor_is_paused(const JemuMonitor *mon);
