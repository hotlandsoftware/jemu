#include "jemu/monitor.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>

#define QUEUE_SIZE 32

struct JemuMonitor {
    JemuMonCmd      queue[QUEUE_SIZE];
    int             head, tail;
    pthread_mutex_t lock;
    pthread_t       thread;
    bool            running;
    bool            paused;
};

/* ── Queue helpers ────────────────────────────────────────────────────────── */

static void enqueue(JemuMonitor *mon, JemuMonCmd cmd) {
    pthread_mutex_lock(&mon->lock);
    int next = (mon->tail + 1) % QUEUE_SIZE;
    if (next != mon->head) {   /* drop if full */
        mon->queue[mon->tail] = cmd;
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
                   "  step / s         execute one instruction (use after stop)\n");
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
            printf("unknown command '%s' (try 'help')\n", line);
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
        cmd = mon->queue[mon->head];
        mon->head = (mon->head + 1) % QUEUE_SIZE;
        /* Keep paused state in sync so callers can just check is_paused() */
        if (cmd == JEMU_MON_STOP || cmd == JEMU_MON_STEP) mon->paused = true;
        if (cmd == JEMU_MON_CONT) mon->paused = false;
    }
    pthread_mutex_unlock(&mon->lock);
    return cmd;
}

bool jemu_monitor_is_paused(const JemuMonitor *mon) {
    return mon && mon->paused;
}
