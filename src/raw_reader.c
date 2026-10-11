#define _POSIX_C_SOURCE 200809L

#include "raw_reader.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <sys/types.h>
#include <unistd.h>
#endif

#define RAW_READER_MAX_READ_SIZE (1024 * 1024)

#ifdef _WIN32
static void set_windows_error(GError **error, DWORD code, const gchar *path)
{
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                "%s: Windows error %lu.", path, (unsigned long)code);
}
#else
static void set_errno_error(GError **error, gint saved_errno, const gchar *path)
{
    g_set_error(error, G_FILE_ERROR, g_file_error_from_errno(saved_errno),
                "%s: %s", path, g_strerror(saved_errno));
}
#endif

GBytes *raw_reader_read(const gchar *device_path, guint64 offset, gsize length,
                        GError **error)
{
    if (device_path == NULL || length == 0 || length > RAW_READER_MAX_READ_SIZE) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                            "Read size must be between 1 byte and 1 MiB.");
        return NULL;
    }
    if (offset > G_MAXINT64 || (guint64)length > G_MAXINT64 - offset) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                            "Offset and read size exceed the supported range.");
        return NULL;
    }

#ifdef _WIN32
    gunichar2 *wide_path = g_utf8_to_utf16(device_path, -1, NULL, NULL, error);
    if (wide_path == NULL) {
        return NULL;
    }

    HANDLE handle = CreateFileW((LPCWSTR)wide_path, GENERIC_READ,
                                FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                                OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    g_free(wide_path);
    if (handle == INVALID_HANDLE_VALUE) {
        set_windows_error(error, GetLastError(), device_path);
        return NULL;
    }

    LARGE_INTEGER position;
    position.QuadPart = (LONGLONG)offset;
    if (!SetFilePointerEx(handle, position, NULL, FILE_BEGIN)) {
        DWORD win_error = GetLastError();
        CloseHandle(handle);
        set_windows_error(error, win_error, device_path);
        return NULL;
    }

    guint8 *buffer = g_malloc(length);
    gsize total_read = 0;
    while (total_read < length) {
        DWORD bytes_to_read = (DWORD)MIN(length - total_read, (gsize)G_MAXUINT32);
        DWORD bytes_read = 0;
        if (!ReadFile(handle, buffer + total_read, bytes_to_read, &bytes_read, NULL)) {
            DWORD win_error = GetLastError();
            CloseHandle(handle);
            g_free(buffer);
            set_windows_error(error, win_error, device_path);
            return NULL;
        }
        if (bytes_read == 0) {
            break;
        }
        total_read += bytes_read;
    }

    if (!CloseHandle(handle)) {
        DWORD win_error = GetLastError();
        g_free(buffer);
        set_windows_error(error, win_error, device_path);
        return NULL;
    }
#else
    int descriptor = open(device_path, O_RDONLY | O_CLOEXEC);
    if (descriptor == -1) {
        set_errno_error(error, errno, device_path);
        return NULL;
    }

    guint8 *buffer = g_malloc(length);
    gsize total_read = 0;
    while (total_read < length) {
        gsize remaining = length - total_read;
        gsize chunk_size = MIN(remaining, (gsize)SSIZE_MAX);
        ssize_t count = pread(descriptor, buffer + total_read, chunk_size,
                              (off_t)(offset + total_read));
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count < 0) {
            int read_errno = errno;
            close(descriptor);
            g_free(buffer);
            set_errno_error(error, read_errno, device_path);
            return NULL;
        }
        if (count == 0) {
            break;
        }
        total_read += (gsize)count;
    }

    if (close(descriptor) != 0) {
        int close_errno = errno;
        g_free(buffer);
        set_errno_error(error, close_errno, device_path);
        return NULL;
    }
#endif

    return g_bytes_new_take(buffer, total_read);
}
