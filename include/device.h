#ifndef RAWREADER_DEVICE_H
#define RAWREADER_DEVICE_H

#include <gio/gio.h>

typedef struct {
    gchar *path;
    gchar *label;
} RawDevice;

GPtrArray *device_list_discover(GError **error);
void raw_device_free(gpointer data);

#endif
