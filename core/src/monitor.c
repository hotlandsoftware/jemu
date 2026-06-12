#include "gemu/monitor.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <ctype.h>
#ifdef _WIN32
#  include <io.h>
#  define isatty(fd)   _isatty(fd)
#  define STDIN_FILENO 0
#  define strcasecmp   _stricmp
#else
#  include <strings.h>
#  include <unistd.h>
#endif

#define QUEUE_SIZE 32
#define MEDIA_DEVICE_MAX 16

typedef enum {
    MON_ENTRY_CMD,
    MON_ENTRY_TEXT,
} MonEntryType;

typedef struct {
    MonEntryType type;
    GemuMonCmd   cmd;
    uint32_t     step_count; /* only used when cmd == GEMU_MON_STEP */
    char         text[256];
} MonEntry;

struct GemuMonitor {
    MonEntry        queue[QUEUE_SIZE];
    int             head, tail;
    pthread_mutex_t lock;
    pthread_t       thread;
    bool            running;
    bool            paused;
    char            last_text[256];
    uint32_t        last_step_count;
    GemuMediaDevice media[MEDIA_DEVICE_MAX];
    int             n_media;
    bool          (*screendump_cb)(void *ud, const char *path);
    void           *screendump_ud;
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

/* Skip leading whitespace; if the argument is wrapped in matching single or
 * double quotes, strip them so callers receive a bare path. */
static char *unquote_arg(char *s) {
    while (*s && isspace((unsigned char)*s)) s++;
    if (*s == '\'' || *s == '"') {
        char q = *s++;
        char *end = s + strlen(s);
        if (end > s && *(end - 1) == q) *(end - 1) = '\0';
    }
    return s;
}

static GemuMediaDevice *find_media(GemuMonitor *mon, const char *name) {
    if (!mon || !name) return NULL;
    for (int i = 0; i < mon->n_media; i++) {
        if (strcasecmp(mon->media[i].name, name) == 0)
            return &mon->media[i];
    }
    return NULL;
}

static void list_media(const GemuMonitor *mon) {
    if (!mon || mon->n_media == 0) {
        printf("media: no media devices registered\n");
        return;
    }
    printf("Media devices:\n");
    for (int i = 0; i < mon->n_media; i++) {
        const GemuMediaDevice *dev = &mon->media[i];
        char status[256] = "";
        if (dev->status)
            dev->status(dev->ud, status, sizeof(status));
        printf("  %-12s %-12s %s\n",
               dev->name, dev->kind ? dev->kind : "media", status);
    }
}

static bool dispatch_media(GemuMonitor *mon, const char *line,
                           GemuMonCmd *out_cmd) {
    char buf[256];
    snprintf(buf, sizeof(buf), "%s", line ? line : "");

    char *p = buf;
    char *verb = next_token(&p);
    if (!verb) return false;

    if (strcasecmp(verb, "screendump") == 0) {
        char *arg = unquote_arg(p);
        if (!*arg) {
            printf("usage: screendump <filename>[.png]\n");
        } else if (!mon->screendump_cb) {
            printf("screendump: not supported by this machine\n");
        } else {
            mon->screendump_cb(mon->screendump_ud, arg);
        }
        *out_cmd = GEMU_MON_NONE;
        return true;
    }

    if (strcasecmp(verb, "media") == 0) {
        list_media(mon);
        *out_cmd = GEMU_MON_NONE;
        return true;
    }

    bool is_change = strcasecmp(verb, "change") == 0;
    bool is_eject  = strcasecmp(verb, "eject") == 0;
    if (!is_change && !is_eject)
        return false;

    char *dev_name = next_token(&p);
    if (!dev_name) {
        printf("usage: change <device> <file> | eject <device>\n");
        *out_cmd = GEMU_MON_NONE;
        return true;
    }

    GemuMediaDevice *dev = find_media(mon, dev_name);
    if (!dev) {
        printf("%s: no such media device '%s' (try 'media')\n",
               verb, dev_name);
        *out_cmd = GEMU_MON_NONE;
        return true;
    }

    GemuMediaResult result = GEMU_MEDIA_ERR;
    char err[256] = "";
    if (is_eject) {
        char *extra = next_token(&p);
        if (extra) {
            printf("usage: eject <device>\n");
            *out_cmd = GEMU_MON_NONE;
            return true;
        }
        if (!dev->eject) {
            printf("%s: device '%s' cannot be ejected\n", verb, dev_name);
            *out_cmd = GEMU_MON_NONE;
            return true;
        }
        result = dev->eject(dev->ud, err, sizeof(err));
    } else {
        char *arg = unquote_arg(p);
        if (!*arg) {
            printf("usage: change <device> <file>\n");
            *out_cmd = GEMU_MON_NONE;
            return true;
        }
        if (strcasecmp(arg, "eject") == 0) {
            if (!dev->eject) {
                printf("change: device '%s' cannot be ejected\n", dev_name);
                *out_cmd = GEMU_MON_NONE;
                return true;
            }
            result = dev->eject(dev->ud, err, sizeof(err));
        } else {
            if (!dev->change) {
                printf("change: device '%s' cannot be changed\n", dev_name);
                *out_cmd = GEMU_MON_NONE;
                return true;
            }
            result = dev->change(dev->ud, arg, err, sizeof(err));
        }
    }

    if (result == GEMU_MEDIA_ERR) {
        if (err[0])
            printf("%s: %s\n", verb, err);
        else
            printf("%s: failed for media device '%s'\n", verb, dev_name);
        *out_cmd = GEMU_MON_NONE;
        return true;
    }

    *out_cmd = (result == GEMU_MEDIA_OK_RESET) ? GEMU_MON_RESET : GEMU_MON_NONE;
    return true;
}

/* ── Queue helpers ────────────────────────────────────────────────────────── */

static void enqueue(GemuMonitor *mon, GemuMonCmd cmd, uint32_t step_count) {
    pthread_mutex_lock(&mon->lock);
    int next = (mon->tail + 1) % QUEUE_SIZE;
    if (next != mon->head) {   /* drop if full */
        mon->queue[mon->tail] = (MonEntry){
            .type = MON_ENTRY_CMD,
            .cmd = cmd,
            .step_count = step_count,
            .text = {0},
        };
        mon->tail = next;
    }
    pthread_mutex_unlock(&mon->lock);
}

static void enqueue_text(GemuMonitor *mon, const char *line) {
    pthread_mutex_lock(&mon->lock);
    int next = (mon->tail + 1) % QUEUE_SIZE;
    if (next != mon->head) {   /* drop if full */
        mon->queue[mon->tail].type = MON_ENTRY_TEXT;
        mon->queue[mon->tail].cmd = GEMU_MON_CUSTOM;
        snprintf(mon->queue[mon->tail].text,
                 sizeof(mon->queue[mon->tail].text), "%s", line);
        mon->tail = next;
    }
    pthread_mutex_unlock(&mon->lock);
}

/* ── Reader thread ────────────────────────────────────────────────────────── */

static void *monitor_thread(void *arg) {
    GemuMonitor *mon = arg;
    char line[256];

    pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, NULL);
    pthread_setcanceltype(PTHREAD_CANCEL_DEFERRED, NULL);

    while (mon->running) {
        printf("(gemu) ");
        fflush(stdout);

        if (!fgets(line, sizeof(line), stdin)) break;

        /* Trim trailing whitespace */
        size_t len = strlen(line);
        while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r' ||
                           line[len-1] == ' '  || line[len-1] == '\t'))
            line[--len] = '\0';

        if (len == 0) continue;

        if (!strcmp(line, "help") || !strcmp(line, "?")) {
            printf(
            "  change <device> <file> -- insert/change media\n"
            "  cont / c -- resume emulation\n"
            "  dipswitch -- lists DIP switches (when supported)\n"
            "  dipswitch [name] [value] -- sets DIP switch (when supported)\n"
            "  eject <device> -- eject media\n"
            "  media -- list media devices\n"
            "  q / quit -- quits the machine immediately\n"
            "  reset -- reset the machine\n"
            "  screendump <file>[.png] -- save screenshot (PPM or PNG)\n"
            "  step / s [count] -- step through (x) instructions (defaults to 1)\n"
            "  stop / halt -- halt emulation\n");
        } else if (!strcmp(line, "reset") || !strcmp(line, "system_reset")) {
            enqueue(mon, GEMU_MON_RESET, 0);
        } else if (!strcmp(line, "q") || !strcmp(line, "quit")) {
            enqueue(mon, GEMU_MON_QUIT, 0);
            break;
        } else if (!strcmp(line, "stop") || !strcmp(line, "halt")) {
            enqueue(mon, GEMU_MON_STOP, 0);
        } else if (!strcmp(line, "cont") || !strcmp(line, "c") ||
                   !strcmp(line, "resume") || !strcmp(line, "continue")) {
            enqueue(mon, GEMU_MON_CONT, 0);
        } else if ((strncmp(line, "step", 4) == 0 && (line[4] == '\0' || line[4] == ' ')) ||
                   (line[0] == 's' && (line[1] == '\0' || line[1] == ' '))) {
            char *p = line + (line[1] == 't' ? 4 : 1);
            while (*p == ' ') p++;
            uint32_t count = (*p != '\0') ? (uint32_t)strtoul(p, NULL, 10) : 1;
            if (count == 0) count = 1;
            enqueue(mon, GEMU_MON_STEP, count);
        } else {
            enqueue_text(mon, line);
        }
    }

    return NULL;
}

/* ── Public API ───────────────────────────────────────────────────────────── */

GemuMonitor *gemu_monitor_create(void) {
    GemuMonitor *mon = calloc(1, sizeof(*mon));
    pthread_mutex_init(&mon->lock, NULL);
    return mon;
}

void gemu_monitor_destroy(GemuMonitor *mon) {
    if (!mon) return;
    pthread_mutex_destroy(&mon->lock);
    free(mon);
}

void gemu_monitor_enqueue_reset(GemuMonitor *mon) {
    if (mon) enqueue(mon, GEMU_MON_RESET, 0);
}

void gemu_monitor_enqueue_quit(GemuMonitor *mon) {
    if (mon) enqueue(mon, GEMU_MON_QUIT, 0);
}

bool gemu_monitor_register_media(GemuMonitor *mon,
                                 const GemuMediaDevice *dev) {
    if (!mon || !dev || !dev->name || mon->n_media >= MEDIA_DEVICE_MAX)
        return false;
    mon->media[mon->n_media++] = *dev;
    return true;
}

void gemu_monitor_start(GemuMonitor *mon) {
    if (!isatty(STDIN_FILENO)) return; /* no console, stay silent */
    mon->running = true;
    pthread_create(&mon->thread, NULL, monitor_thread, mon);
}

void gemu_monitor_stop(GemuMonitor *mon) {
    if (!mon->running) return;
    mon->running = false;
    pthread_cancel(mon->thread);
    pthread_join(mon->thread, NULL);
    printf("\n"); /* tidy up any dangling prompt */
}

GemuMonCmd gemu_monitor_poll(GemuMonitor *mon) {
    if (!mon) return GEMU_MON_NONE;
    pthread_mutex_lock(&mon->lock);
    GemuMonCmd cmd = GEMU_MON_NONE;
    if (mon->head != mon->tail) {
        MonEntry entry = mon->queue[mon->head];
        mon->head = (mon->head + 1) % QUEUE_SIZE;
        if (entry.type == MON_ENTRY_TEXT) {
            cmd = GEMU_MON_CUSTOM;
            snprintf(mon->last_text, sizeof(mon->last_text),
                     "%s", entry.text);
            dispatch_media(mon, mon->last_text, &cmd);
        } else {
            cmd = entry.cmd;
            mon->last_text[0] = '\0';
            if (cmd == GEMU_MON_STEP)
                mon->last_step_count = entry.step_count ? entry.step_count : 1;
        }
        /* Keep paused state in sync so callers can just check is_paused() */
        if (cmd == GEMU_MON_STOP || cmd == GEMU_MON_STEP) mon->paused = true;
        if (cmd == GEMU_MON_CONT) mon->paused = false;
    }
    pthread_mutex_unlock(&mon->lock);
    return cmd;
}

uint32_t gemu_monitor_step_count(const GemuMonitor *mon) {
    return (mon && mon->last_step_count) ? mon->last_step_count : 1;
}

const char *gemu_monitor_command_text(const GemuMonitor *mon) {
    return mon ? mon->last_text : "";
}

void gemu_monitor_unknown_command(const GemuMonitor *mon) {
    const char *text = gemu_monitor_command_text(mon);
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

bool gemu_monitor_is_paused(const GemuMonitor *mon) {
    return mon && mon->paused;
}

void gemu_monitor_set_screendump_cb(GemuMonitor *mon,
                                    bool (*cb)(void *ud, const char *path),
                                    void *ud) {
    if (!mon) return;
    mon->screendump_cb = cb;
    mon->screendump_ud = ud;
}
