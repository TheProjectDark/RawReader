#include "ext4.h"

#include <string.h>

#define EXT4_MAX_ROOT_ENTRIES 4096
#define EXT4_MAX_DIRECTORY_SIZE (64 * 1024 * 1024)
#define EXT4_MAX_EXTENT_NODES 4096
#define EXT4_MAX_EXTENT_DEPTH 5
#define EXT4_READ_CHUNK (256 * 1024)

#define EXT4_SUPERBLOCK_OFFSET 1024
#define EXT4_SUPERBLOCK_SIZE 1024
#define EXT4_MAGIC 0xEF53
#define EXT4_EXTENT_MAGIC 0xF30A
#define EXT4_INODE_READ_SIZE 128
#define EXT4_ROOT_INODE 2


#define EXT4_INCOMPAT_FILETYPE 0x00002u
#define EXT4_INCOMPAT_RECOVER 0x00004u
#define EXT4_INCOMPAT_META_BG 0x00010u
#define EXT4_INCOMPAT_EXTENTS 0x00040u
#define EXT4_INCOMPAT_64BIT 0x00080u
#define EXT4_INCOMPAT_MMP 0x00100u
#define EXT4_INCOMPAT_FLEX_BG 0x00200u
#define EXT4_INCOMPAT_EA_INODE 0x00400u
#define EXT4_INCOMPAT_CSUM_SEED 0x02000u
#define EXT4_INCOMPAT_LARGEDIR 0x04000u
#define EXT4_INCOMPAT_INLINE_DATA 0x08000u
#define EXT4_INCOMPAT_ENCRYPT 0x10000u
#define EXT4_INCOMPAT_CASEFOLD 0x20000u



#define EXT4_INCOMPAT_SUPPORTED \
    (EXT4_INCOMPAT_FILETYPE | EXT4_INCOMPAT_RECOVER | EXT4_INCOMPAT_META_BG \
     | EXT4_INCOMPAT_EXTENTS | EXT4_INCOMPAT_64BIT | EXT4_INCOMPAT_MMP \
     | EXT4_INCOMPAT_FLEX_BG | EXT4_INCOMPAT_EA_INODE | EXT4_INCOMPAT_CSUM_SEED \
     | EXT4_INCOMPAT_LARGEDIR | EXT4_INCOMPAT_INLINE_DATA | EXT4_INCOMPAT_ENCRYPT \
     | EXT4_INCOMPAT_CASEFOLD)

#define EXT4_COMPAT_SPARSE_SUPER2 0x0200u
#define EXT4_RO_COMPAT_SPARSE_SUPER 0x0001u


#define EXT4_FLAG_ENCRYPT 0x00000800u
#define EXT4_FLAG_EXTENTS 0x00080000u
#define EXT4_FLAG_INLINE_DATA 0x10000000u

#define EXT4_MODE_TYPE_MASK 0xF000
#define EXT4_MODE_DIRECTORY 0x4000
#define EXT4_MODE_REGULAR 0x8000

struct _Ext4Volume {
    BlockDevice *device;
    guint64 partition_offset;
    guint64 volume_size;
    guint64 block_count;
    guint32 block_size;
    guint32 first_data_block;
    guint32 blocks_per_group;
    guint32 inodes_per_group;
    guint32 inode_count;
    guint32 inode_size;
    guint32 group_count;
    guint32 descriptor_size;
    guint32 descriptors_per_block;
    guint32 first_meta_bg;
    guint32 backup_groups[2];
    guint32 feature_compat;
    guint32 feature_incompat;
    guint32 feature_ro_compat;
    guint8 *descriptor_cache;
    guint64 descriptor_cache_block;
};

typedef struct {
    Ext4Volume *volume;
    GPtrArray *entries;
    guint64 dir_blocks;
    guint64 next_logical;
    guint nodes_visited;
} Ext4DirWalk;

static guint16 read_le16(const guint8 *bytes)
{
    return (guint16)bytes[0] | ((guint16)bytes[1] << 8);
}

static guint32 read_le32(const guint8 *bytes)
{
    return (guint32)bytes[0] | ((guint32)bytes[1] << 8)
        | ((guint32)bytes[2] << 16) | ((guint32)bytes[3] << 24);
}

static gboolean is_power_of_two(guint64 value)
{
    return value != 0 && (value & (value - 1)) == 0;
}

static void ext4_set_invalid(GError **error, const gchar *message)
{
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, message);
}





static guint8 *ext4_read_bytes(BlockDevice *device, guint64 offset, gsize length,
                               GError **error)
{
    guint64 alignment = MAX((guint64)512, (guint64)block_device_get_sector_size(device));
    guint64 device_size = block_device_get_size(device);
    if (length == 0 || offset > device_size || (guint64)length > device_size - offset) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                            "The requested read is outside the device.");
        return NULL;
    }

    guint64 start = offset - offset % alignment;
    guint64 end = ((offset + length + alignment - 1) / alignment) * alignment;
    if (end > device_size) {
        end = device_size;
    }

    GBytes *bytes = block_device_read(device, start, (gsize)(end - start), error);
    if (bytes == NULL) {
        return NULL;
    }
    guint8 *buffer = g_malloc(length);
    memcpy(buffer, (const guint8 *)g_bytes_get_data(bytes, NULL) + (offset - start), length);
    g_bytes_unref(bytes);
    return buffer;
}

static gboolean ext4_block_in_range(const Ext4Volume *volume, guint64 block, guint64 count)
{
    return block >= volume->first_data_block && block < volume->block_count
        && count <= volume->block_count - block;
}

static guint8 *ext4_read_block(Ext4Volume *volume, guint64 block, GError **error)
{
    if (!ext4_block_in_range(volume, block, 1)) {
        ext4_set_invalid(error, "An ext4 block number lies outside the volume.");
        return NULL;
    }
    return ext4_read_bytes(volume->device,
                           volume->partition_offset + block * volume->block_size,
                           volume->block_size, error);
}



static gboolean ext4_is_power_of(guint64 value, guint64 base)
{
    for (;;) {
        if (value < base) {
            return FALSE;
        }
        if (value == base) {
            return TRUE;
        }
        if (value % base != 0) {
            return FALSE;
        }
        value /= base;
    }
}

static gboolean ext4_group_has_super(const Ext4Volume *volume, guint64 group)
{
    if (group == 0) {
        return TRUE;
    }
    if ((volume->feature_compat & EXT4_COMPAT_SPARSE_SUPER2) != 0) {
        return group == volume->backup_groups[0] || group == volume->backup_groups[1];
    }
    if (group <= 1 || (volume->feature_ro_compat & EXT4_RO_COMPAT_SPARSE_SUPER) == 0) {
        return TRUE;
    }
    if ((group & 1) == 0) {
        return FALSE;
    }
    return ext4_is_power_of(group, 3) || ext4_is_power_of(group, 5)
        || ext4_is_power_of(group, 7);
}

static gboolean ext4_group_inode_table(Ext4Volume *volume, guint32 group,
                                       guint64 *inode_table, GError **error)
{
    guint32 descriptor_block_index = group / volume->descriptors_per_block;
    guint32 index = group % volume->descriptors_per_block;
    guint64 block;

    if ((volume->feature_incompat & EXT4_INCOMPAT_META_BG) == 0
        || descriptor_block_index < volume->first_meta_bg) {
        block = (guint64)volume->first_data_block + descriptor_block_index + 1;
    } else {
        guint64 first_group = (guint64)descriptor_block_index * volume->descriptors_per_block;
        guint64 has_super = ext4_group_has_super(volume, first_group) ? 1 : 0;
        if (volume->block_size == 1024 && descriptor_block_index == 0
            && volume->first_data_block == 0) {
            ++has_super;
        }
        block = (guint64)volume->first_data_block
            + first_group * volume->blocks_per_group + has_super;
    }

    if (volume->descriptor_cache == NULL || volume->descriptor_cache_block != block) {
        guint8 *fresh = ext4_read_block(volume, block, error);
        if (fresh == NULL) {
            return FALSE;
        }
        g_free(volume->descriptor_cache);
        volume->descriptor_cache = fresh;
        volume->descriptor_cache_block = block;
    }

    const guint8 *descriptor = volume->descriptor_cache + (gsize)index * volume->descriptor_size;
    guint64 table = read_le32(descriptor + 0x08);
    if (volume->descriptor_size >= 64) {
        table |= (guint64)read_le32(descriptor + 0x28) << 32;
    }
    if (table < volume->first_data_block || table >= volume->block_count) {
        ext4_set_invalid(error, "An ext4 group descriptor has an invalid inode-table location.");
        return FALSE;
    }
    *inode_table = table;
    return TRUE;
}


static guint8 *ext4_read_inode(Ext4Volume *volume, guint32 number, GError **error)
{
    if (number == 0 || number > volume->inode_count) {
        ext4_set_invalid(error, "An ext4 inode number is out of range.");
        return NULL;
    }

    guint32 group = (number - 1) / volume->inodes_per_group;
    guint32 index = (number - 1) % volume->inodes_per_group;
    guint64 table = 0;
    if (group >= volume->group_count) {
        ext4_set_invalid(error, "An ext4 inode lies in a nonexistent block group.");
        return NULL;
    }
    if (!ext4_group_inode_table(volume, group, &table, error)) {
        return NULL;
    }

    guint64 offset = table * volume->block_size + (guint64)index * volume->inode_size;
    if (offset > volume->volume_size
        || volume->inode_size > volume->volume_size - offset) {
        ext4_set_invalid(error, "An ext4 inode lies outside the volume.");
        return NULL;
    }
    return ext4_read_bytes(volume->device, volume->partition_offset + offset,
                           EXT4_INODE_READ_SIZE, error);
}

static guint64 ext4_inode_file_size(const Ext4Volume *volume, const guint8 *inode)
{
    guint64 size = read_le32(inode + 0x04);
    guint16 mode = read_le16(inode);
    if ((volume->feature_incompat & EXT4_INCOMPAT_LARGEDIR) != 0
        || (mode & EXT4_MODE_TYPE_MASK) == EXT4_MODE_REGULAR) {
        size |= (guint64)read_le32(inode + 0x6C) << 32;
    }
    return size;
}



static gboolean ext4_parse_at(BlockDevice *device, guint64 partition_offset,
                              guint64 partition_size, Ext4Volume **result,
                              gboolean *recognized, GError **error)
{
    guint8 *sb = ext4_read_bytes(device, partition_offset + EXT4_SUPERBLOCK_OFFSET,
                                 EXT4_SUPERBLOCK_SIZE, error);
    if (sb == NULL) {
        return FALSE;
    }
    if (read_le16(sb + 0x38) != EXT4_MAGIC) {
        g_free(sb);
        ext4_set_invalid(error, "The partition does not contain an ext2/3/4 superblock.");
        return FALSE;
    }
    *recognized = TRUE;

    guint32 log_block_size = read_le32(sb + 0x18);
    guint32 revision = read_le32(sb + 0x4C);
    guint32 incompat = read_le32(sb + 0x60);
    if (log_block_size > 6 || revision > 1) {
        g_free(sb);
        ext4_set_invalid(error, "The ext4 superblock has an unsupported block size or revision.");
        return FALSE;
    }
    if ((incompat & ~EXT4_INCOMPAT_SUPPORTED) != 0) {
        guint32 unsupported = incompat & ~EXT4_INCOMPAT_SUPPORTED;
        g_free(sb);
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                    "The ext4 volume uses unsupported incompatible features (0x%x).",
                    unsupported);
        return FALSE;
    }

    Ext4Volume *volume = g_new0(Ext4Volume, 1);
    volume->device = device;
    volume->partition_offset = partition_offset;
    volume->block_size = 1024u << log_block_size;
    volume->inode_count = read_le32(sb + 0x00);
    volume->first_data_block = read_le32(sb + 0x14);
    volume->blocks_per_group = read_le32(sb + 0x20);
    volume->inodes_per_group = read_le32(sb + 0x28);
    volume->inode_size = revision == 0 ? 128 : read_le16(sb + 0x58);
    volume->feature_compat = revision == 0 ? 0 : read_le32(sb + 0x5C);
    volume->feature_incompat = revision == 0 ? 0 : incompat;
    volume->feature_ro_compat = revision == 0 ? 0 : read_le32(sb + 0x64);
    volume->first_meta_bg = read_le32(sb + 0x104);
    volume->backup_groups[0] = read_le32(sb + 0x24C);
    volume->backup_groups[1] = read_le32(sb + 0x250);
    volume->descriptor_size = (volume->feature_incompat & EXT4_INCOMPAT_64BIT) != 0
        ? read_le16(sb + 0xFE) : 32;
    volume->block_count = read_le32(sb + 0x04);
    if ((volume->feature_incompat & EXT4_INCOMPAT_64BIT) != 0) {
        volume->block_count |= (guint64)read_le32(sb + 0x150) << 32;
    }
    g_free(sb);

    guint32 block_size = volume->block_size;
    if (volume->block_count == 0 || volume->first_data_block >= volume->block_count
        || volume->blocks_per_group == 0
        || volume->inodes_per_group < 2 || volume->inodes_per_group > block_size * 8
        || volume->inode_count < 2
        || volume->inode_size < 128 || volume->inode_size > block_size
        || !is_power_of_two(volume->inode_size)
        || volume->descriptor_size < 32 || volume->descriptor_size > 1024
        || !is_power_of_two(volume->descriptor_size)
        || ((volume->feature_incompat & EXT4_INCOMPAT_64BIT) != 0
            && volume->descriptor_size < 64)) {
        ext4_set_invalid(error, "The ext4 superblock describes an invalid geometry.");
        goto fail;
    }
    volume->descriptors_per_block = block_size / volume->descriptor_size;

    if (volume->block_count > G_MAXUINT64 / block_size) {
        ext4_set_invalid(error, "The ext4 superblock has an invalid block count.");
        goto fail;
    }
    volume->volume_size = volume->block_count * block_size;
    guint64 device_size = block_device_get_size(device);
    if (partition_offset >= device_size
        || volume->volume_size > device_size - partition_offset
        || (partition_size != 0 && volume->volume_size > partition_size)) {
        ext4_set_invalid(error, "The ext4 volume extends beyond the device partition.");
        goto fail;
    }

    guint64 groups = (volume->block_count - volume->first_data_block
                      + volume->blocks_per_group - 1) / volume->blocks_per_group;
    if (groups == 0 || groups > G_MAXUINT32
        || (guint64)volume->inode_count > groups * volume->inodes_per_group) {
        ext4_set_invalid(error, "The ext4 superblock has an inconsistent group count.");
        goto fail;
    }
    volume->group_count = (guint32)groups;


    guint8 *root = ext4_read_inode(volume, EXT4_ROOT_INODE, error);
    if (root == NULL) {
        goto fail;
    }
    guint16 root_mode = read_le16(root);
    g_free(root);
    if ((root_mode & EXT4_MODE_TYPE_MASK) != EXT4_MODE_DIRECTORY) {
        ext4_set_invalid(error, "The ext4 root inode is not a directory.");
        goto fail;
    }

    *result = volume;
    return TRUE;

fail:
    ext4_volume_close(volume);
    return FALSE;
}

Ext4Volume *ext4_volume_open(BlockDevice *device, GError **error)
{
    if (device == NULL) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                            "A block device is required.");
        return NULL;
    }

    GError *superfloppy_error = NULL;
    Ext4Volume *volume = NULL;
    gboolean recognized = FALSE;
    if (ext4_parse_at(device, 0, 0, &volume, &recognized, &superfloppy_error)) {
        return volume;
    }
    if (recognized) {

        g_propagate_error(error, superfloppy_error);
        return NULL;
    }

    GError *mbr_read_error = NULL;
    guint8 *mbr = ext4_read_bytes(device, 0, 512, &mbr_read_error);
    if (mbr == NULL || mbr[510] != 0x55 || mbr[511] != 0xAA) {
        g_free(mbr);
        g_clear_error(&mbr_read_error);
        g_propagate_prefixed_error(error, superfloppy_error, "No ext4 volume found: ");
        return NULL;
    }

    guint32 sector_size = block_device_get_sector_size(device);
    guint64 device_size = block_device_get_size(device);
    GError *partition_error = NULL;
    for (guint index = 0; index < 4; ++index) {
        const guint8 *entry = mbr + 446 + index * 16;
        if (entry[4] != 0x83) {  
            continue;
        }

        guint64 start = (guint64)read_le32(entry + 8) * sector_size;
        guint64 size = (guint64)read_le32(entry + 12) * sector_size;
        if (start >= device_size || size == 0 || size > device_size - start) {
            g_clear_error(&partition_error);
            g_set_error_literal(&partition_error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                                "An MBR Linux partition has invalid bounds.");
            continue;
        }

        GError *candidate_error = NULL;
        gboolean candidate_recognized = FALSE;
        if (ext4_parse_at(device, start, size, &volume, &candidate_recognized,
                          &candidate_error)) {
            g_free(mbr);
            g_clear_error(&superfloppy_error);
            g_clear_error(&partition_error);
            return volume;
        }
        g_clear_error(&partition_error);
        partition_error = candidate_error;
    }

    g_free(mbr);
    if (partition_error != NULL) {
        g_clear_error(&superfloppy_error);
        g_propagate_error(error, partition_error);
    } else {
        g_propagate_prefixed_error(error, superfloppy_error, "No ext4 volume found: ");
    }
    return NULL;
}

void ext4_volume_close(Ext4Volume *volume)
{
    if (volume == NULL) {
        return;
    }
    g_free(volume->descriptor_cache);
    g_free(volume);
}

Ext4Info ext4_volume_get_info(const Ext4Volume *volume)
{
    Ext4Info info = {
        .block_size = volume->block_size,
        .blocks_per_group = volume->blocks_per_group,
        .inodes_per_group = volume->inodes_per_group,
        .block_count = volume->block_count,
        .inode_count = volume->inode_count,
        .volume_size = volume->volume_size,
        .partition_offset = volume->partition_offset
    };
    return info;
}

void ext4_directory_entry_free(gpointer data)
{
    Ext4DirectoryEntry *entry = data;
    if (entry != NULL) {
        g_free(entry->name);
        g_free(entry);
    }
}



static guint32 ext4_rec_len(guint32 block_size, guint32 raw)
{
    if (block_size < 65536) {
        return raw;
    }
    if (raw == 65535 || raw == 0) {
        return block_size;
    }
    return (raw & 65532) | ((raw & 3) << 16);
}


static gboolean ext4_append_entry(Ext4DirWalk *walk, const guint8 *name,
                                  guint32 name_length, guint32 inode_number,
                                  GError **error)
{
    if (walk->entries->len >= EXT4_MAX_ROOT_ENTRIES) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_TOO_MANY_OPEN_FILES,
                    "The root directory exceeds the limit of %u entries.",
                    EXT4_MAX_ROOT_ENTRIES);
        return FALSE;
    }

    guint8 *inode = ext4_read_inode(walk->volume, inode_number, error);
    if (inode == NULL) {
        return FALSE;
    }

    guint16 mode = read_le16(inode) & EXT4_MODE_TYPE_MASK;
    if (mode != EXT4_MODE_DIRECTORY && mode != EXT4_MODE_REGULAR) {
        g_free(inode);
        return TRUE;
    }

    Ext4DirectoryEntry *item = g_new0(Ext4DirectoryEntry, 1);
    item->name = g_utf8_make_valid((const gchar *)name, name_length);
    item->is_directory = mode == EXT4_MODE_DIRECTORY;
    item->size = item->is_directory ? 0 : ext4_inode_file_size(walk->volume, inode);
    item->inode_number = inode_number;
    g_free(inode);
    g_ptr_array_add(walk->entries, item);
    return TRUE;
}

static gboolean ext4_parse_dir_block(Ext4DirWalk *walk, const guint8 *data, GError **error)
{
    guint32 block_size = walk->volume->block_size;
    guint32 position = 0;

    while (position + 8 <= block_size) {
        const guint8 *entry = data + position;
        guint32 inode_number = read_le32(entry);
        guint32 record_length = ext4_rec_len(block_size, read_le16(entry + 4));
        guint32 name_length = entry[6];

        if (record_length < 8 || (record_length & 3) != 0
            || record_length > block_size - position
            || (inode_number != 0 && 8 + name_length > record_length)) {
            ext4_set_invalid(error, "An ext4 directory entry is corrupt.");
            return FALSE;
        }


        if (inode_number != 0 && name_length != 0
            && !(name_length == 1 && entry[8] == '.')
            && !(name_length == 2 && entry[8] == '.' && entry[9] == '.')
            && !ext4_append_entry(walk, entry + 8, name_length, inode_number, error)) {
            return FALSE;
        }
        position += record_length;
    }

    if (position != block_size) {
        ext4_set_invalid(error, "An ext4 directory block is truncated.");
        return FALSE;
    }
    return TRUE;
}


static gboolean ext4_process_blocks(Ext4DirWalk *walk, guint64 start, guint64 count,
                                    GError **error)
{
    Ext4Volume *volume = walk->volume;
    guint64 chunk_blocks = MAX((guint64)1, (guint64)EXT4_READ_CHUNK / volume->block_size);

    if (!ext4_block_in_range(volume, start, count)) {
        ext4_set_invalid(error, "An ext4 directory extent lies outside the volume.");
        return FALSE;
    }

    while (count > 0) {
        guint64 blocks = MIN(count, chunk_blocks);
        guint8 *buffer = ext4_read_bytes(volume->device,
                                         volume->partition_offset + start * volume->block_size,
                                         (gsize)(blocks * volume->block_size), error);
        if (buffer == NULL) {
            return FALSE;
        }
        for (guint64 i = 0; i < blocks; ++i) {
            if (!ext4_parse_dir_block(walk, buffer + i * volume->block_size, error)) {
                g_free(buffer);
                return FALSE;
            }
        }
        g_free(buffer);
        start += blocks;
        count -= blocks;
        walk->next_logical += blocks;
    }
    return TRUE;
}



static gboolean ext4_walk_extent_node(Ext4DirWalk *walk, const guint8 *data,
                                      gsize data_size, gint expected_depth,
                                      GError **error)
{
    Ext4Volume *volume = walk->volume;

    if (data_size < 12 || read_le16(data) != EXT4_EXTENT_MAGIC) {
        ext4_set_invalid(error, "An ext4 extent node has a bad signature.");
        return FALSE;
    }
    guint32 entries = read_le16(data + 2);
    guint32 maximum = read_le16(data + 4);
    guint32 depth = read_le16(data + 6);
    if (depth > EXT4_MAX_EXTENT_DEPTH
        || (expected_depth >= 0 && depth != (guint32)expected_depth)
        || entries > maximum || 12 + (gsize)entries * 12 > data_size) {
        ext4_set_invalid(error, "An ext4 extent node header is invalid.");
        return FALSE;
    }

    for (guint32 i = 0; i < entries && walk->next_logical < walk->dir_blocks; ++i) {
        const guint8 *entry = data + 12 + (gsize)i * 12;

        if (depth == 0) {
            guint32 logical = read_le32(entry);
            guint32 raw_length = read_le16(entry + 4);
            guint64 start = ((guint64)read_le16(entry + 6) << 32) | read_le32(entry + 8);

            if (raw_length > 32768) {
                ext4_set_invalid(error, "An ext4 directory contains an unwritten extent.");
                return FALSE;
            }
            if (raw_length == 0 || logical != walk->next_logical) {
                ext4_set_invalid(error, "An ext4 directory has a hole or unordered extents.");
                return FALSE;
            }
            guint64 count = MIN((guint64)raw_length, walk->dir_blocks - logical);
            if (!ext4_process_blocks(walk, start, count, error)) {
                return FALSE;
            }
        } else {
            guint64 child = ((guint64)read_le16(entry + 8) << 32) | read_le32(entry + 4);
            if (++walk->nodes_visited > EXT4_MAX_EXTENT_NODES) {
                ext4_set_invalid(error, "An ext4 extent tree is too large.");
                return FALSE;
            }
            guint8 *block = ext4_read_block(volume, child, error);
            if (block == NULL) {
                return FALSE;
            }
            gboolean ok = ext4_walk_extent_node(walk, block, volume->block_size,
                                                (gint)depth - 1, error);
            g_free(block);
            if (!ok) {
                return FALSE;
            }
        }
    }
    return TRUE;
}


static gboolean ext4_process_mapped_block(Ext4DirWalk *walk, guint32 block, GError **error)
{
    if (block == 0) {
        ext4_set_invalid(error, "An ext2/3 directory has a hole.");
        return FALSE;
    }
    return ext4_process_blocks(walk, block, 1, error);
}

static gboolean ext4_walk_indirect(Ext4DirWalk *walk, guint32 block, guint level,
                                   GError **error)
{
    if (block == 0) {
        ext4_set_invalid(error, "An ext2/3 directory has a hole.");
        return FALSE;
    }
    guint8 *data = ext4_read_block(walk->volume, block, error);
    if (data == NULL) {
        return FALSE;
    }

    guint32 pointers = walk->volume->block_size / 4;
    for (guint32 i = 0; i < pointers && walk->next_logical < walk->dir_blocks; ++i) {
        guint32 child = read_le32(data + (gsize)i * 4);
        gboolean ok = level == 1
            ? ext4_process_mapped_block(walk, child, error)
            : ext4_walk_indirect(walk, child, level - 1, error);
        if (!ok) {
            g_free(data);
            return FALSE;
        }
    }
    g_free(data);
    return TRUE;
}

static gboolean ext4_walk_block_map(Ext4DirWalk *walk, const guint8 *i_block, GError **error)
{
    for (guint i = 0; i < 12 && walk->next_logical < walk->dir_blocks; ++i) {
        if (!ext4_process_mapped_block(walk, read_le32(i_block + i * 4), error)) {
            return FALSE;
        }
    }
    for (guint level = 1; level <= 3 && walk->next_logical < walk->dir_blocks; ++level) {
        if (!ext4_walk_indirect(walk, read_le32(i_block + (11 + level) * 4), level, error)) {
            return FALSE;
        }
    }
    return TRUE;
}

GPtrArray *ext4_list_directory(Ext4Volume *volume, guint32 inode_number,
                               GError **error)
{
    if (volume == NULL) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                            "An ext4 volume is required.");
        return NULL;
    }

    GPtrArray *entries = g_ptr_array_new_with_free_func(ext4_directory_entry_free);
    Ext4DirWalk walk = { .volume = volume, .entries = entries };
    gboolean ok = FALSE;

    guint8 *directory = ext4_read_inode(volume, inode_number, error);
    if (directory == NULL) {
        goto cleanup;
    }

    guint32 flags = read_le32(directory + 0x20);
    guint64 directory_size = ext4_inode_file_size(volume, directory);
    if ((read_le16(directory) & EXT4_MODE_TYPE_MASK) != EXT4_MODE_DIRECTORY) {
        ext4_set_invalid(error, "The ext4 root inode is not a directory.");
        goto cleanup;
    }
    if ((flags & (EXT4_FLAG_INLINE_DATA | EXT4_FLAG_ENCRYPT)) != 0) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                            "Inline-data and encrypted root directories are not supported.");
        goto cleanup;
    }
    if (directory_size == 0 || directory_size > EXT4_MAX_DIRECTORY_SIZE) {
        ext4_set_invalid(error, "The ext4 root directory has an invalid size.");
        goto cleanup;
    }
    walk.dir_blocks = (directory_size + volume->block_size - 1) / volume->block_size;

    if ((flags & EXT4_FLAG_EXTENTS) != 0) {
        ok = ext4_walk_extent_node(&walk, directory + 0x28, 60, -1, error);
    } else {
        ok = ext4_walk_block_map(&walk, directory + 0x28, error);
    }
    if (ok && walk.next_logical < walk.dir_blocks) {
        ext4_set_invalid(error, "The ext4 root directory is shorter than its recorded size.");
        ok = FALSE;
    }

cleanup:
    g_free(directory);
    if (!ok) {
        g_ptr_array_unref(entries);
        return NULL;
    }

    return entries;
}

GPtrArray *ext4_list_root(Ext4Volume *volume, GError **error)
{
    return ext4_list_directory(volume, EXT4_ROOT_INODE, error);
}

typedef struct {
    Ext4Volume *volume;
    guint8 *data;
    guint64 file_offset;
    guint64 request_end;
    guint64 file_size;
    guint nodes_visited;
} Ext4FileWalk;

static gboolean ext4_copy_file_blocks(Ext4FileWalk *walk, guint64 start,
                                      guint64 logical, guint64 count,
                                      gboolean unwritten, GError **error)
{
    guint64 block_size = walk->volume->block_size;
    guint64 extent_start = logical * block_size;
    guint64 extent_end = MIN(walk->file_size, (logical + count) * block_size);
    guint64 copy_start = MAX(extent_start, walk->file_offset);
    guint64 copy_end = MIN(extent_end, walk->request_end);
    if (copy_start >= copy_end || unwritten) {
        return TRUE;
    }

    guint64 physical = start + (copy_start - extent_start) / block_size;
    guint64 leading = copy_start % block_size;
    guint64 byte_count = copy_end - copy_start;
    while (byte_count > 0) {
        guint8 *block_data = ext4_read_block(walk->volume, physical, error);
        if (block_data == NULL) {
            return FALSE;
        }
        gsize chunk = (gsize)MIN(byte_count, block_size - leading);
        guint64 target = copy_start - walk->file_offset;
        memcpy(walk->data + target, block_data + leading, chunk);
        g_free(block_data);
        copy_start += chunk;
        byte_count -= chunk;
        ++physical;
        leading = 0;
    }
    return TRUE;
}

static gboolean ext4_walk_file_extent(Ext4FileWalk *walk, const guint8 *data,
                                      gsize data_size, gint expected_depth,
                                      GError **error)
{
    if (data_size < 12 || read_le16(data) != EXT4_EXTENT_MAGIC) {
        ext4_set_invalid(error, "An ext4 extent node has a bad signature.");
        return FALSE;
    }
    guint32 entries = read_le16(data + 2);
    guint32 maximum = read_le16(data + 4);
    guint32 depth = read_le16(data + 6);
    if (depth > EXT4_MAX_EXTENT_DEPTH
        || (expected_depth >= 0 && depth != (guint32)expected_depth)
        || entries > maximum || 12 + (gsize)entries * 12 > data_size) {
        ext4_set_invalid(error, "An ext4 extent node header is invalid.");
        return FALSE;
    }

    guint64 first_block = walk->file_offset / walk->volume->block_size;
    guint64 end_block = (walk->request_end + walk->volume->block_size - 1)
        / walk->volume->block_size;
    for (guint32 i = 0; i < entries; ++i) {
        const guint8 *entry = data + 12 + (gsize)i * 12;
        guint64 logical = read_le32(entry);
        guint32 raw_length = read_le16(entry + 4);
        guint64 count = raw_length > 32768 ? raw_length - 32768 : raw_length;
        guint64 physical = ((guint64)read_le16(entry + 6) << 32)
            | read_le32(entry + 8);
        if (count == 0 || logical > G_MAXUINT64 - count) {
            ext4_set_invalid(error, "An ext4 file extent has invalid bounds.");
            return FALSE;
        }
        if (logical >= end_block || logical + count <= first_block) {
            continue;
        }
        if (depth == 0) {
            if (physical == 0 || !ext4_block_in_range(walk->volume, physical, count)) {
                ext4_set_invalid(error, "An ext4 file extent lies outside the volume.");
                return FALSE;
            }
            if (!ext4_copy_file_blocks(walk, physical, logical, count,
                                       raw_length > 32768, error)) {
                return FALSE;
            }
        } else {
            if (++walk->nodes_visited > EXT4_MAX_EXTENT_NODES) {
                ext4_set_invalid(error, "An ext4 file extent tree is too large.");
                return FALSE;
            }
            guint64 child = physical;
            guint8 *block = ext4_read_block(walk->volume, child, error);
            if (block == NULL) {
                return FALSE;
            }
            gboolean ok = ext4_walk_file_extent(walk, block, walk->volume->block_size,
                                                 (gint)depth - 1, error);
            g_free(block);
            if (!ok) {
                return FALSE;
            }
        }
    }
    return TRUE;
}

static gboolean ext4_map_legacy_block(Ext4Volume *volume, const guint8 *i_block,
                                      guint64 logical, guint64 *physical,
                                      GError **error)
{
    guint64 pointers = volume->block_size / sizeof(guint32);
    guint64 indices[3] = { 0 };
    guint levels = 0;
    if (logical < 12) {
        *physical = read_le32(i_block + logical * sizeof(guint32));
    } else {
        logical -= 12;
        if (logical < pointers) {
            levels = 1;
        } else {
            logical -= pointers;
            if (logical < pointers * pointers) {
                levels = 2;
                indices[0] = logical / pointers;
                logical %= pointers;
            } else {
                logical -= pointers * pointers;
                if (logical >= pointers * pointers * pointers) {
                    ext4_set_invalid(error, "The ext4 file is too large for block mapping.");
                    return FALSE;
                }
                levels = 3;
                indices[0] = logical / (pointers * pointers);
                indices[1] = (logical / pointers) % pointers;
                logical %= pointers;
            }
        }
        if (levels == 1) {
            indices[0] = logical;
        } else {
            indices[levels - 1] = logical;
        }

        guint32 block = read_le32(i_block + (12 + levels) * sizeof(guint32));
        for (guint level = 0; level < levels; ++level) {
            if (block == 0) {
                *physical = 0;
                return TRUE;
            }
            guint8 *data = ext4_read_block(volume, block, error);
            if (data == NULL) {
                return FALSE;
            }
            block = read_le32(data + indices[level] * sizeof(guint32));
            g_free(data);
        }
        *physical = block;
    }
    if (*physical != 0 && !ext4_block_in_range(volume, *physical, 1)) {
        ext4_set_invalid(error, "An ext4 file block lies outside the volume.");
        return FALSE;
    }
    return TRUE;
}

GBytes *ext4_read_file(Ext4Volume *volume, guint32 inode_number,
                       guint64 offset, gsize length, GError **error)
{
    if (volume == NULL || length == 0 || length > 16 * 1024 * 1024) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                            "The requested ext4 file read must be between 1 byte and 16 MiB.");
        return NULL;
    }
    guint8 *inode = ext4_read_inode(volume, inode_number, error);
    if (inode == NULL) {
        return NULL;
    }
    guint16 mode = read_le16(inode);
    guint64 file_size = ext4_inode_file_size(volume, inode);
    guint32 flags = read_le32(inode + 0x20);
    if ((flags & (EXT4_FLAG_INLINE_DATA | EXT4_FLAG_ENCRYPT)) != 0) {
        g_free(inode);
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                            "Inline-data and encrypted ext4 files are not supported.");
        return NULL;
    }
    if ((mode & EXT4_MODE_TYPE_MASK) != EXT4_MODE_REGULAR
        || offset > file_size || (guint64)length > file_size - offset) {
        g_free(inode);
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                            "The requested ext4 file range is invalid.");
        return NULL;
    }

    guint8 *buffer = g_malloc0(length);
    const guint8 *i_block = inode + 0x28;
    gboolean ok = TRUE;
    if ((flags & EXT4_FLAG_EXTENTS) != 0) {
        Ext4FileWalk walk = {
            .volume = volume,
            .data = buffer,
            .file_offset = offset,
            .request_end = offset + length,
            .file_size = file_size
        };
        ok = ext4_walk_file_extent(&walk, i_block, 60, -1, error);
    } else {
        guint64 position = offset;
        gsize copied = 0;
        while (copied < length) {
            guint64 logical = position / volume->block_size;
            gsize in_block = (gsize)(position % volume->block_size);
            guint64 physical = 0;
            if (!ext4_map_legacy_block(volume, i_block, logical, &physical, error)) {
                ok = FALSE;
                break;
            }
            gsize chunk = MIN(length - copied, volume->block_size - in_block);
            if (physical != 0) {
                guint8 *block_data = ext4_read_block(volume, physical, error);
                if (block_data == NULL) {
                    ok = FALSE;
                    break;
                }
                memcpy(buffer + copied, block_data + in_block, chunk);
                g_free(block_data);
            }
            copied += chunk;
            position += chunk;
        }
    }
    g_free(inode);
    if (!ok) {
        g_free(buffer);
        return NULL;
    }
    return g_bytes_new_take(buffer, length);
}
