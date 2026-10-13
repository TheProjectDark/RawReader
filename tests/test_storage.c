#include "block_device.h"
#include "ext4.h"
#include "fat32.h"
#include "ntfs.h"

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

static void write_le64(guint8 *bytes, guint64 value)
{
    write_le32(bytes, (guint32)value);
    write_le32(bytes + 4, (guint32)(value >> 32));
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
    write_le32(fat + 12, 0x0FFFFFFF);
    write_le32(fat + 16, 0x0FFFFFFF);

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

    guint8 *directory_entry = root + 64;
    memcpy(directory_entry, "SUBDIR     ", 11);
    directory_entry[11] = 0x10;
    write_le16(directory_entry + 26, 4);
    root[96] = 0;

    guint8 *file_data = volume + 3 * 512;
    memcpy(file_data, "hello", 5);
    guint8 *subdirectory = volume + 4 * 512;
    memcpy(subdirectory, "INNER   TXT", 11);
    subdirectory[11] = 0x20;
    write_le16(subdirectory + 26, 3);
    write_le32(subdirectory + 28, 5);
    subdirectory[32] = 0;

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

static gchar *write_ext4_test_image(void)
{
    const gsize image_size = 16 * 1024;
    guint8 *image = g_malloc0(image_size);
    guint8 *superblock = image + 1024;
    write_le32(superblock + 0x00, 3);
    write_le32(superblock + 0x04, 16);
    write_le32(superblock + 0x14, 1);
    write_le32(superblock + 0x18, 0);
    write_le32(superblock + 0x20, 16);
    write_le32(superblock + 0x28, 16);
    write_le16(superblock + 0x38, 0xEF53);
    write_le32(superblock + 0x4C, 1);
    write_le16(superblock + 0x58, 128);
    write_le32(superblock + 0x60, 0x00040);

    guint8 *group_descriptor = image + 2 * 1024;
    write_le32(group_descriptor + 0x08, 3);

    guint8 *root_inode = image + 3 * 1024 + 128;
    write_le16(root_inode, 0x41ED);
    write_le32(root_inode + 0x04, 1024);
    write_le32(root_inode + 0x20, 0x00080000);
    write_le16(root_inode + 0x28, 0xF30A);
    write_le16(root_inode + 0x2A, 1);
    write_le16(root_inode + 0x2C, 4);
    write_le16(root_inode + 0x2E, 0);
    write_le32(root_inode + 0x34, 0);
    write_le16(root_inode + 0x38, 1);
    write_le32(root_inode + 0x3C, 5);

    guint8 *file_inode = image + 3 * 1024 + 2 * 128;
    write_le16(file_inode, 0x81A4);
    write_le32(file_inode + 0x04, 5);
    write_le32(file_inode + 0x20, 0x00080000);
    write_le16(file_inode + 0x28, 0xF30A);
    write_le16(file_inode + 0x2A, 1);
    write_le16(file_inode + 0x2C, 4);
    write_le16(file_inode + 0x2E, 0);
    write_le32(file_inode + 0x34, 0);
    write_le16(file_inode + 0x38, 1);
    write_le32(file_inode + 0x3C, 6);

    guint8 *directory = image + 5 * 1024;
    write_le32(directory, 2);
    write_le16(directory + 4, 12);
    directory[6] = 1;
    directory[7] = 2;
    directory[8] = '.';
    write_le32(directory + 12, 2);
    write_le16(directory + 16, 12);
    directory[18] = 2;
    directory[19] = 2;
    directory[20] = '.';
    directory[21] = '.';
    write_le32(directory + 24, 3);
    write_le16(directory + 28, 1000);
    directory[30] = 8;
    directory[31] = 1;
    memcpy(directory + 32, "file.txt", 8);
    memcpy(image + 6 * 1024, "hello", 5);

    gchar *path = NULL;
    gint descriptor = g_file_open_tmp("rawreader-ext4-XXXXXX", &path, NULL);
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

static void finish_ntfs_record(guint8 *record, guint32 used_size, guint64 number)
{
    memcpy(record, "FILE", 4);
    write_le16(record + 4, 0x30);
    write_le16(record + 6, 3);
    write_le16(record + 16, 1);
    write_le16(record + 18, 1);
    write_le16(record + 20, 0x38);
    write_le16(record + 22, 1);
    write_le32(record + 24, used_size);
    write_le32(record + 28, 1024);
    write_le64(record + 32, 0);
    write_le32(record + 44, (guint32)number);
    write_le16(record + 0x30, 0xA55A);
    write_le16(record + 0x32, 0x1234);
    write_le16(record + 0x34, 0x5678);
    write_le16(record + 510, 0xA55A);
    write_le16(record + 1022, 0xA55A);
}

static gchar *write_ntfs_test_image(void)
{
    const gsize image_size = 128 * 512;
    guint8 *image = g_malloc0(image_size);
    guint8 *boot = image;
    boot[0] = 0xEB;
    memcpy(boot + 3, "NTFS    ", 8);
    write_le16(boot + 11, 512);
    boot[13] = 1;
    write_le64(boot + 40, 128);
    write_le64(boot + 48, 4);
    boot[64] = (guint8)-10;
    boot[68] = 1;
    boot[510] = 0x55;
    boot[511] = 0xAA;

    guint8 *mft = image + 4 * 512;
    guint8 *mft_data = mft + 0x38;
    write_le32(mft_data, 0x80);
    write_le32(mft_data + 4, 72);
    mft_data[8] = 1;
    write_le64(mft_data + 16, 0);
    write_le64(mft_data + 24, 39);
    write_le16(mft_data + 32, 64);
    write_le64(mft_data + 40, 40 * 512);
    write_le64(mft_data + 48, 40 * 512);
    write_le64(mft_data + 56, 40 * 512);
    mft_data[64] = 0x11;
    mft_data[65] = 40;
    mft_data[66] = 4;
    mft_data[67] = 0;
    write_le32(mft_data + 72, 0xFFFFFFFF);
    finish_ntfs_record(mft, 132, 0);

    guint8 *root = image + 4 * 512 + 5 * 1024;
    guint8 *index_attribute = root + 0x38;
    write_le32(index_attribute, 0x90);
    write_le32(index_attribute + 4, 184);
    index_attribute[9] = 4;
    write_le16(index_attribute + 10, 24);
    write_le32(index_attribute + 16, 152);
    write_le16(index_attribute + 20, 32);
    static const gunichar2 index_name[] = { '$', 'I', '3', '0' };
    for (guint i = 0; i < G_N_ELEMENTS(index_name); ++i) {
        write_le16(index_attribute + 24 + i * 2, index_name[i]);
    }
    guint8 *index_root = index_attribute + 32;
    write_le32(index_root, 0x30);
    write_le32(index_root + 8, 512);
    write_le32(index_root + 16, 16);
    write_le32(index_root + 20, 136);
    guint8 *index_entry = index_root + 32;
    write_le64(index_entry, 16 | (G_GUINT64_CONSTANT(1) << 48));
    write_le16(index_entry + 8, 104);
    write_le16(index_entry + 10, 84);
    guint8 *file_name_key = index_entry + 16;
    write_le64(file_name_key + 48, 5);
    file_name_key[64] = 9;
    file_name_key[65] = 1;
    static const gunichar2 file_name[] = {
        'h', 'e', 'l', 'l', 'o', '.', 't', 'x', 't'
    };
    for (guint i = 0; i < G_N_ELEMENTS(file_name); ++i) {
        write_le16(file_name_key + 66 + i * 2, file_name[i]);
    }
    guint8 *last_entry = index_entry + 104;
    write_le16(last_entry + 8, 16);
    write_le16(last_entry + 12, 2);
    write_le32(index_attribute + 184, 0xFFFFFFFF);
    finish_ntfs_record(root, 244, 5);
    write_le16(root + 22, 3);

    guint8 *file_record = image + 4 * 512 + 16 * 1024;
    guint8 *data_attribute = file_record + 0x38;
    write_le32(data_attribute, 0x80);
    write_le32(data_attribute + 4, 32);
    write_le32(data_attribute + 16, 5);
    write_le16(data_attribute + 20, 24);
    memcpy(data_attribute + 24, "hello", 5);
    write_le32(data_attribute + 32, 0xFFFFFFFF);
    finish_ntfs_record(file_record, 92, 16);
    memcpy(image + 44 * 512, "MFT file data", 13);

    gchar *path = NULL;
    gint descriptor = g_file_open_tmp("rawreader-ntfs-XXXXXX", &path, NULL);
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
    g_assert_cmpuint(entries->len, ==, 2);
    Fat32DirectoryEntry *entry = g_ptr_array_index(entries, 0);
    g_assert_cmpstr(entry->name, ==, "Long file.txt");
    g_assert_false(entry->is_directory);
    g_assert_cmpuint(entry->size, ==, 5);
    g_assert_cmpuint(entry->first_cluster, ==, 3);

    GPtrArray *subentries = fat32_list_directory(volume, 4, &error);
    g_assert_no_error(error);
    g_assert_nonnull(subentries);
    g_assert_cmpuint(subentries->len, ==, 1);
    Fat32DirectoryEntry *subentry = g_ptr_array_index(subentries, 0);
    g_assert_cmpstr(subentry->name, ==, "INNER.TXT");
    GBytes *file = fat32_read_file(volume, subentry->first_cluster,
                                   subentry->size, 0, subentry->size, &error);
    g_assert_no_error(error);
    g_assert_nonnull(file);
    gsize file_length = 0;
    const gchar *file_contents = g_bytes_get_data(file, &file_length);
    g_assert_cmpuint(file_length, ==, 5);
    g_assert_cmpmem(file_contents, file_length, "hello", 5);

    g_bytes_unref(file);
    g_ptr_array_unref(subentries);
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

static void test_ext4_directory_and_file(void)
{
    gchar *path = write_ext4_test_image();
    GError *error = NULL;
    BlockDevice *device = block_device_open(path, &error);
    g_assert_no_error(error);
    Ext4Volume *volume = ext4_volume_open(device, &error);
    g_assert_no_error(error);
    g_assert_nonnull(volume);

    GPtrArray *entries = ext4_list_root(volume, &error);
    g_assert_no_error(error);
    g_assert_nonnull(entries);
    g_assert_cmpuint(entries->len, ==, 1);
    Ext4DirectoryEntry *entry = g_ptr_array_index(entries, 0);
    g_assert_cmpstr(entry->name, ==, "file.txt");
    g_assert_false(entry->is_directory);
    g_assert_cmpuint(entry->inode_number, ==, 3);

    GBytes *file = ext4_read_file(volume, entry->inode_number,
                                  0, (gsize)entry->size, &error);
    g_assert_no_error(error);
    g_assert_nonnull(file);
    gsize file_length = 0;
    const gchar *contents = g_bytes_get_data(file, &file_length);
    g_assert_cmpuint(file_length, ==, 5);
    g_assert_cmpmem(contents, file_length, "hello", 5);

    g_bytes_unref(file);
    g_ptr_array_unref(entries);
    ext4_volume_close(volume);
    block_device_close(device);
    g_assert_cmpint(unlink(path), ==, 0);
    g_free(path);
}

static void test_ntfs_directory_and_file(void)
{
    gchar *path = write_ntfs_test_image();
    GError *error = NULL;
    BlockDevice *device = block_device_open(path, &error);
    g_assert_no_error(error);
    NtfsVolume *volume = ntfs_volume_open(device, &error);
    g_assert_no_error(error);
    g_assert_nonnull(volume);

    GPtrArray *entries = ntfs_list_root(volume, &error);
    g_assert_no_error(error);
    g_assert_nonnull(entries);
    g_assert_cmpuint(entries->len, ==, 1);
    NtfsDirectoryEntry *entry = g_ptr_array_index(entries, 0);
    g_assert_cmpstr(entry->name, ==, "hello.txt");
    g_assert_false(entry->is_directory);
    g_assert_cmpuint(entry->record_number, ==, 16);

    GBytes *file = ntfs_read_file(volume, entry->record_number, 0,
                                  (gsize)entry->size, &error);
    g_assert_no_error(error);
    g_assert_nonnull(file);
    gsize file_length = 0;
    const gchar *contents = g_bytes_get_data(file, &file_length);
    g_assert_cmpuint(file_length, ==, 5);
    g_assert_cmpmem(contents, file_length, "hello", 5);

    g_bytes_unref(file);
    g_ptr_array_unref(entries);
    ntfs_volume_close(volume);
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
    g_test_add_func("/storage/ext4/directory-file", test_ext4_directory_and_file);
    g_test_add_func("/storage/ntfs/directory-file", test_ntfs_directory_and_file);
    return g_test_run();
}
