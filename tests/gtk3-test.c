#include <gtk/gtk.h>
#include <stdio.h>

int main(int argc, char **argv)
{
    if (!gtk_init_check(&argc, &argv)) {
        fprintf(stderr, "gtk_init failed (no display?)\n");
        return 1;
    }
    printf("GTK %u.%u.%u\n", gtk_get_major_version(), gtk_get_minor_version(),
           gtk_get_micro_version());
    GtkWidget *w = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(w), "cross-compiled");
    gtk_container_add(GTK_CONTAINER(w), gtk_label_new("Hello from a cross-compiled GTK 3 app"));
    g_signal_connect(w, "destroy", G_CALLBACK(gtk_main_quit), NULL);
    g_timeout_add_seconds(2, (GSourceFunc)gtk_main_quit, NULL);
    gtk_widget_show_all(w);
    gtk_main();
    return 0;
}
