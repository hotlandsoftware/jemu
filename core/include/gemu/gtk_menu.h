#pragma once
#ifdef GEMU_GTK
#include <gtk/gtk.h>
#include "gemu/monitor.h"

/* Append the standard "Action" menu bar (Reset, Quit) to vbox.
 * Both items enqueue commands into mon so the emulator's run loop handles them. */
void gemu_gtk_add_action_menu(GtkWidget *vbox, GemuMonitor *mon);

#endif /* GEMU_GTK */
