#include "jemu/monitor.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <pthread.h>
#include <unistd.h>
#include <ctype.h>

#define QUEUE_SIZE 32
#define MEDIA_DEVICE_MAX 16

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
    JemuMediaDevice media[MEDIA_DEVICE_MAX];
    int             n_media;
};

/* ── Media helpers ───────────────────────────────────────────────────────── */

static char *next_token(char **p) {
    char *s = *p;
    while (*s && isspace((unsigned char)*s)) s++;
    if (!*s) {
        *p = s;
        return NULL;
    }
    char *tok = s;
    while (*s && !isspace((unsigned char)*s)) s++;
    if (*s) *s++ = '\0';
    *p = s;
    return tok;
}

static char *skip_ws(char *s) {
    while (*s && isspace((unsigned char)*s)) s++;
    return s;
}

static JemuMediaDevice *find_media(JemuMonitor *mon, const char *name) {
    if (!mon || !name) return NULL;
    for (int i = 0; i < mon->n_media; i++) {
        if (strcasecmp(mon->media[i].name, name) == 0)
            return &mon->media[i];
    }
    return NULL;
}

static void list_media(const JemuMonitor *mon) {
    if (!mon || mon->n_media == 0) {
        printf("media: no media devices registered\n");
        return;
    }
    printf("Media devices:\n");
    for (int i = 0; i < mon->n_media; i++) {
        const JemuMediaDevice *dev = &mon->media[i];
        char status[256] = "";
        if (dev->status)
            dev->status(dev->ud, status, sizeof(status));
        printf("  %-12s %-12s %s\n",
               dev->name, dev->kind ? dev->kind : "media", status);
    }
}

static bool dispatch_media(JemuMonitor *mon, const char *line,
                           JemuMonCmd *out_cmd) {
    char buf[256];
    snprintf(buf, sizeof(buf), "%s", line ? line : "");

    char *p = buf;
    char *verb = next_token(&p);
    if (!verb) return false;

    if (strcasecmp(verb, "media") == 0) {
        list_media(mon);
        *out_cmd = JEMU_MON_NONE;
        return true;
    }

    bool is_change = strcasecmp(verb, "change") == 0;
    bool is_eject  = strcasecmp(verb, "eject") == 0;
    if (!is_change && !is_eject)
        return false;

    char *dev_name = next_token(&p);
    if (!dev_name) {
        printf("usage: change <device> <file> | eject <device>\n");
        *out_cmd = JEMU_MON_NONE;
        return true;
    }

    JemuMediaDevice *dev = find_media(mon, dev_name);
    if (!dev) {
        printf("%s: no such media device '%s' (try 'media')\n",
               verb, dev_name);
        *out_cmd = JEMU_MON_NONE;
        return true;
    }

    JemuMediaResult result = JEMU_MEDIA_ERR;
    char err[256] = "";
    if (is_eject) {
        char *extra = next_token(&p);
        if (extra) {
            printf("usage: eject <device>\n");
            *out_cmd = JEMU_MON_NONE;
            return true;
        }
        if (!dev->eject) {
            printf("%s: device '%s' cannot be ejected\n", verb, dev_name);
            *out_cmd = JEMU_MON_NONE;
            return true;
        }
        result = dev->eject(dev->ud, err, sizeof(err));
    } else {
        char *arg = skip_ws(p);
        if (!*arg) {
            printf("usage: change <device> <file>\n");
            *out_cmd = JEMU_MON_NONE;
            return true;
        }
        if (strcasecmp(arg, "eject") == 0) {
            if (!dev->eject) {
                printf("change: device '%s' cannot be ejected\n", dev_name);
                *out_cmd = JEMU_MON_NONE;
                return true;
            }
            result = dev->eject(dev->ud, err, sizeof(err));
        } else {
            if (!dev->change) {
                printf("change: device '%s' cannot be changed\n", dev_name);
                *out_cmd = JEMU_MON_NONE;
                return true;
            }
            result = dev->change(dev->ud, arg, err, sizeof(err));
        }
    }

    if (result == JEMU_MEDIA_ERR) {
        if (err[0])
            printf("%s: %s\n", verb, err);
        else
            printf("%s: failed for media device '%s'\n", verb, dev_name);
        *out_cmd = JEMU_MON_NONE;
        return true;
    }

    *out_cmd = (result == JEMU_MEDIA_OK_RESET) ? JEMU_MON_RESET : JEMU_MON_NONE;
    return true;
}

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
                   "  media            list media devices\n"
                   "  dipswitch ...    machine-specific DIP switch command, when supported\n"
                   "  change <device> <file>   insert/change media\n"
                   "  eject <device>           eject media\n");
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

bool jemu_monitor_register_media(JemuMonitor *mon,
                                 const JemuMediaDevice *dev) {
    if (!mon || !dev || !dev->name || mon->n_media >= MEDIA_DEVICE_MAX)
        return false;
    mon->media[mon->n_media++] = *dev;
    return true;
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
            dispatch_media(mon, mon->last_text, &cmd);
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
