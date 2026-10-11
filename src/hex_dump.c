#include "hex_dump.h"

gchar *hex_dump_format(const guint8 *data, gsize length, guint64 base_offset)
{
    GString *dump = g_string_new(NULL);

    for (gsize row = 0; row < length; row += 16) {
        gsize row_length = MIN((gsize)16, length - row);
        g_string_append_printf(dump, "%016llx  ",
                               (unsigned long long)(base_offset + row));

        for (gsize column = 0; column < 16; ++column) {
            if (column < row_length) {
                g_string_append_printf(dump, "%02x ", data[row + column]);
            } else {
                g_string_append(dump, "   ");
            }
            if (column == 7) {
                g_string_append_c(dump, ' ');
            }
        }

        g_string_append(dump, " |");
        for (gsize column = 0; column < row_length; ++column) {
            guint8 byte = data[row + column];
            g_string_append_c(dump, g_ascii_isprint(byte) ? (gchar)byte : '.');
        }
        g_string_append(dump, "|\n");
    }

    return g_string_free(dump, FALSE);
}
