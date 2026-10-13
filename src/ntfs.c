#include "ntfs.h"

#include <string.h>

#define NTFS_MAX_ROOT_ENTRIES 4096
#define NTFS_MAX_CLUSTER_SIZE (2 * 1024 * 1024)
#define NTFS_MAX_RECORD_SIZE (64 * 1024)
#define NTFS_MAX_INDEX_BLOCK_SIZE (1024 * 1024)
#define NTFS_MAX_INDEX_BLOCKS (1 << 20)
#define NTFS_MAX_INDEX_DEPTH 16
#define NTFS_READ_CHUNK (1024 * 1024)
#define NTFS_MAX_ATTRIBUTE_LIST_SIZE (1024 * 1024)
#define NTFS_MAX_FRAGMENTS 256
#define NTFS_RECORD_NUMBER_MASK G_GUINT64_CONSTANT(0x0000FFFFFFFFFFFF)



#define NTFS_FIXUP_STRIDE 512

#define NTFS_ROOT_RECORD 5



#ifndef NTFS_FIRST_USER_RECORD
#define NTFS_FIRST_USER_RECORD 16
#endif

#define NTFS_ATTR_ATTRIBUTE_LIST 0x20u
#define NTFS_ATTR_DATA 0x80u
#define NTFS_ATTR_INDEX_ROOT 0x90u
#define NTFS_ATTR_INDEX_ALLOCATION 0xA0u
#define NTFS_ATTR_END 0xFFFFFFFFu

#define NTFS_RECORD_IN_USE 0x0001
#define NTFS_RECORD_DIRECTORY 0x0002

#define NTFS_NAMESPACE_DOS 2
#define NTFS_FILE_NAME_ATTRIBUTE_SIZE 66
#define NTFS_FILE_FLAG_DIRECTORY 0x10000000u
#define NTFS_FILE_FLAG_DOS_DIRECTORY 0x00000010u

typedef struct {
    guint64 vcn;
    guint64 length;
    gint64 lcn;  
} NtfsRun;

typedef struct {
    GArray *runs;  
    guint64 data_size;
} NtfsRunList;

struct _NtfsVolume {
    BlockDevice *device;
    guint64 partition_offset;
    guint64 volume_size;
    guint32 bytes_per_sector;
    guint32 sectors_per_cluster;
    guint32 cluster_size;
    guint32 file_record_size;
    guint64 cluster_count;
    guint64 mft_start_cluster;
    NtfsRunList mft;
};

typedef struct {
    NtfsVolume *volume;
    GPtrArray *entries;
    NtfsRunList allocation;
    guint32 index_block_size;
    guint32 vcn_size;
    guint64 block_count;
    guint8 *visited;
} NtfsIndexWalk;

static const gunichar2 ntfs_i30_name[] = { '$', 'I', '3', '0' };

static guint16 read_le16(const guint8 *bytes)
{
    return (guint16)bytes[0] | ((guint16)bytes[1] << 8);
}

static guint32 read_le32(const guint8 *bytes)
{
    return (guint32)bytes[0] | ((guint32)bytes[1] << 8)
        | ((guint32)bytes[2] << 16) | ((guint32)bytes[3] << 24);
}

static guint64 read_le64(const guint8 *bytes)
{
    return (guint64)read_le32(bytes) | ((guint64)read_le32(bytes + 4) << 32);
}

static gboolean is_power_of_two(guint64 value)
{
    return value != 0 && (value & (value - 1)) == 0;
}

static void ntfs_set_invalid(GError **error, const gchar *message)
{
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, message);
}

static gsize ntfs_boot_read_size(BlockDevice *device)
{
    return MAX((gsize)512, (gsize)block_device_get_sector_size(device));
}



static void ntfs_run_list_clear(NtfsRunList *list)
{
    if (list->runs != NULL) {
        g_array_free(list->runs, TRUE);
    }
    list->runs = NULL;
    list->data_size = 0;
}



static gboolean ntfs_parse_runs(const NtfsVolume *volume, const guint8 *data,
                                gsize length, guint64 start_vcn, GArray *runs,
                                GError **error)
{
    guint64 vcn = start_vcn;
    gint64 lcn = 0;
    gsize position = 0;
    guint first_run = runs->len;

    guint64 expected_vcn = 0;
    if (runs->len > 0) {
        const NtfsRun *last = &g_array_index(runs, NtfsRun, runs->len - 1);
        expected_vcn = last->vcn + last->length;
    }
    if (start_vcn != expected_vcn) {
        ntfs_set_invalid(error, "NTFS attribute fragments are not contiguous.");
        return FALSE;
    }

    while (position < length && data[position] != 0) {
        guint8 header = data[position++];
        guint length_size = header & 0x0F;
        guint offset_size = header >> 4;
        if (length_size == 0 || length_size > 8 || offset_size > 8
            || position + length_size + offset_size > length) {
            ntfs_set_invalid(error, "NTFS data-run list is malformed.");
            return FALSE;
        }

        guint64 run_length = 0;
        for (guint i = 0; i < length_size; ++i) {
            run_length |= (guint64)data[position + i] << (8 * i);
        }
        position += length_size;

        NtfsRun run = { .vcn = vcn, .length = run_length, .lcn = -1 };
        if (offset_size > 0) {
            guint64 raw = 0;
            for (guint i = 0; i < offset_size; ++i) {
                raw |= (guint64)data[position + i] << (8 * i);
            }
            if (offset_size < 8 && (data[position + offset_size - 1] & 0x80) != 0) {
                raw |= G_MAXUINT64 << (8 * offset_size);
            }
            gint64 delta = (gint64)raw;
            if ((delta > 0 && lcn > G_MAXINT64 - delta)
                || (delta < 0 && lcn < G_MININT64 - delta)) {
                ntfs_set_invalid(error, "NTFS data-run offset overflows.");
                return FALSE;
            }
            lcn += delta;
            run.lcn = lcn;
        }
        position += offset_size;

        if (run_length == 0 || run_length > volume->cluster_count
            || vcn > G_MAXUINT64 - run_length
            || (run.lcn >= 0
                && ((guint64)run.lcn >= volume->cluster_count
                    || run_length > volume->cluster_count - (guint64)run.lcn))) {
            ntfs_set_invalid(error, "NTFS data run lies outside the volume.");
            return FALSE;
        }

        g_array_append_val(runs, run);
        vcn += run_length;
    }

    if (runs->len == first_run) {
        ntfs_set_invalid(error, "NTFS attribute has an empty data-run list.");
        return FALSE;
    }
    return TRUE;
}



static gboolean ntfs_load_runs(const NtfsVolume *volume,
                               const guint8 *const *fragments, guint count,
                               NtfsRunList *list, GError **error)
{
    ntfs_run_list_clear(list);
    if (count == 0) {
        ntfs_set_invalid(error, "NTFS attribute has no fragments.");
        return FALSE;
    }

    list->runs = g_array_new(FALSE, FALSE, sizeof(NtfsRun));
    for (guint i = 0; i < count; ++i) {
        const guint8 *attribute = fragments[i];
        guint32 attribute_length = read_le32(attribute + 4);
        guint32 runs_offset = read_le16(attribute + 32);
        if (attribute[8] == 0 || runs_offset < 64 || runs_offset > attribute_length) {
            ntfs_set_invalid(error, "NTFS attribute is not a valid non-resident attribute.");
            goto fail;
        }
        if (i == 0) {
            if (read_le64(attribute + 16) != 0) {
                ntfs_set_invalid(error, "NTFS attribute does not start at VCN 0.");
                goto fail;
            }
            list->data_size = read_le64(attribute + 48);
        }
        if (!ntfs_parse_runs(volume, attribute + runs_offset,
                             attribute_length - runs_offset,
                             read_le64(attribute + 16), list->runs, error)) {
            goto fail;
        }
    }
    return TRUE;

fail:
    ntfs_run_list_clear(list);
    return FALSE;
}

static const NtfsRun *ntfs_find_run(const NtfsRunList *list, guint64 vcn)
{
    for (guint i = 0; i < list->runs->len; ++i) {
        const NtfsRun *run = &g_array_index(list->runs, NtfsRun, i);
        if (vcn >= run->vcn && vcn - run->vcn < run->length) {
            return run;
        }
    }
    return NULL;
}



static guint8 *ntfs_read_runs(NtfsVolume *volume, const NtfsRunList *list,
                              guint64 offset, gsize length, GError **error)
{
    if (list->runs == NULL || length == 0 || offset > list->data_size
        || (guint64)length > list->data_size - offset) {
        ntfs_set_invalid(error, "NTFS read lies outside the attribute data.");
        return NULL;
    }

    guint8 *buffer = g_malloc0(length);
    gsize done = 0;
    while (done < length) {
        guint64 position = offset + done;
        guint64 vcn = position / volume->cluster_size;
        guint64 in_cluster = position % volume->cluster_size;
        const NtfsRun *run = ntfs_find_run(list, vcn);
        if (run == NULL) {
            g_free(buffer);
            ntfs_set_invalid(error, "NTFS data runs do not cover the requested range.");
            return NULL;
        }

        guint64 run_offset = (vcn - run->vcn) * volume->cluster_size + in_cluster;
        guint64 run_bytes = run->length * volume->cluster_size;
        guint64 available = run_bytes - run_offset;
        gsize chunk = (gsize)MIN((guint64)(length - done), MIN(available, (guint64)NTFS_READ_CHUNK));

        if (run->lcn >= 0) {
            guint64 device_offset = volume->partition_offset
                + (guint64)run->lcn * volume->cluster_size + run_offset;
            GBytes *bytes = block_device_read(volume->device, device_offset, chunk, error);
            if (bytes == NULL) {
                g_free(buffer);
                return NULL;
            }
            memcpy(buffer + done, g_bytes_get_data(bytes, NULL), chunk);
            g_bytes_unref(bytes);
        }
        done += chunk;
    }
    return buffer;
}



static gboolean ntfs_apply_fixups(guint8 *record, gsize size, const gchar magic[4],
                                  GError **error)
{
    if (size < NTFS_FIXUP_STRIDE || size % NTFS_FIXUP_STRIDE != 0
        || memcmp(record, magic, 4) != 0) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                    "NTFS record does not have the expected '%.4s' signature.", magic);
        return FALSE;
    }

    guint32 usa_offset = read_le16(record + 4);
    guint32 usa_count = read_le16(record + 6);
    gsize strides = size / NTFS_FIXUP_STRIDE;
    if (usa_count != strides + 1 || usa_offset < 8
        || usa_offset + usa_count * 2 > NTFS_FIXUP_STRIDE - 2) {
        ntfs_set_invalid(error, "NTFS record has an invalid update sequence array.");
        return FALSE;
    }

    const guint8 *usa = record + usa_offset;
    guint16 sequence = read_le16(usa);
    for (gsize i = 1; i <= strides; ++i) {
        guint8 *tail = record + i * NTFS_FIXUP_STRIDE - 2;
        if (read_le16(tail) != sequence) {
            ntfs_set_invalid(error, "NTFS record is damaged (update sequence mismatch).");
            return FALSE;
        }
        tail[0] = usa[i * 2];
        tail[1] = usa[i * 2 + 1];
    }
    return TRUE;
}

static gboolean ntfs_validate_file_record(guint8 *record, guint32 size, GError **error)
{
    if (!ntfs_apply_fixups(record, size, "FILE", error)) {
        return FALSE;
    }

    guint32 flags = read_le16(record + 22);
    guint32 first_attribute = read_le16(record + 20);
    guint32 used_size = read_le32(record + 24);
    guint32 allocated_size = read_le32(record + 28);
    if ((flags & NTFS_RECORD_IN_USE) == 0 || used_size > size || allocated_size > size
        || first_attribute < 24 || first_attribute + 4 > used_size) {
        ntfs_set_invalid(error, "NTFS file record header is invalid.");
        return FALSE;
    }
    return TRUE;
}



static gboolean ntfs_find_attribute(const guint8 *record, guint32 type,
                                    const gunichar2 *name, guint32 name_units,
                                    const guint8 **found, GError **error)
{
    guint32 used_size = read_le32(record + 24);
    guint32 position = read_le16(record + 20);

    *found = NULL;
    while (position + 4 <= used_size) {
        const guint8 *attribute = record + position;
        guint32 attribute_type = read_le32(attribute);
        if (attribute_type == NTFS_ATTR_END) {
            return TRUE;
        }
        if (position + 16 > used_size) {
            ntfs_set_invalid(error, "NTFS attribute header is truncated.");
            return FALSE;
        }

        guint32 attribute_length = read_le32(attribute + 4);
        guint32 minimum_length = attribute[8] != 0 ? 64 : 24;
        guint32 name_length = attribute[9];
        guint32 name_offset = read_le16(attribute + 10);
        if (attribute[8] > 1 || attribute_length < minimum_length
            || (attribute_length & 7) != 0 || attribute_length > used_size - position
            || (name_length != 0
                && (name_offset < 16 || name_offset + name_length * 2 > attribute_length))) {
            ntfs_set_invalid(error, "NTFS attribute header is invalid.");
            return FALSE;
        }

        if (attribute_type == type && name_length == name_units) {
            gboolean match = TRUE;
            for (guint32 i = 0; i < name_units; ++i) {
                if (read_le16(attribute + name_offset + i * 2) != name[i]) {
                    match = FALSE;
                    break;
                }
            }
            if (match) {
                *found = attribute;
                return TRUE;
            }
        }
        position += attribute_length;
    }
    return TRUE;
}

static gboolean ntfs_resident_value(const guint8 *attribute, const guint8 **value,
                                    guint32 *length, GError **error)
{
    guint32 attribute_length = read_le32(attribute + 4);
    guint32 value_length = read_le32(attribute + 16);
    guint32 value_offset = read_le16(attribute + 20);
    if (attribute[8] != 0 || value_offset > attribute_length
        || value_length > attribute_length - value_offset) {
        ntfs_set_invalid(error, "NTFS resident attribute value is invalid.");
        return FALSE;
    }
    *value = attribute + value_offset;
    *length = value_length;
    return TRUE;
}



static guint8 *ntfs_read_file_record(NtfsVolume *volume, guint64 number,
                                     guint64 expected_base, GError **error)
{
    if (number > G_MAXUINT64 / volume->file_record_size) {
        ntfs_set_invalid(error, "NTFS file-record number is out of range.");
        return NULL;
    }

    guint8 *record = ntfs_read_runs(volume, &volume->mft,
                                    number * volume->file_record_size,
                                    volume->file_record_size, error);
    if (record == NULL) {
        return NULL;
    }
    if (!ntfs_validate_file_record(record, volume->file_record_size, error)) {
        g_free(record);
        return NULL;
    }
    if ((read_le64(record + 32) & NTFS_RECORD_NUMBER_MASK) != expected_base) {
        g_free(record);
        ntfs_set_invalid(error, "NTFS file record belongs to a different base record.");
        return NULL;
    }
    return record;
}





static gboolean ntfs_locate_attribute(NtfsVolume *volume, const guint8 *base_record,
                                      guint64 base_number, guint32 type,
                                      const gunichar2 *name, guint32 name_units,
                                      GPtrArray *buffers, GPtrArray *fragments,
                                      GError **error)
{
    const guint8 *attribute = NULL;
    if (!ntfs_find_attribute(base_record, type, name, name_units, &attribute, error)) {
        return FALSE;
    }
    if (attribute != NULL) {
        g_ptr_array_add(fragments, (gpointer)attribute);
    }

    const guint8 *list_attribute = NULL;
    if (!ntfs_find_attribute(base_record, NTFS_ATTR_ATTRIBUTE_LIST, NULL, 0,
                             &list_attribute, error)) {
        return FALSE;
    }
    if (list_attribute == NULL) {
        return TRUE;
    }


    const guint8 *list = NULL;
    guint32 list_length = 0;
    guint8 *list_buffer = NULL;
    NtfsRunList list_runs = { 0 };
    gboolean ok = FALSE;

    if (list_attribute[8] == 0) {
        if (!ntfs_resident_value(list_attribute, &list, &list_length, error)) {
            goto done;
        }
    } else {
        if (!ntfs_load_runs(volume, &list_attribute, 1, &list_runs, error)) {
            goto done;
        }
        if (list_runs.data_size == 0 || list_runs.data_size > NTFS_MAX_ATTRIBUTE_LIST_SIZE) {
            ntfs_set_invalid(error, "The NTFS $ATTRIBUTE_LIST has an unsupported size.");
            goto done;
        }
        list_buffer = ntfs_read_runs(volume, &list_runs, 0, (gsize)list_runs.data_size, error);
        if (list_buffer == NULL) {
            goto done;
        }
        list = list_buffer;
        list_length = (guint32)list_runs.data_size;
    }

    guint32 position = 0;
    while (position + 26 <= list_length) {
        const guint8 *entry = list + position;
        guint32 entry_length = read_le16(entry + 4);
        guint32 entry_name_units = entry[6];
        guint32 entry_name_offset = entry[7];
        if (entry_length < 26 || (entry_length & 7) != 0
            || entry_length > list_length - position
            || (entry_name_units != 0
                && (entry_name_offset < 26
                    || entry_name_offset + entry_name_units * 2 > entry_length))) {
            ntfs_set_invalid(error, "The NTFS $ATTRIBUTE_LIST is malformed.");
            goto done;
        }

        gboolean match = read_le32(entry) == type && entry_name_units == name_units;
        for (guint32 i = 0; match && i < name_units; ++i) {
            match = read_le16(entry + entry_name_offset + i * 2) == name[i];
        }
        guint64 record_number = read_le64(entry + 16) & NTFS_RECORD_NUMBER_MASK;


        if (match && record_number != base_number) {
            if (fragments->len >= NTFS_MAX_FRAGMENTS) {
                ntfs_set_invalid(error, "The NTFS attribute is split into too many fragments.");
                goto done;
            }
            guint8 *extension = ntfs_read_file_record(volume, record_number,
                                                      base_number, error);
            if (extension == NULL) {
                goto done;
            }
            g_ptr_array_add(buffers, extension);

            const guint8 *fragment = NULL;
            if (!ntfs_find_attribute(extension, type, name, name_units, &fragment, error)) {
                goto done;
            }
            if (fragment == NULL) {
                ntfs_set_invalid(error, "The NTFS $ATTRIBUTE_LIST refers to a missing attribute.");
                goto done;
            }
            g_ptr_array_add(fragments, (gpointer)fragment);
        }
        position += entry_length;
    }
    ok = TRUE;

done:
    g_free(list_buffer);
    ntfs_run_list_clear(&list_runs);
    return ok;
}





static gboolean ntfs_load_mft(NtfsVolume *volume, GError **error)
{
    guint64 offset = volume->partition_offset
        + volume->mft_start_cluster * volume->cluster_size;
    GBytes *bytes = block_device_read(volume->device, offset,
                                      volume->file_record_size, error);
    if (bytes == NULL) {
        return FALSE;
    }

    gsize length = 0;
    guint8 *record = g_bytes_unref_to_data(bytes, &length);
    const guint8 *data_attribute = NULL;
    gboolean ok = FALSE;

    if (!ntfs_validate_file_record(record, volume->file_record_size, error)
        || !ntfs_find_attribute(record, NTFS_ATTR_DATA, NULL, 0, &data_attribute, error)) {
        goto done;
    }
    if (data_attribute == NULL || data_attribute[8] == 0) {
        ntfs_set_invalid(error, "The $MFT record has no non-resident $DATA attribute.");
        goto done;
    }
    if (!ntfs_load_runs(volume, &data_attribute, 1, &volume->mft, error)) {
        goto done;
    }

    const NtfsRun *first = &g_array_index(volume->mft.runs, NtfsRun, 0);
    if (first->vcn != 0 || first->lcn < 0
        || (guint64)first->lcn != volume->mft_start_cluster) {
        ntfs_set_invalid(error, "The $MFT data runs do not match the boot sector.");
        goto done;
    }
    ok = TRUE;

done:
    g_free(record);
    return ok;
}

static gboolean ntfs_parse_at(BlockDevice *device, guint64 partition_offset,
                              guint64 partition_size, NtfsVolume **result,
                              GError **error)
{
    GBytes *sector = block_device_read(device, partition_offset,
                                       ntfs_boot_read_size(device), error);
    if (sector == NULL) {
        return FALSE;
    }

    gsize read_length = 0;
    const guint8 *boot = g_bytes_get_data(sector, &read_length);
    if (read_length < 512 || boot[510] != 0x55 || boot[511] != 0xAA
        || memcmp(boot + 3, "NTFS    ", 8) != 0) {
        g_bytes_unref(sector);
        ntfs_set_invalid(error, "The partition does not contain an NTFS boot sector.");
        return FALSE;
    }

    guint32 bytes_per_sector = read_le16(boot + 11);
    guint32 cluster_code = boot[13];
    guint64 total_sectors = read_le64(boot + 40);
    guint64 mft_start_cluster = read_le64(boot + 48);
    gint32 record_code = (gint8)boot[64];
    g_bytes_unref(sector);

    if (bytes_per_sector != 512 && bytes_per_sector != 1024
        && bytes_per_sector != 2048 && bytes_per_sector != 4096) {
        ntfs_set_invalid(error, "The NTFS boot sector has an unsupported sector size.");
        return FALSE;
    }


    guint64 cluster_size = 0;
    if (cluster_code != 0 && cluster_code <= 0x80) {
        if (is_power_of_two(cluster_code)) {
            cluster_size = (guint64)cluster_code * bytes_per_sector;
        }
    } else if (cluster_code > 0x80 && 256 - cluster_code <= 21) {
        cluster_size = (guint64)1 << (256 - cluster_code);
    }
    if (cluster_size < bytes_per_sector || cluster_size > NTFS_MAX_CLUSTER_SIZE) {
        ntfs_set_invalid(error, "The NTFS boot sector has an unsupported cluster size.");
        return FALSE;
    }


    guint64 record_size = 0;
    if (record_code > 0) {
        record_size = (guint64)record_code * cluster_size;
    } else if (record_code < 0 && -record_code <= 16) {
        record_size = (guint64)1 << (-record_code);
    }
    if (record_size < NTFS_FIXUP_STRIDE || record_size > NTFS_MAX_RECORD_SIZE
        || record_size % NTFS_FIXUP_STRIDE != 0) {
        ntfs_set_invalid(error, "The NTFS boot sector has an unsupported file-record size.");
        return FALSE;
    }

    if (total_sectors == 0 || total_sectors > G_MAXUINT64 / bytes_per_sector) {
        ntfs_set_invalid(error, "The NTFS boot sector has an invalid sector count.");
        return FALSE;
    }
    guint64 volume_size = total_sectors * bytes_per_sector;
    guint64 device_size = block_device_get_size(device);
    if (partition_offset >= device_size || volume_size > device_size - partition_offset
        || (partition_size != 0 && volume_size > partition_size)) {
        ntfs_set_invalid(error, "The NTFS volume extends beyond the device partition.");
        return FALSE;
    }

    guint64 cluster_count = volume_size / cluster_size;
    if (cluster_count == 0 || mft_start_cluster >= cluster_count
        || mft_start_cluster * cluster_size + record_size > volume_size) {
        ntfs_set_invalid(error, "The NTFS $MFT location is invalid.");
        return FALSE;
    }

    NtfsVolume *volume = g_new0(NtfsVolume, 1);
    volume->device = device;
    volume->partition_offset = partition_offset;
    volume->volume_size = volume_size;
    volume->bytes_per_sector = bytes_per_sector;
    volume->sectors_per_cluster = (guint32)(cluster_size / bytes_per_sector);
    volume->cluster_size = (guint32)cluster_size;
    volume->file_record_size = (guint32)record_size;
    volume->cluster_count = cluster_count;
    volume->mft_start_cluster = mft_start_cluster;

    if (!ntfs_load_mft(volume, error)) {
        ntfs_volume_close(volume);
        return FALSE;
    }
    *result = volume;
    return TRUE;
}

NtfsVolume *ntfs_volume_open(BlockDevice *device, GError **error)
{
    if (device == NULL) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                            "A block device is required.");
        return NULL;
    }

    GError *superfloppy_error = NULL;
    NtfsVolume *volume = NULL;
    if (ntfs_parse_at(device, 0, 0, &volume, &superfloppy_error)) {
        return volume;
    }

    GError *mbr_read_error = NULL;
    GBytes *mbr_bytes = block_device_read(device, 0, ntfs_boot_read_size(device),
                                          &mbr_read_error);
    if (mbr_bytes == NULL) {
        g_propagate_prefixed_error(error, superfloppy_error,
                                   "No NTFS volume found: ");
        g_clear_error(&mbr_read_error);
        return NULL;
    }

    gsize mbr_length = 0;
    const guint8 *mbr = g_bytes_get_data(mbr_bytes, &mbr_length);
    if (mbr_length < 512 || mbr[510] != 0x55 || mbr[511] != 0xAA) {
        g_bytes_unref(mbr_bytes);
        g_propagate_prefixed_error(error, superfloppy_error,
                                   "No NTFS volume found: ");
        g_clear_error(&mbr_read_error);
        return NULL;
    }
    if (memcmp(mbr + 3, "NTFS    ", 8) == 0) {

        g_bytes_unref(mbr_bytes);
        g_propagate_error(error, superfloppy_error);
        return NULL;
    }

    guint32 sector_size = block_device_get_sector_size(device);
    guint64 device_size = block_device_get_size(device);
    GError *partition_error = NULL;
    for (guint index = 0; index < 4; ++index) {
        const guint8 *entry = mbr + 446 + index * 16;
        guint8 type = entry[4];

        if (type != 0x07 && type != 0x17 && type != 0x27) {
            continue;
        }

        guint64 start = (guint64)read_le32(entry + 8) * sector_size;
        guint64 size = (guint64)read_le32(entry + 12) * sector_size;
        if (start >= device_size || size == 0 || size > device_size - start) {
            g_clear_error(&partition_error);
            g_set_error_literal(&partition_error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                                "An MBR NTFS partition has invalid bounds.");
            continue;
        }

        GError *candidate_error = NULL;
        if (ntfs_parse_at(device, start, size, &volume, &candidate_error)) {
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
                                   "No NTFS volume found: ");
    }
    return NULL;
}

void ntfs_volume_close(NtfsVolume *volume)
{
    if (volume == NULL) {
        return;
    }
    ntfs_run_list_clear(&volume->mft);
    g_free(volume);
}

NtfsInfo ntfs_volume_get_info(const NtfsVolume *volume)
{
    NtfsInfo info = {
        .bytes_per_sector = volume->bytes_per_sector,
        .sectors_per_cluster = volume->sectors_per_cluster,
        .cluster_count = volume->cluster_count,
        .mft_start_cluster = volume->mft_start_cluster,
        .volume_size = volume->volume_size,
        .partition_offset = volume->partition_offset
    };
    return info;
}

void ntfs_directory_entry_free(gpointer data)
{
    NtfsDirectoryEntry *entry = data;
    if (entry != NULL) {
        g_free(entry->name);
        g_free(entry);
    }
}



static gboolean ntfs_append_entry(NtfsIndexWalk *walk, const guint8 *entry,
                                  GError **error)
{
    guint64 record_number = read_le64(entry) & G_GUINT64_CONSTANT(0x0000FFFFFFFFFFFF);
    guint32 key_length = read_le16(entry + 10);
    const guint8 *key = entry + 16;
    guint32 name_units = key[64];
    guint8 name_space = key[65];

    if (NTFS_FILE_NAME_ATTRIBUTE_SIZE + name_units * 2 > key_length) {
        ntfs_set_invalid(error, "NTFS directory entry has a truncated file name.");
        return FALSE;
    }


    if (name_space == NTFS_NAMESPACE_DOS || name_units == 0
        || record_number < NTFS_FIRST_USER_RECORD) {
        return TRUE;
    }

    if (walk->entries->len >= NTFS_MAX_ROOT_ENTRIES) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_TOO_MANY_OPEN_FILES,
                    "The root directory exceeds the limit of %u entries.",
                    NTFS_MAX_ROOT_ENTRIES);
        return FALSE;
    }

    gunichar2 units[256];
    for (guint32 i = 0; i < name_units; ++i) {
        units[i] = read_le16(key + NTFS_FILE_NAME_ATTRIBUTE_SIZE + i * 2);
    }

    for (guint32 i = 0; i < name_units; ++i) {
        if (units[i] >= 0xD800 && units[i] <= 0xDBFF) {
            if (i + 1 < name_units && units[i + 1] >= 0xDC00 && units[i + 1] <= 0xDFFF) {
                ++i;
            } else {
                units[i] = 0xFFFD;
            }
        } else if (units[i] >= 0xDC00 && units[i] <= 0xDFFF) {
            units[i] = 0xFFFD;
        }
    }
    gchar *name = g_utf16_to_utf8(units, name_units, NULL, NULL, NULL);
    if (name == NULL || name[0] == '\0') {
        g_free(name);
        name = g_strdup("?");
    }

    guint32 attributes = read_le32(key + 56);
    NtfsDirectoryEntry *item = g_new0(NtfsDirectoryEntry, 1);
    item->name = name;
    item->is_directory =
        (attributes & (NTFS_FILE_FLAG_DIRECTORY | NTFS_FILE_FLAG_DOS_DIRECTORY)) != 0;
    item->size = item->is_directory ? 0 : read_le64(key + 48);
    item->record_number = record_number;
    g_ptr_array_add(walk->entries, item);
    return TRUE;
}

static gboolean ntfs_walk_node(NtfsIndexWalk *walk, const guint8 *data,
                               gsize start, gsize end, guint depth, GError **error);

static gboolean ntfs_walk_child(NtfsIndexWalk *walk, guint64 vcn, guint depth,
                                GError **error)
{
    if (walk->visited == NULL) {
        ntfs_set_invalid(error, "The directory index refers to a missing $INDEX_ALLOCATION.");
        return FALSE;
    }
    if (vcn > walk->allocation.data_size / walk->vcn_size) {
        ntfs_set_invalid(error, "An NTFS index node lies outside $INDEX_ALLOCATION.");
        return FALSE;
    }

    guint64 byte_offset = vcn * walk->vcn_size;
    guint64 block = byte_offset / walk->index_block_size;
    if (byte_offset % walk->index_block_size != 0 || block >= walk->block_count) {
        ntfs_set_invalid(error, "An NTFS index node is misaligned.");
        return FALSE;
    }
    if (walk->visited[block]) {
        ntfs_set_invalid(error, "The NTFS directory index contains a cycle.");
        return FALSE;
    }
    walk->visited[block] = 1;

    guint8 *buffer = ntfs_read_runs(walk->volume, &walk->allocation, byte_offset,
                                    walk->index_block_size, error);
    if (buffer == NULL) {
        return FALSE;
    }

    gboolean ok = FALSE;
    if (ntfs_apply_fixups(buffer, walk->index_block_size, "INDX", error)) {
        guint32 first_entry = read_le32(buffer + 24);
        guint32 total_size = read_le32(buffer + 28);
        if (read_le64(buffer + 16) != vcn || first_entry < 16 || first_entry > total_size
            || 24 + (guint64)total_size > walk->index_block_size) {
            ntfs_set_invalid(error, "An NTFS index node header is invalid.");
        } else {
            ok = ntfs_walk_node(walk, buffer, 24 + first_entry, 24 + total_size,
                                depth + 1, error);
        }
    }
    g_free(buffer);
    return ok;
}



static gboolean ntfs_walk_node(NtfsIndexWalk *walk, const guint8 *data,
                               gsize start, gsize end, guint depth, GError **error)
{
    if (depth > NTFS_MAX_INDEX_DEPTH) {
        ntfs_set_invalid(error, "The NTFS directory index is nested too deeply.");
        return FALSE;
    }

    gsize position = start;
    for (;;) {
        if (position > end || end - position < 16) {
            ntfs_set_invalid(error, "An NTFS index entry is truncated.");
            return FALSE;
        }

        const guint8 *entry = data + position;
        guint32 entry_length = read_le16(entry + 8);
        guint32 key_length = read_le16(entry + 10);
        guint32 flags = read_le16(entry + 12);
        gboolean has_child = (flags & 0x01) != 0;
        gboolean is_last = (flags & 0x02) != 0;
        guint32 tail = has_child ? 8 : 0;

        if (entry_length < 16 + tail || (entry_length & 7) != 0
            || entry_length > end - position) {
            ntfs_set_invalid(error, "An NTFS index entry has an invalid length.");
            return FALSE;
        }

        if (has_child
            && !ntfs_walk_child(walk, read_le64(entry + entry_length - 8), depth, error)) {
            return FALSE;
        }
        if (is_last) {
            return TRUE;
        }

        if (key_length < NTFS_FILE_NAME_ATTRIBUTE_SIZE
            || key_length > entry_length - 16 - tail) {
            ntfs_set_invalid(error, "An NTFS index entry has an invalid file-name key.");
            return FALSE;
        }
        if (!ntfs_append_entry(walk, entry, error)) {
            return FALSE;
        }
        position += entry_length;
    }
}

GPtrArray *ntfs_list_directory(NtfsVolume *volume, guint64 record_number,
                               GError **error)
{
    if (volume == NULL) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                            "An NTFS volume is required.");
        return NULL;
    }

    GPtrArray *entries = g_ptr_array_new_with_free_func(ntfs_directory_entry_free);
    NtfsIndexWalk walk = { .volume = volume, .entries = entries };
    guint8 *record = NULL;
    GPtrArray *buffers = g_ptr_array_new_with_free_func(g_free);
    GPtrArray *fragments = g_ptr_array_new();
    const guint8 *value = NULL;
    guint32 value_length = 0;
    guint32 index_block_size = 0;
    guint32 first_entry = 0;
    guint32 total_size = 0;
    gboolean ok = FALSE;

    record = ntfs_read_file_record(volume, record_number, 0, error);
    if (record == NULL) {
        goto cleanup;
    }
    if ((read_le16(record + 22) & NTFS_RECORD_DIRECTORY) == 0) {
        ntfs_set_invalid(error, "The NTFS file record is not a directory.");
        goto cleanup;
    }


    if (!ntfs_locate_attribute(volume, record, record_number, NTFS_ATTR_INDEX_ROOT,
                               ntfs_i30_name, G_N_ELEMENTS(ntfs_i30_name),
                               buffers, fragments, error)) {
        goto cleanup;
    }
    if (fragments->len != 1) {
        ntfs_set_invalid(error, "The NTFS directory has no usable $I30 index.");
        goto cleanup;
    }
    if (!ntfs_resident_value(g_ptr_array_index(fragments, 0), &value, &value_length, error)) {
        goto cleanup;
    }


    if (value_length >= 32) {
        index_block_size = read_le32(value + 8);
        first_entry = read_le32(value + 16);
        total_size = read_le32(value + 20);
    }
    if (value_length < 32 || read_le32(value) != 0x30
        || index_block_size < 512 || index_block_size > NTFS_MAX_INDEX_BLOCK_SIZE
        || !is_power_of_two(index_block_size)
        || first_entry < 16 || first_entry > total_size
        || 16 + (guint64)total_size > value_length) {
        ntfs_set_invalid(error, "The NTFS directory index header is invalid.");
        goto cleanup;
    }

    if ((value[28] & 0x01) != 0) {

        g_ptr_array_set_size(fragments, 0);
        if (!ntfs_locate_attribute(volume, record, record_number,
                                   NTFS_ATTR_INDEX_ALLOCATION, ntfs_i30_name,
                                   G_N_ELEMENTS(ntfs_i30_name), buffers, fragments,
                                   error)) {
            goto cleanup;
        }
        if (!ntfs_load_runs(volume, (const guint8 *const *)fragments->pdata,
                            fragments->len, &walk.allocation, error)) {
            goto cleanup;
        }

        guint64 block_count = walk.allocation.data_size / index_block_size;
        if (block_count == 0 || block_count > NTFS_MAX_INDEX_BLOCKS) {
            ntfs_set_invalid(error, "The NTFS $INDEX_ALLOCATION size is invalid.");
            goto cleanup;
        }
        walk.block_count = block_count;
        walk.visited = g_malloc0(block_count);
        walk.index_block_size = index_block_size;


        walk.vcn_size = volume->cluster_size <= index_block_size
            ? volume->cluster_size : volume->bytes_per_sector;
    }

    ok = ntfs_walk_node(&walk, value, 16 + first_entry, 16 + total_size, 0, error);

cleanup:
    g_free(walk.visited);
    ntfs_run_list_clear(&walk.allocation);
    g_ptr_array_unref(fragments);
    g_ptr_array_unref(buffers);
    g_free(record);
    if (!ok) {
        g_ptr_array_unref(entries);
        return NULL;
    }
    return entries;
}

GPtrArray *ntfs_list_root(NtfsVolume *volume, GError **error)
{
    return ntfs_list_directory(volume, NTFS_ROOT_RECORD, error);
}

GBytes *ntfs_read_file(NtfsVolume *volume, guint64 record_number,
                       guint64 offset, gsize length, GError **error)
{
    if (volume == NULL || length == 0 || length > 16 * 1024 * 1024) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                            "The requested NTFS file read must be between 1 byte and 16 MiB.");
        return NULL;
    }

    guint8 *record = ntfs_read_file_record(volume, record_number, 0, error);
    if (record == NULL) {
        return NULL;
    }
    if ((read_le16(record + 22) & NTFS_RECORD_DIRECTORY) != 0) {
        g_free(record);
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                            "The selected NTFS record is a directory.");
        return NULL;
    }

    GPtrArray *buffers = g_ptr_array_new_with_free_func(g_free);
    GPtrArray *fragments = g_ptr_array_new();
    NtfsRunList runs = { 0 };
    GBytes *result = NULL;
    if (!ntfs_locate_attribute(volume, record, record_number, NTFS_ATTR_DATA,
                               NULL, 0, buffers, fragments, error)) {
        goto cleanup;
    }
    if (fragments->len == 0) {
        ntfs_set_invalid(error, "The NTFS file has no unnamed $DATA attribute.");
        goto cleanup;
    }

    const guint8 *first_attribute = g_ptr_array_index(fragments, 0);
    guint16 data_flags = read_le16(first_attribute + 12);
    if ((data_flags & 0x4001) != 0) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                            "Compressed or encrypted NTFS files are not supported.");
        goto cleanup;
    }
    guint64 file_size = 0;
    guint8 *contents = NULL;
    if (first_attribute[8] == 0) {
        const guint8 *value = NULL;
        guint32 value_length = 0;
        if (fragments->len != 1
            || !ntfs_resident_value(first_attribute, &value, &value_length, error)) {
            if (error != NULL && *error == NULL) {
                ntfs_set_invalid(error, "The NTFS resident file data is fragmented.");
            }
            goto cleanup;
        }
        file_size = value_length;
        if (offset > file_size || (guint64)length > file_size - offset) {
            g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                                "The requested NTFS file range is outside the file.");
            goto cleanup;
        }
        contents = g_memdup2(value + offset, length);
    } else {
        if (!ntfs_load_runs(volume, (const guint8 *const *)fragments->pdata,
                            fragments->len, &runs, error)) {
            goto cleanup;
        }
        file_size = runs.data_size;
        if (offset > file_size || (guint64)length > file_size - offset) {
            g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                                "The requested NTFS file range is outside the file.");
            goto cleanup;
        }
        contents = ntfs_read_runs(volume, &runs, offset, length, error);
        if (contents == NULL) {
            goto cleanup;
        }
    }
    result = g_bytes_new_take(contents, length);

cleanup:
    ntfs_run_list_clear(&runs);
    g_ptr_array_unref(fragments);
    g_ptr_array_unref(buffers);
    g_free(record);
    return result;
}