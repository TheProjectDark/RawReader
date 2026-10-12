#include "block_device.h"
#include "fat32.h"

#include <string.h>
#include <unistd.h>

static void write_le16(guint8 *bytes, guint16 value)
{
    bytes[0] = (guint8)value;
    bytes[1] = (guint8)(value >> 8);
}

static void write_le32(guint8 *bytes, guint32 value)
{
    bytes[0] = (guint8)value;
    bytes[1] = (guint8)(value >> 8);
    bytes[2] = (guint8)(value >> 16);
    bytes[3] = (guint8)(value >> 24);
}

static guint8 short_name_checksum(const guint8 name[11])
{
    guint8 checksum = 0;
    for (guint i = 0; i < 11; ++i) {
        checksum = (guint8)(((checksum & 1) << 7) + (checksum >> 1) + name[i]);
    }
    return checksum;
}

static void set_fat32_boot_sector(guint8 *boot)
{
    boot[0] = 0xEB;
    boot[1] = 0x58;
    boot[2] = 0x90;
    memcpy(boot + 3, "MSDOS5.0", 8);
    write_le16(boot + 11, 512);
    boot[13] = 1;
    write_le16(boot + 14, 1);
    boot[16] = 1;
    write_le32(boot + 32, 128);
    write_le32(boot + 36, 1);
    write_le32(boot + 44, 2);
    boot[510] = 0x55;
    boot[511] = 0xAA;
}

static gchar *write_test_image(gboolean partitioned)
{
    const gsize image_size = (partitioned ? 129 : 128) * 512;
    guint8 *image = g_malloc0(image_size);
    guint8 *volume = image + (partitioned ? 512 : 0);
    set_fat32_boot_sector(volume);

    guint8 *fat = volume + 512;
    write_le32(fat, 0x0FFFFFF8);
    write_le32(fat + 4, 0x0FFFFFFF);
    write_le32(fat + 8, 0x0FFFFFFF);

    guint8 *root = volume + 2 * 512;
    static const guint8 short_name[11] = {
        'L', 'O', 'N', 'G', 'F', 'I', '~', '1', 'T', 'X', 'T'
    };
    guint8 *long_entry = root;
    long_entry[0] = 0x41;
    long_entry[11] = 0x0F;
    long_entry[13] = short_name_checksum(short_name);
    static const guint8 offsets[] = { 1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30 };
    static const gunichar2 long_name[] = {
        'L', 'o', 'n', 'g', ' ', 'f', 'i', 'l', 'e', '.', 't', 'x', 't'
    };
    for (guint i = 0; i < G_N_ELEMENTS(long_name); ++i) {
        write_le16(long_entry + offsets[i], long_name[i]);
    }

    guint8 *short_entry = root + 32;
    memcpy(short_entry, short_name, sizeof(short_name));
    short_entry[11] = 0x20;
    write_le16(short_entry + 26, 3);
    write_le32(short_entry + 28, 5);
    root[64] = 0;

    if (partitioned) {
        image[446 + 4] = 0x0C;
        write_le32(image + 446 + 8, 1);
        write_le32(image + 446 + 12, 128);
        image[510] = 0x55;
        image[511] = 0xAA;
    }

    gchar *path = NULL;
    gint descriptor = g_file_open_tmp("rawreader-fat32-XXXXXX", &path, NULL);
    g_assert_cmpint(descriptor, >=, 0);
    gsize written = 0;
    while (written < image_size) {
        ssize_t count = write(descriptor, image + written, image_size - written);
        g_assert_cmpint(count, >, 0);
        written += (gsize)count;
    }
    g_assert_cmpint(close(descriptor), ==, 0);
    g_free(image);
    return path;
}

static void test_block_device_bounds(void)
{
    gchar *path = write_test_image(FALSE);
    GError *error = NULL;
    BlockDevice *device = block_device_open(path, &error);
    g_assert_no_error(error);
    g_assert_nonnull(device);
    g_assert_cmpuint(block_device_get_size(device), ==, 128 * 512);
    g_assert_cmpuint(block_device_get_sector_size(device), ==, 512);

    GBytes *bytes = block_device_read(device, 0, 512, &error);
    g_assert_no_error(error);
    g_assert_nonnull(bytes);
    g_bytes_unref(bytes);

    bytes = block_device_read(device, 128 * 512 - 1, 2, &error);
    g_assert_null(bytes);
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
    g_clear_error(&error);
    block_device_close(device);
    g_assert_cmpint(unlink(path), ==, 0);
    g_free(path);
}

static void test_fat32_superfloppy(void)
{
    gchar *path = write_test_image(FALSE);
    GError *error = NULL;
    BlockDevice *device = block_device_open(path, &error);
    g_assert_no_error(error);
    Fat32Volume *volume = fat32_volume_open(device, &error);
    g_assert_no_error(error);
    g_assert_nonnull(volume);

    Fat32Info info = fat32_volume_get_info(volume);
    g_assert_cmpuint(info.bytes_per_sector, ==, 512);
    g_assert_cmpuint(info.root_cluster, ==, 2);
    g_assert_cmpuint(info.volume_size, ==, 128 * 512);
    GPtrArray *entries = fat32_list_root(volume, &error);
    g_assert_no_error(error);
    g_assert_nonnull(entries);
    g_assert_cmpuint(entries->len, ==, 1);
    Fat32DirectoryEntry *entry = g_ptr_array_index(entries, 0);
    g_assert_cmpstr(entry->name, ==, "Long file.txt");
    g_assert_false(entry->is_directory);
    g_assert_cmpuint(entry->size, ==, 5);

    g_ptr_array_unref(entries);
    fat32_volume_close(volume);
    block_device_close(device);
    g_assert_cmpint(unlink(path), ==, 0);
    g_free(path);
}

static void test_fat32_mbr_partition(void)
{
    gchar *path = write_test_image(TRUE);
    GError *error = NULL;
    BlockDevice *device = block_device_open(path, &error);
    g_assert_no_error(error);
    Fat32Volume *volume = fat32_volume_open(device, &error);
    g_assert_no_error(error);
    g_assert_nonnull(volume);
    g_assert_cmpuint(fat32_volume_get_info(volume).partition_offset, ==, 512);

    fat32_volume_close(volume);
    block_device_close(device);
    g_assert_cmpint(unlink(path), ==, 0);
    g_free(path);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/storage/block-device/bounds", test_block_device_bounds);
    g_test_add_func("/storage/fat32/superfloppy", test_fat32_superfloppy);
    g_test_add_func("/storage/fat32/mbr-partition", test_fat32_mbr_partition);
    return g_test_run();
}
