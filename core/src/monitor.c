#include "jemu/monitor.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <pthread.h>
#include <unistd.h>

#define QUEUE_SIZE 32

typedef enum {
    MON_ENTRY_CMD,
    MON_ENTRY_TEXT,
} MonEntryType;

typedef struct {
    MonEntryType type;
    JemuMonCmd   cmd;
    char         text[256];
} MonEntry;

struct JemuMonitor {
    MonEntry        queue[QUEUE_SIZE];
    int             head, tail;
    pthread_mutex_t lock;
    pthread_t       thread;
    bool            running;
    bool            paused;
    char            last_text[256];
};

/* ── Queue helpers ────────────────────────────────────────────────────────── */

static void enqueue(JemuMonitor *mon, JemuMonCmd cmd) {
    pthread_mutex_lock(&mon->lock);
    int next = (mon->tail + 1) % QUEUE_SIZE;
    if (next != mon->head) {   /* drop if full */
        mon->queue[mon->tail] = (MonEntry){
            .type = MON_ENTRY_CMD,
            .cmd = cmd,
            .text = {0},
        };
        mon->tail = next;
    }
    pthread_mutex_unlock(&mon->lock);
}

static void enqueue_text(JemuMonitor *mon, const char *line) {
    pthread_mutex_lock(&mon->lock);
    int next = (mon->tail + 1) % QUEUE_SIZE;
    if (next != mon->head) {   /* drop if full */
        mon->queue[mon->tail].type = MON_ENTRY_TEXT;
        mon->queue[mon->tail].cmd = JEMU_MON_CUSTOM;
        snprintf(mon->queue[mon->tail].text,
                 sizeof(mon->queue[mon->tail].text), "%s", line);
        mon->tail = next;
    }
    pthread_mutex_unlock(&mon->lock);
}

/* ── Reader thread ────────────────────────────────────────────────────────── */

static void *monitor_thread(void *arg) {
    JemuMonitor *mon = arg;
    char line[256];

    pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, NULL);
    pthread_setcanceltype(PTHREAD_CANCEL_DEFERRED, NULL);

    while (mon->running) {
        printf("(jemu) ");
        fflush(stdout);

        if (!fgets(line, sizeof(line), stdin)) break;

        /* Trim trailing whitespace */
        size_t len = strlen(line);
        while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r' ||
                           line[len-1] == ' '  || line[len-1] == '\t'))
            line[--len] = '\0';

        if (len == 0) continue;

        if (!strcmp(line, "help") || !strcmp(line, "?")) {
            printf("  reset            reset the machine\n"
                   "  q / quit         exit\n"
                   "  stop / halt      halt emulation\n"
                   "  cont / c         resume emulation\n"
                   "  step / s         execute one instruction (use after stop)\n"
                   "  dipswitch ...    machine-specific DIP switch command, when supported\n"
                   "  change <media> <arg>     insert/change/eject media (machine-specific)\n"
                   "                           e.g. tape, cartridge, floppy, cd\n"
                   "                           use 'eject' as arg to remove media\n");
        } else if (!strcmp(line, "reset") || !strcmp(line, "system_reset")) {
            enqueue(mon, JEMU_MON_RESET);
        } else if (!strcmp(line, "q") || !strcmp(line, "quit")) {
            enqueue(mon, JEMU_MON_QUIT);
            break;
        } else if (!strcmp(line, "stop") || !strcmp(line, "halt")) {
            enqueue(mon, JEMU_MON_STOP);
        } else if (!strcmp(line, "cont") || !strcmp(line, "c") ||
                   !strcmp(line, "resume")) {
            enqueue(mon, JEMU_MON_CONT);
        } else if (!strcmp(line, "step") || !strcmp(line, "s")) {
            enqueue(mon, JEMU_MON_STEP);
        } else {
            enqueue_text(mon, line);
        }
    }

    return NULL;
}

/* ── Public API ───────────────────────────────────────────────────────────── */

JemuMonitor *jemu_monitor_create(void) {
    JemuMonitor *mon = calloc(1, sizeof(*mon));
    pthread_mutex_init(&mon->lock, NULL);
    return mon;
}

void jemu_monitor_destroy(JemuMonitor *mon) {
    if (!mon) return;
    pthread_mutex_destroy(&mon->lock);
    free(mon);
}

void jemu_monitor_start(JemuMonitor *mon) {
    if (!isatty(STDIN_FILENO)) return; /* no console, stay silent */
    mon->running = true;
    pthread_create(&mon->thread, NULL, monitor_thread, mon);
}

void jemu_monitor_stop(JemuMonitor *mon) {
    if (!mon->running) return;
    mon->running = false;
    pthread_cancel(mon->thread);
    pthread_join(mon->thread, NULL);
    printf("\n"); /* tidy up any dangling prompt */
}

JemuMonCmd jemu_monitor_poll(JemuMonitor *mon) {
    if (!mon) return JEMU_MON_NONE;
    pthread_mutex_lock(&mon->lock);
    JemuMonCmd cmd = JEMU_MON_NONE;
    if (mon->head != mon->tail) {
        MonEntry entry = mon->queue[mon->head];
        mon->head = (mon->head + 1) % QUEUE_SIZE;
        if (entry.type == MON_ENTRY_TEXT) {
            cmd = JEMU_MON_CUSTOM;
            snprintf(mon->last_text, sizeof(mon->last_text),
                     "%s", entry.text);
        } else {
            cmd = entry.cmd;
            mon->last_text[0] = '\0';
        }
        /* Keep paused state in sync so callers can just check is_paused() */
        if (cmd == JEMU_MON_STOP || cmd == JEMU_MON_STEP) mon->paused = true;
        if (cmd == JEMU_MON_CONT) mon->paused = false;
    }
    pthread_mutex_unlock(&mon->lock);
    return cmd;
}

const char *jemu_monitor_command_text(const JemuMonitor *mon) {
    return mon ? mon->last_text : "";
}

void jemu_monitor_unknown_command(const JemuMonitor *mon) {
    const char *text = jemu_monitor_command_text(mon);
    if (text && text[0]) {
        char buf[256];
        snprintf(buf, sizeof(buf), "%s", text);
        char *cmd = strtok(buf, " \t");
        if (cmd && strcasecmp(cmd, "dipswitch") == 0) {
            printf("dipswitch: dip switches are not available on this machine\n");
            return;
        }
        printf("unknown command '%s' (try 'help')\n", text);
    }
}

bool jemu_monitor_is_paused(const JemuMonitor *mon) {
    return mon && mon->paused;
}
