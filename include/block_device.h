#ifndef RAWREADER_BLOCK_DEVICE_H
#define RAWREADER_BLOCK_DEVICE_H

#include <gio/gio.h>

typedef struct _BlockDevice BlockDevice;

BlockDevice *block_device_open(const gchar *path, GError **error);
void block_device_close(BlockDevice *device);
guint64 block_device_get_size(const BlockDevice *device);
guint32 block_device_get_sector_size(const BlockDevice *device);
GBytes *block_device_read(BlockDevice *device, guint64 offset, gsize length,
                          GError **error);

#endif
