#include "fat32.h"

#include <string.h>

#define FAT32_MAX_ROOT_ENTRIES 4096
#define FAT32_MAX_CLUSTER_SIZE (1024 * 1024)

struct _Fat32Volume {
    BlockDevice *device;
    guint64 partition_offset;
    guint64 volume_size;
    guint32 bytes_per_sector;
    guint32 sectors_per_cluster;
    guint32 reserved_sectors;
    guint32 fat_count;
    guint32 sectors_per_fat;
    guint32 root_cluster;
    guint32 cluster_count;
};

static guint16 read_le16(const guint8 *bytes)
{
    return (guint16)bytes[0] | ((guint16)bytes[1] << 8);
}

static guint32 read_le32(const guint8 *bytes)
{
    return (guint32)bytes[0] | ((guint32)bytes[1] << 8)
        | ((guint32)bytes[2] << 16) | ((guint32)bytes[3] << 24);
}

static gboolean is_power_of_two(guint32 value)
{
    return value != 0 && (value & (value - 1)) == 0;
}

static gboolean fat32_parse_at(BlockDevice *device, guint64 partition_offset,
                               guint64 partition_size, Fat32Volume **result,
                               GError **error)
{
    GBytes *sector = block_device_read(device, partition_offset, 512, error);
    if (sector == NULL) {
        return FALSE;
    }

    gsize read_length = 0;
    const guint8 *boot = g_bytes_get_data(sector, &read_length);
    if (read_length < 512 || boot[510] != 0x55 || boot[511] != 0xAA) {
        g_bytes_unref(sector);
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                            "The partition does not contain a valid boot-sector signature.");
        return FALSE;
    }

    guint32 bytes_per_sector = read_le16(boot + 11);
    guint32 sectors_per_cluster = boot[13];
    guint32 reserved_sectors = read_le16(boot + 14);
    guint32 fat_count = boot[16];
    guint32 root_entry_count = read_le16(boot + 17);
    guint32 total_sectors_16 = read_le16(boot + 19);
    guint32 fat_size_16 = read_le16(boot + 22);
    guint32 total_sectors = read_le32(boot + 32);
    guint32 sectors_per_fat = read_le32(boot + 36);
    guint32 filesystem_version = read_le16(boot + 42);
    guint32 root_cluster = read_le32(boot + 44) & 0x0FFFFFFF;
    g_bytes_unref(sector);

    if ((bytes_per_sector != 512 && bytes_per_sector != 1024
         && bytes_per_sector != 2048 && bytes_per_sector != 4096)
        || !is_power_of_two(sectors_per_cluster) || sectors_per_cluster > 128
        || reserved_sectors == 0 || fat_count == 0 || fat_count > 2
        || root_entry_count != 0 || total_sectors_16 != 0 || fat_size_16 != 0
        || total_sectors == 0 || sectors_per_fat == 0 || filesystem_version != 0
        || root_cluster < 2) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                            "The boot sector does not describe a supported FAT32 volume.");
        return FALSE;
    }

    guint64 volume_size = (guint64)total_sectors * bytes_per_sector;
    if (volume_size > block_device_get_size(device) - partition_offset
        || (partition_size != 0 && volume_size > partition_size)) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                            "The FAT32 volume extends beyond the device partition.");
        return FALSE;
    }

    guint64 fat_region_sectors = (guint64)fat_count * sectors_per_fat;
    guint64 data_start_sector = (guint64)reserved_sectors + fat_region_sectors;
    if (data_start_sector >= total_sectors) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                            "The FAT32 metadata occupies the entire volume.");
        return FALSE;
    }

    guint64 cluster_count_64 =
        (total_sectors - data_start_sector) / sectors_per_cluster;
    guint64 fat_entry_capacity =
        (guint64)sectors_per_fat * bytes_per_sector / sizeof(guint32);
    if (cluster_count_64 == 0 || cluster_count_64 > G_MAXUINT32 - 1
        || root_cluster > cluster_count_64 + 1
        || fat_entry_capacity < cluster_count_64 + 2
        || (guint64)bytes_per_sector * sectors_per_cluster
            > FAT32_MAX_CLUSTER_SIZE) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                            "FAT32 cluster or allocation-table geometry is invalid.");
        return FALSE;
    }

    Fat32Volume *volume = g_new0(Fat32Volume, 1);
    volume->device = device;
    volume->partition_offset = partition_offset;
    volume->volume_size = volume_size;
    volume->bytes_per_sector = bytes_per_sector;
    volume->sectors_per_cluster = sectors_per_cluster;
    volume->reserved_sectors = reserved_sectors;
    volume->fat_count = fat_count;
    volume->sectors_per_fat = sectors_per_fat;
    volume->root_cluster = root_cluster;
    volume->cluster_count = (guint32)cluster_count_64;
    *result = volume;
    return TRUE;
}

Fat32Volume *fat32_volume_open(BlockDevice *device, GError **error)
{
    if (device == NULL) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                            "A block device is required.");
        return NULL;
    }

    GError *superfloppy_error = NULL;
    Fat32Volume *volume = NULL;
    if (fat32_parse_at(device, 0, 0, &volume, &superfloppy_error)) {
        return volume;
    }

    GError *mbr_read_error = NULL;
    GBytes *mbr_bytes = block_device_read(device, 0, 512, &mbr_read_error);
    if (mbr_bytes == NULL) {
        g_propagate_prefixed_error(error, superfloppy_error,
                                   "No FAT32 volume found: ");
        g_clear_error(&mbr_read_error);
        return NULL;
    }

    gsize mbr_length = 0;
    const guint8 *mbr = g_bytes_get_data(mbr_bytes, &mbr_length);
    if (mbr_length < 512 || mbr[510] != 0x55 || mbr[511] != 0xAA) {
        g_bytes_unref(mbr_bytes);
        g_propagate_prefixed_error(error, superfloppy_error,
                                   "No FAT32 volume found: ");
        g_clear_error(&mbr_read_error);
        return NULL;
    }

    GError *partition_error = NULL;
    for (guint index = 0; index < 4; ++index) {
        const guint8 *entry = mbr + 446 + index * 16;
        guint8 type = entry[4];
        if (type != 0x0B && type != 0x0C && type != 0x1B && type != 0x1C) {
            continue;
        }

        guint64 start = (guint64)read_le32(entry + 8) * 512;
        guint64 size = (guint64)read_le32(entry + 12) * 512;
        if (start >= block_device_get_size(device) || size == 0
            || size > block_device_get_size(device) - start) {
            g_clear_error(&partition_error);
            g_set_error_literal(&partition_error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                                "An MBR FAT32 partition has invalid bounds.");
            continue;
        }

        GError *candidate_error = NULL;
        if (fat32_parse_at(device, start, size, &volume, &candidate_error)) {
            g_bytes_unref(mbr_bytes);
            g_clear_error(&superfloppy_error);
            g_clear_error(&partition_error);
            return volume;
        }
        g_clear_error(&partition_error);
        partition_error = candidate_error;
    }

    g_bytes_unref(mbr_bytes);
    if (partition_error != NULL) {
        g_clear_error(&superfloppy_error);
        g_propagate_error(error, partition_error);
    } else {
        g_propagate_prefixed_error(error, superfloppy_error,
                                   "No FAT32 volume found: ");
    }
    return NULL;
}

void fat32_volume_close(Fat32Volume *volume)
{
    g_free(volume);
}

Fat32Info fat32_volume_get_info(const Fat32Volume *volume)
{
    Fat32Info info = {
        .bytes_per_sector = volume->bytes_per_sector,
        .sectors_per_cluster = volume->sectors_per_cluster,
        .cluster_count = volume->cluster_count,
        .root_cluster = volume->root_cluster,
        .volume_size = volume->volume_size,
        .partition_offset = volume->partition_offset
    };
    return info;
}

void fat32_directory_entry_free(gpointer data)
{
    Fat32DirectoryEntry *entry = data;
    if (entry != NULL) {
        g_free(entry->name);
        g_free(entry);
    }
}

static guint8 fat32_short_name_checksum(const guint8 name[11])
{
    guint8 checksum = 0;
    for (guint i = 0; i < 11; ++i) {
        checksum = (guint8)(((checksum & 1) << 7) + (checksum >> 1) + name[i]);
    }
    return checksum;
}

static gchar *fat32_decode_lfn(const gunichar2 units[260])
{
    gunichar2 terminated[260];
    memcpy(terminated, units, sizeof(terminated));
    for (guint i = 0; i < G_N_ELEMENTS(terminated); ++i) {
        if (terminated[i] == 0 || terminated[i] == 0xFFFF) {
            terminated[i] = 0;
            break;
        }
    }
    return g_utf16_to_utf8(terminated, -1, NULL, NULL, NULL);
}

static gchar *fat32_decode_short_name(const guint8 *entry)
{
    gchar name[13];
    guint output = 0;
    guint base_length = 8;
    while (base_length > 0 && entry[base_length - 1] == ' ') {
        --base_length;
    }
    for (guint i = 0; i < base_length; ++i) {
        name[output++] = (gchar)(i == 0 && entry[i] == 0x05 ? 0xE5 : entry[i]);
    }

    guint extension_length = 3;
    while (extension_length > 0 && entry[8 + extension_length - 1] == ' ') {
        --extension_length;
    }
    if (extension_length > 0) {
        name[output++] = '.';
        for (guint i = 0; i < extension_length; ++i) {
            name[output++] = (gchar)entry[8 + i];
        }
    }
    name[output] = '\0';
    return g_utf8_make_valid(name, -1);
}

static gboolean fat32_next_cluster(Fat32Volume *volume, guint32 cluster,
                                   guint32 *next_cluster, gboolean *is_end,
                                   GError **error)
{
    guint64 fat_offset = volume->partition_offset
        + (guint64)volume->reserved_sectors * volume->bytes_per_sector
        + (guint64)cluster * sizeof(guint32);
    GBytes *bytes = block_device_read(volume->device, fat_offset, sizeof(guint32), error);
    if (bytes == NULL) {
        return FALSE;
    }
    gsize length = 0;
    const guint8 *data = g_bytes_get_data(bytes, &length);
    (void)length;
    guint32 value = read_le32(data) & 0x0FFFFFFF;
    g_bytes_unref(bytes);

    if (value >= 0x0FFFFFF8) {
        *is_end = TRUE;
        return TRUE;
    }
    if (value < 2 || value == 0x0FFFFFF7 || value >= 0x0FFFFFF0
        || value > volume->cluster_count + 1) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                    "FAT32 contains an invalid cluster-chain value (0x%08x).", value);
        return FALSE;
    }
    *next_cluster = value;
    *is_end = FALSE;
    return TRUE;
}

static gboolean fat32_append_short_entry(GPtrArray *entries, const guint8 *raw,
                                         const gunichar2 long_name[260],
                                         gboolean has_long_name,
                                         GError **error)
{
    if (entries->len >= FAT32_MAX_ROOT_ENTRIES) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_TOO_MANY_OPEN_FILES,
                    "The root directory exceeds the limit of %u entries.",
                    FAT32_MAX_ROOT_ENTRIES);
        return FALSE;
    }

    gchar *name = NULL;
    if (has_long_name) {
        name = fat32_decode_lfn(long_name);
    }
    if (name == NULL || name[0] == '\0') {
        g_free(name);
        name = fat32_decode_short_name(raw);
    }

    Fat32DirectoryEntry *entry = g_new0(Fat32DirectoryEntry, 1);
    entry->name = name;
    entry->is_directory = (raw[11] & 0x10) != 0;
    entry->size = read_le32(raw + 28);
    entry->first_cluster = ((guint32)(read_le16(raw + 20) & 0x0FFF) << 16)
        | read_le16(raw + 26);
    g_ptr_array_add(entries, entry);
    return TRUE;
}

GPtrArray *fat32_list_directory(Fat32Volume *volume, guint32 cluster,
                                GError **error)
{
    if (volume == NULL) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                            "A FAT32 volume is required.");
        return NULL;
    }
    if (cluster < 2 || cluster > volume->cluster_count + 1) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                            "The FAT32 directory cluster is out of range.");
        return NULL;
    }

    GPtrArray *entries = g_ptr_array_new_with_free_func(fat32_directory_entry_free);
    guint32 visited_clusters = 0;
    guint32 cluster_size = volume->bytes_per_sector * volume->sectors_per_cluster;
    gunichar2 long_name_units[260] = { 0 };
    guint8 long_name_checksum = 0;
    guint8 expected_long_name_sequence = 0;
    gboolean has_long_name = FALSE;
    gboolean directory_end = FALSE;

    while (!directory_end) {
        if (++visited_clusters > volume->cluster_count) {
            g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                                "FAT32 root directory contains a cyclic cluster chain.");
            goto fail;
        }

        guint64 first_data_sector = (guint64)volume->reserved_sectors
            + (guint64)volume->fat_count * volume->sectors_per_fat;
        guint64 cluster_sector = first_data_sector
            + (guint64)(cluster - 2) * volume->sectors_per_cluster;
        guint64 cluster_offset = volume->partition_offset
            + cluster_sector * volume->bytes_per_sector;
        GBytes *cluster_bytes =
            block_device_read(volume->device, cluster_offset, cluster_size, error);
        if (cluster_bytes == NULL) {
            goto fail;
        }

        gsize length = 0;
        const guint8 *data = g_bytes_get_data(cluster_bytes, &length);
        for (gsize position = 0; position + 32 <= length; position += 32) {
            const guint8 *raw = data + position;
            guint8 first = raw[0];
            if (first == 0x00) {
                directory_end = TRUE;
                break;
            }
            if (first == 0xE5) {
                has_long_name = FALSE;
                expected_long_name_sequence = 0;
                continue;
            }

            if (raw[11] == 0x0F) {
                guint8 sequence = raw[0] & 0x1F;
                if ((raw[0] & 0x40) != 0) {
                    memset(long_name_units, 0xFF, sizeof(long_name_units));
                    expected_long_name_sequence = sequence;
                    long_name_checksum = raw[13];
                    has_long_name = sequence > 0 && sequence <= 20;
                }
                if (!has_long_name || sequence != expected_long_name_sequence
                    || raw[13] != long_name_checksum) {
                    has_long_name = FALSE;
                    expected_long_name_sequence = 0;
                    continue;
                }

                static const guint8 unit_offsets[] = {
                    1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30
                };
                guint start = (sequence - 1) * 13;
                for (guint i = 0; i < G_N_ELEMENTS(unit_offsets); ++i) {
                    long_name_units[start + i] = read_le16(raw + unit_offsets[i]);
                }
                --expected_long_name_sequence;
                continue;
            }

            gboolean valid_long_name =
                has_long_name && expected_long_name_sequence == 0
                && long_name_checksum == fat32_short_name_checksum(raw);
            gboolean is_dot_entry =
                raw[0] == '.' && (raw[1] == ' ' || (raw[1] == '.' && raw[2] == ' '));
            if ((raw[11] & 0x08) == 0
                && !is_dot_entry
                && !fat32_append_short_entry(entries, raw, long_name_units,
                                             valid_long_name, error)) {
                g_bytes_unref(cluster_bytes);
                goto fail;
            }
            has_long_name = FALSE;
            expected_long_name_sequence = 0;
        }
        g_bytes_unref(cluster_bytes);
        if (directory_end) {
            break;
        }

        guint32 next_cluster = 0;
        gboolean is_end = FALSE;
        if (!fat32_next_cluster(volume, cluster, &next_cluster, &is_end, error)) {
            goto fail;
        }
        if (is_end) {
            break;
        }
        cluster = next_cluster;
    }
    return entries;

fail:
    g_ptr_array_unref(entries);
    return NULL;
}

GPtrArray *fat32_list_root(Fat32Volume *volume, GError **error)
{
    if (volume == NULL) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                            "A FAT32 volume is required.");
        return NULL;
    }
    return fat32_list_directory(volume, volume->root_cluster, error);
}

GBytes *fat32_read_file(Fat32Volume *volume, guint32 first_cluster,
                        guint64 file_size, guint64 offset, gsize length,
                        GError **error)
{
    if (volume == NULL || length == 0 || offset > file_size
        || (guint64)length > file_size - offset
        || length > 16 * 1024 * 1024) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                            "The requested FAT32 file range is invalid or too large.");
        return NULL;
    }
    if (first_cluster < 2 || first_cluster > volume->cluster_count + 1) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                            "The FAT32 file starts at an invalid cluster.");
        return NULL;
    }

    guint32 cluster_size = volume->bytes_per_sector * volume->sectors_per_cluster;
    guint64 cluster_index = offset / cluster_size;
    if (cluster_index >= volume->cluster_count) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                            "The FAT32 file offset exceeds the volume cluster chain.");
        return NULL;
    }
    guint32 cluster = first_cluster;
    guint32 visited = 0;
    for (guint64 i = 0; i < cluster_index; ++i) {
        if (++visited > volume->cluster_count) {
            g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                                "The FAT32 file contains a cyclic cluster chain.");
            return NULL;
        }
        guint32 next_cluster = 0;
        gboolean is_end = FALSE;
        if (!fat32_next_cluster(volume, cluster, &next_cluster, &is_end, error)) {
            return NULL;
        }
        if (is_end) {
            g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                                "The FAT32 file cluster chain ends before its recorded size.");
            return NULL;
        }
        cluster = next_cluster;
    }

    guint8 *buffer = g_malloc(length);
    gsize copied = 0;
    guint64 position = offset;
    while (copied < length) {
        if (++visited > volume->cluster_count) {
            g_free(buffer);
            g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                                "The FAT32 file contains a cyclic cluster chain.");
            return NULL;
        }
        guint32 first_data_sector = volume->reserved_sectors
            + volume->fat_count * volume->sectors_per_fat;
        guint64 cluster_sector = (guint64)first_data_sector
            + (guint64)(cluster - 2) * volume->sectors_per_cluster;
        guint64 cluster_offset = volume->partition_offset
            + cluster_sector * volume->bytes_per_sector;
        GBytes *cluster_bytes =
            block_device_read(volume->device, cluster_offset, cluster_size, error);
        if (cluster_bytes == NULL) {
            g_free(buffer);
            return NULL;
        }

        gsize in_cluster = (gsize)(position % cluster_size);
        gsize chunk = MIN(length - copied, cluster_size - in_cluster);
        memcpy(buffer + copied,
               (const guint8 *)g_bytes_get_data(cluster_bytes, NULL) + in_cluster,
               chunk);
        g_bytes_unref(cluster_bytes);
        copied += chunk;
        position += chunk;
        if (copied == length) {
            break;
        }

        guint32 next_cluster = 0;
        gboolean is_end = FALSE;
        if (!fat32_next_cluster(volume, cluster, &next_cluster, &is_end, error)) {
            g_free(buffer);
            return NULL;
        }
        if (is_end) {
            g_free(buffer);
            g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                                "The FAT32 file cluster chain ends before its recorded size.");
            return NULL;
        }
        cluster = next_cluster;
    }

    return g_bytes_new_take(buffer, copied);
}
