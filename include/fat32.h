#ifndef RAWREADER_FAT32_H
#define RAWREADER_FAT32_H

#include "block_device.h"

typedef struct _Fat32Volume Fat32Volume;

typedef struct {
    gchar *name;
    gboolean is_directory;
    guint32 size;
    guint32 first_cluster;
} Fat32DirectoryEntry;

typedef struct {
    guint32 bytes_per_sector;
    guint32 sectors_per_cluster;
    guint32 cluster_count;
    guint32 root_cluster;
    guint64 volume_size;
    guint64 partition_offset;
} Fat32Info;

Fat32Volume *fat32_volume_open(BlockDevice *device, GError **error);
void fat32_volume_close(Fat32Volume *volume);
Fat32Info fat32_volume_get_info(const Fat32Volume *volume);
GPtrArray *fat32_list_root(Fat32Volume *volume, GError **error);
GPtrArray *fat32_list_directory(Fat32Volume *volume, guint32 cluster,
                                GError **error);
GBytes *fat32_read_file(Fat32Volume *volume, guint32 first_cluster,
                        guint64 file_size, guint64 offset, gsize length,
                        GError **error);
void fat32_directory_entry_free(gpointer data);

#endif
