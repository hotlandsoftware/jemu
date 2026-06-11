#include "nes_display.h"
#include "../hardware/nes.h"
#include "../vga/rp2c02.h"
#include "gemu/gtk_menu.h"
#include <gtk/gtk.h>
#include <stdlib.h>

#define NES_GTK_DEFAULT_SCALE 2

typedef struct {
    GtkWidget        *window;
    GtkWidget        *drawing_area;
    cairo_surface_t  *surface;
    const uint32_t   *palette;
    int               scale;
    bool              active;
    bool              quit;
    uint8_t           ctrl1;
} NesGtkCtx;

static uint8_t gdk_key_to_btn(guint k) {
    switch (k) {
    case GDK_KEY_z: case GDK_KEY_Z:             return NES_BTN_A;
    case GDK_KEY_x: case GDK_KEY_X:             return NES_BTN_B;
    case GDK_KEY_Return: case GDK_KEY_KP_Enter: return NES_BTN_START;
    case GDK_KEY_Shift_R:                        return NES_BTN_SELECT;
    case GDK_KEY_Up:                             return NES_BTN_UP;
    case GDK_KEY_Down:                           return NES_BTN_DOWN;
    case GDK_KEY_Left:                           return NES_BTN_LEFT;
    case GDK_KEY_Right:                          return NES_BTN_RIGHT;
    default:                                     return 0;
    }
}

static gboolean on_draw(GtkWidget *w, cairo_t *cr, gpointer data) {
    (void)w;
    NesGtkCtx *c = data;
    if (!c->active) { cairo_set_source_rgb(cr, 0, 0, 0); cairo_paint(cr); return FALSE; }
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
    NesGtkCtx *c = data;
    bool down = ev->type == GDK_KEY_PRESS;
    if (down && ev->keyval == GDK_KEY_Escape) { c->quit = true; return TRUE; }
    uint8_t btn = gdk_key_to_btn(ev->keyval);
    if (!btn) return FALSE;
    if (down) c->ctrl1 |=  btn;
    else      c->ctrl1 &= ~btn;
    return TRUE;
}

static gboolean on_delete(GtkWidget *w, GdkEvent *ev, gpointer data) {
    (void)w; (void)ev;
    ((NesGtkCtx *)data)->quit = true;
    return TRUE;
}

static void gtk_render(void *vctx, const uint8_t *pixels, int w, int h) {
    NesGtkCtx *c = vctx;
    if (w != RP2C02_WIDTH || h != RP2C02_HEIGHT) return;
    uint32_t *px     = (uint32_t *)cairo_image_surface_get_data(c->surface);
    int       stride = cairo_image_surface_get_stride(c->surface) / 4;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
            px[y * stride + x] = c->palette[pixels[y * w + x] & 0x3Fu];
    cairo_surface_mark_dirty(c->surface);
    c->active = true;
    gtk_widget_queue_draw(c->drawing_area);
    while (gtk_events_pending()) gtk_main_iteration_do(FALSE);
}

static void gtk_poll(void *vctx) {
    (void)vctx;
    while (gtk_events_pending()) gtk_main_iteration_do(FALSE);
}

static void gtk_destroy(void *vctx) {
    NesGtkCtx *c = vctx;
    if (!c) return;
    if (c->surface) cairo_surface_destroy(c->surface);
    if (c->window)  gtk_widget_destroy(c->window);
    free(c);
}

static bool    gtk_should_quit(void *vctx) { return ((NesGtkCtx *)vctx)->quit; }
static uint8_t gtk_ctrl1(void *vctx)       { return ((NesGtkCtx *)vctx)->ctrl1; }

NesDisplay *nes_display_gtk_create(const char *title,
                                   const uint32_t *palette, int scale,
                                   GemuMonitor *mon) {
    if (scale <= 0) scale = NES_GTK_DEFAULT_SCALE;
    if (!gtk_init_check(NULL, NULL)) return NULL;

    NesGtkCtx *c = calloc(1, sizeof(*c));
    if (!c) return NULL;
    c->palette = palette;
    c->scale   = scale;
    c->surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32,
                                            RP2C02_WIDTH, RP2C02_HEIGHT);

    c->window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(c->window), title ? title : "GEMU");
    gtk_window_set_resizable(GTK_WINDOW(c->window), FALSE);

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_container_add(GTK_CONTAINER(c->window), vbox);

    gemu_gtk_add_action_menu(vbox, mon);

    c->drawing_area = gtk_drawing_area_new();
    gtk_widget_set_size_request(c->drawing_area,
                                RP2C02_WIDTH * scale, RP2C02_HEIGHT * scale);
    gtk_box_pack_start(GTK_BOX(vbox), c->drawing_area, TRUE, TRUE, 0);

    gtk_widget_add_events(c->window, GDK_KEY_PRESS_MASK | GDK_KEY_RELEASE_MASK);
    g_signal_connect(c->window,       "delete-event",      G_CALLBACK(on_delete), c);
    g_signal_connect(c->window,       "key-press-event",   G_CALLBACK(on_key),    c);
    g_signal_connect(c->window,       "key-release-event", G_CALLBACK(on_key),    c);
    g_signal_connect(c->drawing_area, "draw",              G_CALLBACK(on_draw),   c);

    gtk_widget_show_all(c->window);
    while (gtk_events_pending()) gtk_main_iteration_do(FALSE);

    NesDisplay *d = calloc(1, sizeof(*d));
    if (!d) { gtk_destroy(c); return NULL; }
    d->render      = gtk_render;
    d->poll        = gtk_poll;
    d->destroy     = gtk_destroy;
    d->should_quit = gtk_should_quit;
    d->ctrl1       = gtk_ctrl1;
    d->ctx         = c;
    return d;
}
