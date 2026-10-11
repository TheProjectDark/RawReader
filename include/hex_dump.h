#ifndef RAWREADER_HEX_DUMP_H
#define RAWREADER_HEX_DUMP_H

#include <glib.h>

gchar *hex_dump_format(const guint8 *data, gsize length, guint64 base_offset);

#endif
