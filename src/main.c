#include <gtk/gtk.h>

#include "app.h"

int main(int argc, char **argv)
{
    GtkApplication *application =
        gtk_application_new("com.rawreader.app", G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(application, "activate", G_CALLBACK(rawreader_app_activate), NULL);

    int status = g_application_run(G_APPLICATION(application), argc, argv);
    g_object_unref(application);
    return status;
}
