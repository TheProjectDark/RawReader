#define _POSIX_C_SOURCE 200809L

#include "block_device.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winioctl.h>
#include <ntddstor.h>
#else
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#ifdef __APPLE__
#include <sys/disk.h>
#else
#include <linux/fs.h>
#include <sys/ioctl.h>
#endif
#endif

#define BLOCK_DEVICE_MAX_READ_SIZE (1024 * 1024)

struct _BlockDevice {
    gchar *path;
    guint64 size;
    guint32 sector_size;
#ifdef _WIN32
    HANDLE handle;
#else
    int descriptor;
#endif
};

#ifdef _WIN32
static void set_windows_error(GError **error, DWORD code, const gchar *path)
{
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                "%s: Windows error %lu.", path, (unsigned long)code);
}
#else
static void set_errno_error(GError **error, int saved_errno, const gchar *path)
{
    g_set_error(error, G_FILE_ERROR, g_file_error_from_errno(saved_errno),
                "%s: %s", path, g_strerror(saved_errno));
}
#endif

BlockDevice *block_device_open(const gchar *path, GError **error)
{
    if (path == NULL || path[0] == '\0') {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                            "A block-device path is required.");
        return NULL;
    }

    BlockDevice *device = g_new0(BlockDevice, 1);
    device->path = g_strdup(path);
    device->sector_size = 512;

#ifdef _WIN32
    device->handle = INVALID_HANDLE_VALUE;
    gunichar2 *wide_path = g_utf8_to_utf16(path, -1, NULL, NULL, error);
    if (wide_path == NULL) {
        g_free(device->path);
        g_free(device);
        return NULL;
    }
    device->handle = CreateFileW((LPCWSTR)wide_path, GENERIC_READ,
                                 FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                 NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    g_free(wide_path);
    if (device->handle == INVALID_HANDLE_VALUE) {
        set_windows_error(error, GetLastError(), path);
        g_free(device->path);
        g_free(device);
        return NULL;
    }

    GET_LENGTH_INFORMATION length_info;
    DWORD bytes_returned = 0;
    if (DeviceIoControl(device->handle, IOCTL_DISK_GET_LENGTH_INFO, NULL, 0,
                        &length_info, sizeof(length_info), &bytes_returned, NULL)) {
        device->size = (guint64)length_info.Length.QuadPart;
    } else {
        LARGE_INTEGER file_size;
        if (GetFileSizeEx(device->handle, &file_size)) {
            device->size = (guint64)file_size.QuadPart;
        }
    }

    STORAGE_PROPERTY_QUERY query = { 0 };
    STORAGE_ACCESS_ALIGNMENT_DESCRIPTOR alignment = { 0 };
    query.PropertyId = StorageAccessAlignmentProperty;
    query.QueryType = PropertyStandardQuery;
    if (DeviceIoControl(device->handle, IOCTL_STORAGE_QUERY_PROPERTY, &query,
                        sizeof(query), &alignment, sizeof(alignment),
                        &bytes_returned, NULL)
        && bytes_returned >= sizeof(alignment)
        && alignment.BytesPerLogicalSector != 0) {
        device->sector_size = alignment.BytesPerLogicalSector;
    }
#else
    device->descriptor = open(path, O_RDONLY | O_CLOEXEC);
    if (device->descriptor == -1) {
        int saved_errno = errno;
        set_errno_error(error, saved_errno, path);
        g_free(device->path);
        g_free(device);
        return NULL;
    }

    struct stat status;
    if (fstat(device->descriptor, &status) != 0) {
        int saved_errno = errno;
        set_errno_error(error, saved_errno, path);
        close(device->descriptor);
        g_free(device->path);
        g_free(device);
        return NULL;
    }

    if (S_ISREG(status.st_mode)) {
        if (status.st_size < 0) {
            g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                        "%s has an invalid size.", path);
            close(device->descriptor);
            g_free(device->path);
            g_free(device);
            return NULL;
        }
        device->size = (guint64)status.st_size;
    } else {
#ifdef __APPLE__
        uint32_t sector_size = 0;
        uint64_t sector_count = 0;
        if (ioctl(device->descriptor, DKIOCGETBLOCKSIZE, &sector_size) != 0
            || ioctl(device->descriptor, DKIOCGETBLOCKCOUNT, &sector_count) != 0) {
            int saved_errno = errno;
            set_errno_error(error, saved_errno, path);
            close(device->descriptor);
            g_free(device->path);
            g_free(device);
            return NULL;
        }
        if (sector_size == 0 || sector_count > G_MAXUINT64 / sector_size) {
            g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                        "%s reports invalid block geometry.", path);
            close(device->descriptor);
            g_free(device->path);
            g_free(device);
            return NULL;
        }
        device->sector_size = sector_size;
        device->size = sector_count * sector_size;
#else
        unsigned int sector_size = 0;
        unsigned long long byte_size = 0;
        if (ioctl(device->descriptor, BLKSSZGET, &sector_size) != 0
            || ioctl(device->descriptor, BLKGETSIZE64, &byte_size) != 0) {
            int saved_errno = errno;
            set_errno_error(error, saved_errno, path);
            close(device->descriptor);
            g_free(device->path);
            g_free(device);
            return NULL;
        }
        if (sector_size == 0) {
            g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                        "%s reports an invalid logical sector size.", path);
            close(device->descriptor);
            g_free(device->path);
            g_free(device);
            return NULL;
        }
        device->sector_size = sector_size;
        device->size = (guint64)byte_size;
#endif
    }
#endif

    if (device->size == 0 || device->sector_size == 0) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                    "%s reports an empty device or an invalid sector size.", path);
#ifdef _WIN32
        CloseHandle(device->handle);
#else
        close(device->descriptor);
#endif
        g_free(device->path);
        g_free(device);
        return NULL;
    }

    return device;
}

void block_device_close(BlockDevice *device)
{
    if (device == NULL) {
        return;
    }
#ifdef _WIN32
    CloseHandle(device->handle);
#else
    close(device->descriptor);
#endif
    g_free(device->path);
    g_free(device);
}

guint64 block_device_get_size(const BlockDevice *device)
{
    return device->size;
}

guint32 block_device_get_sector_size(const BlockDevice *device)
{
    return device->sector_size;
}

GBytes *block_device_read(BlockDevice *device, guint64 offset, gsize length,
                          GError **error)
{
    if (device == NULL || length == 0 || length > BLOCK_DEVICE_MAX_READ_SIZE) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                            "Read size must be between 1 byte and 1 MiB.");
        return NULL;
    }
    if (offset > device->size || (guint64)length > device->size - offset) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                            "The requested read is outside the device.");
        return NULL;
    }
    if (offset > G_MAXINT64 || (guint64)length > G_MAXINT64 - offset) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                            "The requested read exceeds the supported offset range.");
        return NULL;
    }

    guint8 *buffer = g_malloc(length);
    gsize total_read = 0;
#ifdef _WIN32
    LARGE_INTEGER position;
    position.QuadPart = (LONGLONG)offset;
    if (!SetFilePointerEx(device->handle, position, NULL, FILE_BEGIN)) {
        DWORD code = GetLastError();
        g_free(buffer);
        set_windows_error(error, code, device->path);
        return NULL;
    }
    while (total_read < length) {
        DWORD chunk = (DWORD)MIN(length - total_read, (gsize)G_MAXUINT32);
        DWORD bytes_read = 0;
        if (!ReadFile(device->handle, buffer + total_read, chunk, &bytes_read, NULL)) {
            DWORD code = GetLastError();
            g_free(buffer);
            set_windows_error(error, code, device->path);
            return NULL;
        }
        if (bytes_read == 0) {
            break;
        }
        total_read += bytes_read;
    }
#else
    while (total_read < length) {
        gsize remaining = length - total_read;
        gsize chunk = MIN(remaining, (gsize)SSIZE_MAX);
        ssize_t count = pread(device->descriptor, buffer + total_read, chunk,
                              (off_t)(offset + total_read));
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count < 0) {
            int saved_errno = errno;
            g_free(buffer);
            set_errno_error(error, saved_errno, device->path);
            return NULL;
        }
        if (count == 0) {
            break;
        }
        total_read += (gsize)count;
    }
#endif

    if (total_read != length) {
        g_free(buffer);
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_PARTIAL_INPUT,
                    "%s returned %zu of %zu requested bytes.",
                    device->path, total_read, length);
        return NULL;
    }
    return g_bytes_new_take(buffer, total_read);
}
