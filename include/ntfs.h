#ifndef RAWREADER_NTFS_H
#define RAWREADER_NTFS_H

#include "block_device.h"

typedef struct _NtfsVolume NtfsVolume;

typedef struct {
    gchar *name;
    gboolean is_directory;
    guint64 size;
} NtfsDirectoryEntry;

typedef struct {
    guint32 bytes_per_sector;
    guint32 sectors_per_cluster;
    guint64 cluster_count;
    guint64 mft_start_cluster;
    guint64 volume_size;
    guint64 partition_offset;
} NtfsInfo;

NtfsVolume *ntfs_volume_open(BlockDevice *device, GError **error);
void ntfs_volume_close(NtfsVolume *volume);
NtfsInfo ntfs_volume_get_info(const NtfsVolume *volume);
GPtrArray *ntfs_list_root(NtfsVolume *volume, GError **error);
void ntfs_directory_entry_free(gpointer data);

#endif