#ifndef RAWREADER_EXT4_H
#define RAWREADER_EXT4_H

#include "block_device.h"

typedef struct _Ext4Volume Ext4Volume;

typedef struct {
    gchar *name;
    gboolean is_directory;
    guint64 size;
    guint32 inode_number;
} Ext4DirectoryEntry;

typedef struct {
    guint32 block_size;
    guint32 blocks_per_group;
    guint32 inodes_per_group;
    guint64 block_count;
    guint32 inode_count;
    guint64 volume_size;
    guint64 partition_offset;
} Ext4Info;

Ext4Volume *ext4_volume_open(BlockDevice *device, GError **error);
void ext4_volume_close(Ext4Volume *volume);
Ext4Info ext4_volume_get_info(const Ext4Volume *volume);
GPtrArray *ext4_list_root(Ext4Volume *volume, GError **error);
GPtrArray *ext4_list_directory(Ext4Volume *volume, guint32 inode_number,
                               GError **error);
GBytes *ext4_read_file(Ext4Volume *volume, guint32 inode_number,
                       guint64 offset, gsize length, GError **error);
void ext4_directory_entry_free(gpointer data);

#endif