#ifdef GEMU_GTK
#include "gemu/video.h"
#include "gemu/gtk_menu.h"
#include <stdlib.h>

struct GemuVideoGtk {
    GtkWidget       *window;
    GtkWidget       *drawing_area;
    cairo_surface_t *surface;
    uint32_t        *frame_argb;
    const uint32_t  *palette;
    int              n_colors;
    int              width;
    int              height;
    int              scale;
    bool             active;
};

static gboolean on_draw(GtkWidget *w, cairo_t *cr, gpointer data) {
    (void)w;
    GemuVideoGtk *v = data;
    if (!v->active) {
        cairo_set_source_rgb(cr, 0, 0, 0);
        cairo_paint(cr);
        return FALSE;
    }

    cairo_save(cr);
    cairo_scale(cr, v->scale, v->scale);
    cairo_set_source_surface(cr, v->surface, 0, 0);
    cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_NEAREST);
    cairo_paint(cr);
    cairo_restore(cr);
    return FALSE;
}

static void mark_presented(GemuVideoGtk *v) {
    cairo_surface_mark_dirty(v->surface);
    v->active = true;
    gtk_widget_queue_draw(v->drawing_area);
    gemu_video_gtk_poll();
}

GemuVideoGtk *gemu_video_gtk_create(const GemuVideoGtkSpec *spec) {
    if (!spec || spec->width <= 0 || spec->height <= 0) return NULL;
    if (!gtk_init_check(NULL, NULL)) return NULL;

    GemuVideoGtk *v = calloc(1, sizeof(*v));
    if (!v) return NULL;

    v->width    = spec->width;
    v->height   = spec->height;
    v->scale    = spec->scale > 0 ? spec->scale : 1;
    v->palette  = spec->palette;
    v->n_colors = spec->n_colors;
    v->surface  = cairo_image_surface_create(CAIRO_FORMAT_ARGB32,
                                             v->width, v->height);
    v->frame_argb = malloc((size_t)v->width * (size_t)v->height *
                           sizeof(*v->frame_argb));
    if (!v->surface || cairo_surface_status(v->surface) != CAIRO_STATUS_SUCCESS ||
        !v->frame_argb) {
        gemu_video_gtk_destroy(v);
        return NULL;
    }

    v->window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(v->window), spec->title ? spec->title : "GEMU");
    gtk_window_set_resizable(GTK_WINDOW(v->window), FALSE);

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_container_add(GTK_CONTAINER(v->window), vbox);
    gemu_gtk_add_action_menu(vbox, spec->monitor);

    v->drawing_area = gtk_drawing_area_new();
    gtk_widget_set_size_request(v->drawing_area,
                                v->width * v->scale, v->height * v->scale);
    gtk_box_pack_start(GTK_BOX(vbox), v->drawing_area, TRUE, TRUE, 0);
    g_signal_connect(v->drawing_area, "draw", G_CALLBACK(on_draw), v);

    gtk_widget_show_all(v->window);
    gemu_video_gtk_poll();
    return v;
}

void gemu_video_gtk_destroy(GemuVideoGtk *v) {
    if (!v) return;
    if (v->surface) cairo_surface_destroy(v->surface);
    if (v->window) gtk_widget_destroy(v->window);
    free(v->frame_argb);
    free(v);
}

void gemu_video_gtk_present_argb(GemuVideoGtk *v, const uint32_t *pixels,
                                 int w, int h) {
    if (!v || !pixels || w != v->width || h != v->height) return;
    uint32_t *out = (uint32_t *)cairo_image_surface_get_data(v->surface);
    int stride = cairo_image_surface_get_stride(v->surface) / 4;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
            out[y * stride + x] = pixels[y * w + x];
    mark_presented(v);
}

void gemu_video_gtk_present_indexed(GemuVideoGtk *v, const uint8_t *pixels,
                                    int w, int h) {
    if (!v || !pixels || w != v->width || h != v->height || !v->palette)
        return;
    for (int i = 0; i < w * h; i++) {
        uint8_t idx = pixels[i];
        if (v->n_colors > 0 && idx >= v->n_colors)
            idx = (uint8_t)(v->n_colors - 1);
        v->frame_argb[i] = v->palette[idx];
    }
    gemu_video_gtk_present_argb(v, v->frame_argb, w, h);
}

void gemu_video_gtk_present_mono(GemuVideoGtk *v, const uint8_t *pixels,
                                 int w, int h, uint32_t on, uint32_t off) {
    if (!v || !pixels || w != v->width || h != v->height) return;
    for (int i = 0; i < w * h; i++)
        v->frame_argb[i] = pixels[i] ? on : off;
    gemu_video_gtk_present_argb(v, v->frame_argb, w, h);
}

void gemu_video_gtk_poll(void) {
    while (gtk_events_pending())
        gtk_main_iteration_do(FALSE);
}

GtkWidget *gemu_video_gtk_window(GemuVideoGtk *v) {
    return v ? v->window : NULL;
}

GtkWidget *gemu_video_gtk_drawing_area(GemuVideoGtk *v) {
    return v ? v->drawing_area : NULL;
}
#endif /* GEMU_GTK */
