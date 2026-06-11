#ifdef GEMU_GTK
#include "gemu/gtk_menu.h"

static void on_menu_reset(GtkMenuItem *item, gpointer mon) {
    (void)item;
    gemu_monitor_enqueue_reset((GemuMonitor *)mon);
}

static void on_menu_quit(GtkMenuItem *item, gpointer mon) {
    (void)item;
    gemu_monitor_enqueue_quit((GemuMonitor *)mon);
}

void gemu_gtk_add_action_menu(GtkWidget *vbox, GemuMonitor *mon) {
    GtkWidget *menubar     = gtk_menu_bar_new();
    GtkWidget *action_menu = gtk_menu_new();
    GtkWidget *action_item = gtk_menu_item_new_with_label("Action");
    GtkWidget *reset_item  = gtk_menu_item_new_with_label("Reset");
    GtkWidget *quit_item   = gtk_menu_item_new_with_label("Quit");

    gtk_menu_item_set_submenu(GTK_MENU_ITEM(action_item), action_menu);
    gtk_menu_shell_append(GTK_MENU_SHELL(action_menu), reset_item);
    gtk_menu_shell_append(GTK_MENU_SHELL(action_menu), quit_item);
    gtk_menu_shell_append(GTK_MENU_SHELL(menubar), action_item);
    gtk_box_pack_start(GTK_BOX(vbox), menubar, FALSE, FALSE, 0);

    g_signal_connect(reset_item, "activate", G_CALLBACK(on_menu_reset), mon);
    g_signal_connect(quit_item,  "activate", G_CALLBACK(on_menu_quit),  mon);
}
#endif /* GEMU_GTK */
