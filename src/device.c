#include "device.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <setupapi.h>
#include <ntddstor.h>
#include <winioctl.h>
#endif

#ifdef __linux__
#include <sys/stat.h>
#endif

void raw_device_free(gpointer data)
{
    RawDevice *device = data;
    if (device == NULL) {
        return;
    }
    g_free(device->path);
    g_free(device->label);
    g_free(device);
}

static void add_device(GPtrArray *devices, const gchar *path, const gchar *label)
{
    RawDevice *device = g_new0(RawDevice, 1);
    device->path = g_strdup(path);
    device->label = g_strdup(label);
    g_ptr_array_add(devices, device);
}

#ifdef __APPLE__
static gboolean discover_macos(GPtrArray *devices, GError **error)
{
    gchar *standard_output = NULL;
    gchar *standard_error = NULL;
    gint exit_status = 0;
    gchar *arguments[] = { (gchar *)"/usr/sbin/diskutil", (gchar *)"list", NULL };

    if (!g_spawn_sync(NULL, arguments, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL,
                      &standard_output, &standard_error, &exit_status, error)) {
        g_free(standard_output);
        g_free(standard_error);
        return FALSE;
    }

    if (!g_spawn_check_wait_status(exit_status, error)) {
        if (standard_error != NULL && standard_error[0] != '\0') {
            g_clear_error(error);
            g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                        "diskutil list: %s", standard_error);
        }
        g_free(standard_output);
        g_free(standard_error);
        return FALSE;
    }

    GRegex *pattern = g_regex_new("^/dev/(disk[0-9]+) \\(([^)]*physical[^)]*)\\):",
                                  G_REGEX_CASELESS, 0, error);
    if (pattern == NULL) {
        g_free(standard_output);
        g_free(standard_error);
        return FALSE;
    }

    gchar **lines = g_strsplit(standard_output, "\n", -1);
    for (gsize i = 0; lines[i] != NULL; ++i) {
        gchar *line = g_strstrip(lines[i]);
        GMatchInfo *match = NULL;
        if (g_regex_match(pattern, line, 0, &match)) {
            gchar *disk_name = g_match_info_fetch(match, 1);
            gchar *kind = g_match_info_fetch(match, 2);
            gchar *raw_path = g_strdup_printf("/dev/r%s", disk_name);
            gchar *label = g_strdup_printf("/dev/%s (%s)", disk_name, kind);
            add_device(devices, raw_path, label);
            g_free(label);
            g_free(raw_path);
            g_free(kind);
            g_free(disk_name);
        }
        if (match != NULL) {
            g_match_info_free(match);
        }
    }

    g_strfreev(lines);
    g_regex_unref(pattern);
    g_free(standard_output);
    g_free(standard_error);
    return TRUE;
}
#elif defined(__linux__)
static gboolean discover_linux(GPtrArray *devices, GError **error)
{
    GDir *directory = g_dir_open("/sys/block", 0, error);
    if (directory == NULL) {
        return FALSE;
    }

    const gchar *name;
    while ((name = g_dir_read_name(directory)) != NULL) {
        if (g_str_has_prefix(name, "loop") || g_str_has_prefix(name, "ram")
            || g_str_has_prefix(name, "zram")) {
            continue;
        }

        gchar *path = g_strdup_printf("/dev/%s", name);
        struct stat device_stat;
        if (stat(path, &device_stat) == 0 && S_ISBLK(device_stat.st_mode)) {
            gchar *label = g_strdup_printf("%s — block device", path);
            add_device(devices, path, label);
            g_free(label);
        }
        g_free(path);
    }
    g_dir_close(directory);
    return TRUE;
}
#elif defined(_WIN32)
static gchar *device_friendly_name(HDEVINFO device_set, SP_DEVINFO_DATA *device_info)
{
    WCHAR name[512] = { 0 };
    DWORD property_type = 0;
    DWORD required_size = 0;

    if (SetupDiGetDeviceRegistryPropertyW(device_set, device_info,
                                         SPDRP_FRIENDLYNAME, &property_type,
                                         (PBYTE)name, sizeof(name), &required_size)
        || SetupDiGetDeviceRegistryPropertyW(device_set, device_info,
                                             SPDRP_DEVICEDESC, &property_type,
                                             (PBYTE)name, sizeof(name), &required_size)) {
        return g_utf16_to_utf8((const gunichar2 *)name, -1, NULL, NULL, NULL);
    }

    return g_strdup("Physical disk");
}

static gboolean discover_windows(GPtrArray *devices, GError **error)
{
    HDEVINFO device_set = SetupDiGetClassDevsW(&GUID_DEVINTERFACE_DISK, NULL, NULL,
                                               DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (device_set == INVALID_HANDLE_VALUE) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                    "Could not enumerate Windows disks (error %lu).",
                    (unsigned long)GetLastError());
        return FALSE;
    }

    for (DWORD index = 0;; ++index) {
        SP_DEVICE_INTERFACE_DATA interface_data = { 0 };
        interface_data.cbSize = sizeof(interface_data);

        if (!SetupDiEnumDeviceInterfaces(device_set, NULL, &GUID_DEVINTERFACE_DISK,
                                         index, &interface_data)) {
            DWORD win_error = GetLastError();
            if (win_error == ERROR_NO_MORE_ITEMS) {
                break;
            }
            g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                        "Failed to enumerate Windows disks (error %lu).",
                        (unsigned long)win_error);
            SetupDiDestroyDeviceInfoList(device_set);
            return FALSE;
        }

        DWORD required_size = 0;
        SP_DEVINFO_DATA device_info = { 0 };
        device_info.cbSize = sizeof(device_info);
        SetupDiGetDeviceInterfaceDetailW(device_set, &interface_data, NULL, 0,
                                         &required_size, &device_info);
        if (required_size < sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W)) {
            continue;
        }

        SP_DEVICE_INTERFACE_DETAIL_DATA_W *detail = g_malloc0(required_size);
        detail->cbSize = sizeof(*detail);
        device_info.cbSize = sizeof(device_info);
        if (!SetupDiGetDeviceInterfaceDetailW(device_set, &interface_data, detail,
                                              required_size, NULL, &device_info)) {
            g_free(detail);
            continue;
        }

        gchar *path = g_utf16_to_utf8((const gunichar2 *)detail->DevicePath,
                                      -1, NULL, NULL, NULL);
        gchar *friendly_name = device_friendly_name(device_set, &device_info);
        if (path != NULL && friendly_name != NULL) {
            gchar *label = g_strdup_printf("%s — %s", friendly_name, path);
            add_device(devices, path, label);
            g_free(label);
        }
        g_free(friendly_name);
        g_free(path);
        g_free(detail);
    }

    SetupDiDestroyDeviceInfoList(device_set);
    return TRUE;
}
#endif

GPtrArray *device_list_discover(GError **error)
{
    GPtrArray *devices = g_ptr_array_new_with_free_func(raw_device_free);
    gboolean success;

#ifdef __APPLE__
    success = discover_macos(devices, error);
#elif defined(__linux__)
    success = discover_linux(devices, error);
#elif defined(_WIN32)
    success = discover_windows(devices, error);
#else
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                        "Device discovery is not supported on this platform.");
    success = FALSE;
#endif

    if (!success) {
        g_ptr_array_unref(devices);
        return NULL;
    }
    if (devices->len == 0) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                            "No physical devices were found.");
        g_ptr_array_unref(devices);
        return NULL;
    }
    return devices;
}
