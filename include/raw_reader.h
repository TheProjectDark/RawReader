#ifndef RAWREADER_RAW_READER_H
#define RAWREADER_RAW_READER_H

#include <gio/gio.h>

GBytes *raw_reader_read(const gchar *device_path, guint64 offset, gsize length,
                        GError **error);

#endif
