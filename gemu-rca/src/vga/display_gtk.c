#include "rca_display.h"
#include "gemu/gtk_menu.h"
#include <gtk/gtk.h>
#include <stdlib.h>
#include <string.h>

#define RCA_GTK_MAX_KEYS 128
#define RCA_GTK_DEFAULT_SCALE 4

typedef struct {
    GtkWidget       *window;
    GtkWidget       *drawing_area;
    cairo_surface_t *surface;
    int              w;
    int              h;
    int              scale;
    const uint32_t  *palette;
    int              n_colors;
    uint32_t         pixel_on;
    uint32_t         pixel_off;
    bool             active;
    bool             quit;
    struct {
        uint32_t keysym;
        bool     down;
    } keys[RCA_GTK_MAX_KEYS];
    int              n_keys;
    uint32_t         queued_keysym;
} RcaGtkCtx;

static int find_key(RcaGtkCtx *c, uint32_t keysym) {
    for (int i = 0; i < c->n_keys; i++)
        if (c->keys[i].keysym == keysym)
            return i;
    return -1;
}

static void set_key(RcaGtkCtx *c, uint32_t keysym, bool down) {
    if (keysym >= 'A' && keysym <= 'Z')
        keysym += 'a' - 'A';
    int idx = find_key(c, keysym);
    if (idx < 0) {
        if (c->n_keys >= RCA_GTK_MAX_KEYS)
            return;
        idx = c->n_keys++;
        c->keys[idx].keysym = keysym;
    }
    c->keys[idx].down = down;
    if (down)
        c->queued_keysym = keysym;
}

static uint32_t gtk_key_to_rca(guint keyval) {
    guint lower = gdk_keyval_to_lower(keyval);
    if (lower >= 0x20 && lower <= 0x7e)
        return (uint32_t)lower;
    switch (keyval) {
    case GDK_KEY_Escape: return RCA_KEY_ESCAPE;
    case GDK_KEY_Tab:    return RCA_KEY_TAB;
    case GDK_KEY_F2:     return RCA_KEY_F2;
    case GDK_KEY_F3:     return RCA_KEY_F3;
    case GDK_KEY_Left:   return RCA_KEY_LEFT;
    case GDK_KEY_Right:  return RCA_KEY_RIGHT;
    case GDK_KEY_Alt_L:  return RCA_KEY_LALT;
    case GDK_KEY_KP_0:   return RCA_KEY_KP_0;
    case GDK_KEY_KP_1:   return RCA_KEY_KP_1;
    case GDK_KEY_KP_2:   return RCA_KEY_KP_2;
    case GDK_KEY_KP_3:   return RCA_KEY_KP_3;
    case GDK_KEY_KP_4:   return RCA_KEY_KP_4;
    case GDK_KEY_KP_5:   return RCA_KEY_KP_5;
    case GDK_KEY_KP_6:   return RCA_KEY_KP_6;
    case GDK_KEY_KP_7:   return RCA_KEY_KP_7;
    case GDK_KEY_KP_8:   return RCA_KEY_KP_8;
    case GDK_KEY_KP_9:   return RCA_KEY_KP_9;
    case GDK_KEY_Return: return '\r';
    case GDK_KEY_BackSpace: return '\b';
    default:            return 0;
    }
}

static gboolean on_draw(GtkWidget *w, cairo_t *cr, gpointer data) {
    (void)w;
    RcaGtkCtx *c = data;
    if (!c->active) {
        cairo_set_source_rgb(cr, 0, 0, 0);
        cairo_paint(cr);
        return FALSE;
    }

    cairo_save(cr);
    cairo_scale(cr, c->scale, c->scale);
    cairo_set_source_surface(cr, c->surface, 0, 0);
    cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_NEAREST);
    cairo_paint(cr);
    cairo_restore(cr);
    return FALSE;
}

static gboolean on_key(GtkWidget *w, GdkEventKey *ev, gpointer data) {
    (void)w;
    RcaGtkCtx *c = data;
    bool down = ev->type == GDK_KEY_PRESS;
    uint32_t sym = gtk_key_to_rca(ev->keyval);
    if (!sym)
        return TRUE;
    if (down && sym == RCA_KEY_ESCAPE) {
        c->quit = true;
        return TRUE;
    }
    set_key(c, sym, down);
    return TRUE;
}

static gboolean on_delete(GtkWidget *w, GdkEvent *ev, gpointer data) {
    (void)w;
    (void)ev;
    ((RcaGtkCtx *)data)->quit = true;
    return TRUE;
}

static void gtk_poll(void *ctx) {
    (void)ctx;
    while (gtk_events_pending())
        gtk_main_iteration_do(FALSE);
}

static bool gtk_should_quit(void *ctx) {
    return ((RcaGtkCtx *)ctx)->quit;
}

static bool gtk_key_down(void *ctx, uint32_t keysym) {
    RcaGtkCtx *c = ctx;
    if (keysym >= 'A' && keysym <= 'Z')
        keysym += 'a' - 'A';
    int idx = find_key(c, keysym);
    return idx >= 0 && c->keys[idx].down;
}

static uint32_t gtk_pop_keysym(void *ctx) {
    RcaGtkCtx *c = ctx;
    uint32_t keysym = c->queued_keysym;
    c->queued_keysym = 0;
    return keysym;
}

static void gtk_render(void *ctx, const uint8_t *vram, int w, int h) {
    RcaGtkCtx *c = ctx;
    if (w != c->w || h != c->h)
        return;

    uint32_t *px = (uint32_t *)cairo_image_surface_get_data(c->surface);
    int stride = cairo_image_surface_get_stride(c->surface) / 4;
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            if (c->palette && c->n_colors > 0) {
                uint8_t idx = vram[y * w + x];
                if (idx >= c->n_colors) idx = (uint8_t)(c->n_colors - 1);
                px[y * stride + x] = c->palette[idx];
            } else {
                px[y * stride + x] = vram[y * w + x] ? c->pixel_on : c->pixel_off;
            }
        }
    }
    cairo_surface_mark_dirty(c->surface);
    c->active = true;
    gtk_widget_queue_draw(c->drawing_area);
    gtk_poll(c);
}

static void gtk_destroy(void *ctx) {
    RcaGtkCtx *c = ctx;
    if (!c) return;
    if (c->surface)
        cairo_surface_destroy(c->surface);
    if (c->window)
        gtk_widget_destroy(c->window);
    free(c);
}

static RcaDisplay *gtk_create_common(const char *title, int w, int h,
                                     int scale, const uint32_t *palette,
                                     int n_colors, uint32_t on,
                                     uint32_t off, GemuMonitor *mon) {
    if (scale <= 0) scale = RCA_GTK_DEFAULT_SCALE;
    if (!gtk_init_check(NULL, NULL))
        return NULL;

    RcaGtkCtx *c = calloc(1, sizeof(*c));
    if (!c) return NULL;
    c->w = w;
    c->h = h;
    c->scale = scale;
    c->palette = palette;
    c->n_colors = n_colors;
    c->pixel_on = on;
    c->pixel_off = off;
    c->surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);

    c->window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(c->window), title ? title : "GEMU");
    gtk_window_set_resizable(GTK_WINDOW(c->window), FALSE);

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_container_add(GTK_CONTAINER(c->window), vbox);

    gemu_gtk_add_action_menu(vbox, mon);

    c->drawing_area = gtk_drawing_area_new();
    gtk_widget_set_size_request(c->drawing_area, w * scale, h * scale);
    gtk_box_pack_start(GTK_BOX(vbox), c->drawing_area, TRUE, TRUE, 0);

    gtk_widget_add_events(c->window, GDK_KEY_PRESS_MASK | GDK_KEY_RELEASE_MASK);
    g_signal_connect(c->window,       "delete-event",      G_CALLBACK(on_delete), c);
    g_signal_connect(c->window,       "key-press-event",   G_CALLBACK(on_key),    c);
    g_signal_connect(c->window,       "key-release-event", G_CALLBACK(on_key),    c);
    g_signal_connect(c->drawing_area, "draw",              G_CALLBACK(on_draw),   c);

    gtk_widget_show_all(c->window);
    gtk_poll(c);

    RcaDisplay *d = calloc(1, sizeof(*d));
    if (!d) {
        gtk_destroy(c);
        return NULL;
    }
    d->render = gtk_render;
    d->destroy = gtk_destroy;
    d->poll = gtk_poll;
    d->should_quit = gtk_should_quit;
    d->key_down = gtk_key_down;
    d->pop_keysym = gtk_pop_keysym;
    d->ctx = c;
    return d;
}

RcaDisplay *rca_display_gtk_create_mono(const char *title, int w, int h,
                                        int scale, uint32_t on,
                                        uint32_t off, GemuMonitor *mon) {
    return gtk_create_common(title, w, h, scale, NULL, 0, on, off, mon);
}

RcaDisplay *rca_display_gtk_create_indexed(const char *title, int w, int h,
                                           int scale,
                                           const uint32_t *palette,
                                           int n_colors, GemuMonitor *mon) {
    return gtk_create_common(title, w, h, scale, palette, n_colors,
                             0xFFFFFFFFu, 0xFF000000u, mon);
}
