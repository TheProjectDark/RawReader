#include "app.h"

#include "block_device.h"
#include "device.h"
#include "ext4.h"
#include "fat32.h"
#include "hex_dump.h"
#include "ntfs.h"
#include "raw_reader.h"

#include <errno.h>
#include <string.h>

#define DEFAULT_READ_SIZE 512
#define MAX_READ_SIZE (64 * 1024)
#define BROWSER_MAX_PREVIEW_SIZE (1024 * 1024)

typedef enum {
    BROWSER_FS_FAT32,
    BROWSER_FS_NTFS,
    BROWSER_FS_EXT4
} BrowserFileSystem;

typedef struct {
    gchar *name;
    gboolean is_directory;
    guint64 size;
    guint64 id;
} BrowserEntry;

typedef struct {
    BrowserFileSystem file_system;
    BlockDevice *device;
    union {
        Fat32Volume *fat32;
        NtfsVolume *ntfs;
        Ext4Volume *ext4;
    } volume;
    GtkWidget *window;
    GtkWidget *path_label;
    GtkWidget *file_list;
    GtkWidget *preview;
    GtkWidget *status;
    GPtrArray *entries;
    GPtrArray *path_components;
    GArray *directory_ids;
} FileBrowser;

typedef struct {
    GtkWidget *window;
    GtkWidget *device_dropdown;
    GtkWidget *offset_entry;
    GtkWidget *length_spin;
    GtkWidget *output_view;
    GtkWidget *status_label;
    GPtrArray *devices;
} AppWidgets;

static void app_widgets_free(gpointer data)
{
    AppWidgets *widgets = data;
    if (widgets->devices != NULL) {
        g_ptr_array_unref(widgets->devices);
    }
    g_free(widgets);
}

static void set_status(AppWidgets *widgets, const gchar *message, gboolean is_error)
{
    gtk_label_set_text(GTK_LABEL(widgets->status_label), message);
    gtk_widget_set_name(widgets->status_label, is_error ? "error-status" : "status");
}

static void browser_entry_free(gpointer data)
{
    BrowserEntry *entry = data;
    if (entry != NULL) {
        g_free(entry->name);
        g_free(entry);
    }
}

static void file_browser_free(gpointer data)
{
    FileBrowser *browser = data;
    if (browser->entries != NULL) {
        g_ptr_array_unref(browser->entries);
    }
    g_ptr_array_unref(browser->path_components);
    g_array_unref(browser->directory_ids);
    switch (browser->file_system) {
    case BROWSER_FS_FAT32:
        fat32_volume_close(browser->volume.fat32);
        break;
    case BROWSER_FS_NTFS:
        ntfs_volume_close(browser->volume.ntfs);
        break;
    case BROWSER_FS_EXT4:
        ext4_volume_close(browser->volume.ext4);
        break;
    }
    block_device_close(browser->device);
    g_free(browser);
}

static void browser_set_status(FileBrowser *browser, const gchar *message,
                               gboolean is_error)
{
    gtk_label_set_text(GTK_LABEL(browser->status), message);
    gtk_widget_set_name(browser->status, is_error ? "error-status" : "status");
}

static GPtrArray *browser_list_directory(FileBrowser *browser, guint64 directory_id,
                                         GError **error)
{
    GPtrArray *source = NULL;
    GPtrArray *entries = g_ptr_array_new_with_free_func(browser_entry_free);

    switch (browser->file_system) {
    case BROWSER_FS_FAT32:
        source = fat32_list_directory(browser->volume.fat32,
                                      (guint32)directory_id, error);
        break;
    case BROWSER_FS_NTFS:
        source = ntfs_list_directory(browser->volume.ntfs, directory_id, error);
        break;
    case BROWSER_FS_EXT4:
        source = ext4_list_directory(browser->volume.ext4,
                                     (guint32)directory_id, error);
        break;
    }
    if (source == NULL) {
        g_ptr_array_unref(entries);
        return NULL;
    }

    for (guint i = 0; i < source->len; ++i) {
        BrowserEntry *item = g_new0(BrowserEntry, 1);
        switch (browser->file_system) {
        case BROWSER_FS_FAT32: {
            Fat32DirectoryEntry *entry = g_ptr_array_index(source, i);
            item->name = g_strdup(entry->name);
            item->is_directory = entry->is_directory;
            item->size = entry->size;
            item->id = entry->first_cluster;
            break;
        }
        case BROWSER_FS_NTFS: {
            NtfsDirectoryEntry *entry = g_ptr_array_index(source, i);
            item->name = g_strdup(entry->name);
            item->is_directory = entry->is_directory;
            item->size = entry->size;
            item->id = entry->record_number;
            break;
        }
        case BROWSER_FS_EXT4: {
            Ext4DirectoryEntry *entry = g_ptr_array_index(source, i);
            item->name = g_strdup(entry->name);
            item->is_directory = entry->is_directory;
            item->size = entry->size;
            item->id = entry->inode_number;
            break;
        }
        }
        g_ptr_array_add(entries, item);
    }
    g_ptr_array_unref(source);
    return entries;
}

static GBytes *browser_read_file(FileBrowser *browser, const BrowserEntry *entry,
                                 gsize length, GError **error)
{
    switch (browser->file_system) {
    case BROWSER_FS_FAT32:
        return fat32_read_file(browser->volume.fat32, (guint32)entry->id,
                               entry->size, 0, length, error);
    case BROWSER_FS_NTFS:
        return ntfs_read_file(browser->volume.ntfs, entry->id, 0, length, error);
    case BROWSER_FS_EXT4:
        return ext4_read_file(browser->volume.ext4, (guint32)entry->id,
                              0, length, error);
    }
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                        "The mounted filesystem is not supported.");
    return NULL;
}

static void browser_update_path(FileBrowser *browser)
{
    GString *path = g_string_new("/");
    for (guint i = 0; i < browser->path_components->len; ++i) {
        if (i > 0) {
            g_string_append_c(path, '/');
        }
        g_string_append(path, g_ptr_array_index(browser->path_components, i));
    }
    gtk_label_set_text(GTK_LABEL(browser->path_label), path->str);
    g_string_free(path, TRUE);
}

static void browser_show_preview(FileBrowser *browser, BrowserEntry *entry)
{
    GtkTextBuffer *text_buffer =
        gtk_text_view_get_buffer(GTK_TEXT_VIEW(browser->preview));
    gtk_text_buffer_set_text(text_buffer, "", 0);
    if (entry->size == 0) {
        gtk_text_buffer_set_text(text_buffer, "(empty file)", -1);
        browser_set_status(browser, "File is empty.", FALSE);
        return;
    }

    gsize length = (gsize)MIN(entry->size, (guint64)BROWSER_MAX_PREVIEW_SIZE);
    GError *error = NULL;
    GBytes *bytes = browser_read_file(browser, entry, length, &error);
    if (bytes == NULL) {
        browser_set_status(browser, error->message, TRUE);
        g_clear_error(&error);
        return;
    }

    gsize actual_length = 0;
    const guint8 *data = g_bytes_get_data(bytes, &actual_length);
    gchar *text = NULL;
    if (g_utf8_validate((const gchar *)data, actual_length, NULL)
        && memchr(data, '\0', actual_length) == NULL) {
        text = g_strndup((const gchar *)data, actual_length);
    } else {
        text = hex_dump_format(data, actual_length, 0);
    }
    gtk_text_buffer_set_text(text_buffer, text, -1);

    gchar *message;
    if (entry->size > actual_length) {
        message = g_strdup_printf("Showing first %zu bytes of %"
                                  G_GUINT64_FORMAT " (read-only).",
                                  actual_length, entry->size);
    } else {
        message = g_strdup_printf("Opened %s (%zu bytes, read-only).",
                                  entry->name, actual_length);
    }
    browser_set_status(browser, message, FALSE);
    g_free(message);
    g_free(text);
    g_bytes_unref(bytes);
}

static gboolean browser_show_directory(FileBrowser *browser)
{
    if (browser->directory_ids->len == 0) {
        browser_set_status(browser, "No directory is selected.", TRUE);
        return FALSE;
    }
    guint64 directory_id = g_array_index(browser->directory_ids, guint64,
                                         browser->directory_ids->len - 1);
    GError *error = NULL;
    GPtrArray *entries = browser_list_directory(browser, directory_id, &error);
    if (entries == NULL) {
        browser_set_status(browser, error->message, TRUE);
        g_clear_error(&error);
        return FALSE;
    }

    GtkWidget *child = gtk_widget_get_first_child(browser->file_list);
    while (child != NULL) {
        GtkWidget *next = gtk_widget_get_next_sibling(child);
        gtk_list_box_remove(GTK_LIST_BOX(browser->file_list), child);
        child = next;
    }

    if (browser->entries != NULL) {
        g_ptr_array_unref(browser->entries);
    }
    browser->entries = entries;
    for (guint i = 0; i < entries->len; ++i) {
        BrowserEntry *entry = g_ptr_array_index(entries, i);
        gchar *label_text = entry->is_directory
            ? g_strdup_printf("[DIR]  %s", entry->name)
            : g_strdup_printf("%s  (%" G_GUINT64_FORMAT " bytes)",
                              entry->name, entry->size);
        GtkWidget *label = gtk_label_new(label_text);
        gtk_label_set_xalign(GTK_LABEL(label), 0.0f);
        gtk_widget_set_margin_start(label, 8);
        gtk_widget_set_margin_end(label, 8);
        gtk_widget_set_margin_top(label, 5);
        gtk_widget_set_margin_bottom(label, 5);
        gtk_list_box_append(GTK_LIST_BOX(browser->file_list), label);
        g_free(label_text);
    }
    browser_update_path(browser);
    gchar *message = g_strdup_printf("%u items.", entries->len);
    browser_set_status(browser, message, FALSE);
    g_free(message);
    return TRUE;
}

static void on_browser_row_activated(GtkListBox *list_box, GtkListBoxRow *row,
                                     gpointer user_data)
{
    (void)list_box;
    FileBrowser *browser = user_data;
    guint index = (guint)gtk_list_box_row_get_index(row);
    if (index >= browser->entries->len) {
        return;
    }
    BrowserEntry *entry = g_ptr_array_index(browser->entries, index);
    if (entry->is_directory) {
        if (entry->id == 0) {
            browser_set_status(browser, "This directory has no valid data cluster.", TRUE);
            return;
        }
        g_array_append_val(browser->directory_ids, entry->id);
        g_ptr_array_add(browser->path_components, g_strdup(entry->name));
        gtk_text_buffer_set_text(
            gtk_text_view_get_buffer(GTK_TEXT_VIEW(browser->preview)), "", 0);
        if (!browser_show_directory(browser)) {
            g_array_set_size(browser->directory_ids, browser->directory_ids->len - 1);
            g_ptr_array_remove_index(browser->path_components,
                                     browser->path_components->len - 1);
        }
        return;
    }
    browser_show_preview(browser, entry);
}

static void on_browser_back_clicked(GtkButton *button, gpointer user_data)
{
    (void)button;
    FileBrowser *browser = user_data;
    if (browser->directory_ids->len <= 1) {
        return;
    }
    guint64 previous_id = g_array_index(browser->directory_ids, guint64,
                                        browser->directory_ids->len - 1);
    gchar *previous_component =
        g_strdup(g_ptr_array_index(browser->path_components,
                                   browser->path_components->len - 1));
    g_array_set_size(browser->directory_ids, browser->directory_ids->len - 1);
    g_ptr_array_remove_index(browser->path_components,
                             browser->path_components->len - 1);
    if (!browser_show_directory(browser)) {
        g_array_append_val(browser->directory_ids, previous_id);
        g_ptr_array_add(browser->path_components, previous_component);
    } else {
        g_free(previous_component);
    }
}

static void on_open_file_browser_clicked(GtkButton *button, gpointer user_data)
{
    (void)button;
    AppWidgets *widgets = user_data;
    guint selected = gtk_drop_down_get_selected(GTK_DROP_DOWN(widgets->device_dropdown));
    if (widgets->devices == NULL || selected == GTK_INVALID_LIST_POSITION
        || selected >= widgets->devices->len) {
        set_status(widgets, "Select a device first.", TRUE);
        return;
    }

    RawDevice *raw_device = g_ptr_array_index(widgets->devices, selected);
    GError *error = NULL;
    BlockDevice *device = block_device_open(raw_device->path, &error);
    if (device == NULL) {
        set_status(widgets, error->message, TRUE);
        g_clear_error(&error);
        return;
    }

    FileBrowser *browser = g_new0(FileBrowser, 1);
    browser->device = device;
    browser->path_components = g_ptr_array_new_with_free_func(g_free);
    browser->directory_ids = g_array_new(FALSE, FALSE, sizeof(guint64));

    GString *diagnostics = g_string_new(NULL);
    Ext4Volume *ext4 = ext4_volume_open(device, &error);
    if (ext4 != NULL) {
        browser->file_system = BROWSER_FS_EXT4;
        browser->volume.ext4 = ext4;
    } else {
        g_string_append_printf(diagnostics, "ext4: %s\n",
                               error != NULL ? error->message : "unknown error");
        g_clear_error(&error);
        NtfsVolume *ntfs = ntfs_volume_open(device, &error);
        if (ntfs != NULL) {
            browser->file_system = BROWSER_FS_NTFS;
            browser->volume.ntfs = ntfs;
        } else {
            g_string_append_printf(diagnostics, "NTFS: %s\n",
                                   error != NULL ? error->message : "unknown error");
            g_clear_error(&error);
            Fat32Volume *fat32 = fat32_volume_open(device, &error);
            if (fat32 != NULL) {
                browser->file_system = BROWSER_FS_FAT32;
                browser->volume.fat32 = fat32;
            } else {
                g_string_append_printf(diagnostics, "FAT32: %s",
                                       error != NULL ? error->message : "unknown error");
                g_clear_error(&error);
                browser->file_system = BROWSER_FS_FAT32;
                set_status(widgets, diagnostics->str, TRUE);
                g_string_free(diagnostics, TRUE);
                file_browser_free(browser);
                return;
            }
        }
    }
    g_string_free(diagnostics, TRUE);

    guint64 root_id = 0;
    switch (browser->file_system) {
    case BROWSER_FS_FAT32:
        root_id = fat32_volume_get_info(browser->volume.fat32).root_cluster;
        break;
    case BROWSER_FS_NTFS:
        root_id = 5;
        break;
    case BROWSER_FS_EXT4:
        root_id = 2;
        break;
    }
    g_array_append_val(browser->directory_ids, root_id);

    browser->window = gtk_window_new();
    const gchar *filesystem_name = browser->file_system == BROWSER_FS_EXT4
        ? "ext4" : browser->file_system == BROWSER_FS_NTFS ? "NTFS" : "FAT32";
    gchar *window_title = g_strdup_printf("RawReader — %s File Browser",
                                          filesystem_name);
    gtk_window_set_title(GTK_WINDOW(browser->window), window_title);
    g_free(window_title);
    gtk_window_set_default_size(GTK_WINDOW(browser->window), 860, 560);
    gtk_window_set_transient_for(GTK_WINDOW(browser->window),
                                 GTK_WINDOW(widgets->window));

    GtkWidget *root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_margin_top(root, 12);
    gtk_widget_set_margin_bottom(root, 12);
    gtk_widget_set_margin_start(root, 12);
    gtk_widget_set_margin_end(root, 12);
    gtk_window_set_child(GTK_WINDOW(browser->window), root);

    GtkWidget *toolbar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *back_button = gtk_button_new_with_label("Back");
    browser->path_label = gtk_label_new("/");
    gtk_label_set_xalign(GTK_LABEL(browser->path_label), 0.0f);
    gtk_widget_set_hexpand(browser->path_label, TRUE);
    gtk_box_append(GTK_BOX(toolbar), back_button);
    gtk_box_append(GTK_BOX(toolbar), browser->path_label);
    gtk_box_append(GTK_BOX(root), toolbar);

    GtkWidget *split = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
    browser->file_list = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(browser->file_list),
                                    GTK_SELECTION_SINGLE);
    GtkWidget *list_scroller = gtk_scrolled_window_new();
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(list_scroller), browser->file_list);
    gtk_widget_set_size_request(list_scroller, 280, -1);
    gtk_paned_set_start_child(GTK_PANED(split), list_scroller);

    browser->preview = gtk_text_view_new();
    gtk_text_view_set_editable(GTK_TEXT_VIEW(browser->preview), FALSE);
    gtk_text_view_set_cursor_visible(GTK_TEXT_VIEW(browser->preview), FALSE);
    gtk_text_view_set_monospace(GTK_TEXT_VIEW(browser->preview), TRUE);
    GtkWidget *preview_scroller = gtk_scrolled_window_new();
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(preview_scroller), browser->preview);
    gtk_paned_set_end_child(GTK_PANED(split), preview_scroller);
    gtk_paned_set_position(GTK_PANED(split), 300);
    gtk_widget_set_vexpand(split, TRUE);
    gtk_box_append(GTK_BOX(root), split);

    browser->status = gtk_label_new("Read-only browser.");
    gtk_label_set_xalign(GTK_LABEL(browser->status), 0.0f);
    gtk_box_append(GTK_BOX(root), browser->status);

    g_signal_connect(back_button, "clicked",
                     G_CALLBACK(on_browser_back_clicked), browser);
    g_signal_connect(browser->file_list, "row-activated",
                     G_CALLBACK(on_browser_row_activated), browser);
    g_object_set_data_full(G_OBJECT(browser->window), "rawreader-file-browser",
                           browser, file_browser_free);

    gtk_window_present(GTK_WINDOW(browser->window));
    browser_show_directory(browser);
    set_status(widgets, "Opened read-only file browser.", FALSE);
}

static gboolean parse_offset(const gchar *text, guint64 *offset)
{
    if (text == NULL || text[0] == '\0' || text[0] == '-') {
        return FALSE;
    }

    errno = 0;
    gchar *end = NULL;
    guint64 parsed = g_ascii_strtoull(text, &end, 10);
    if (errno == ERANGE || end == text || *end != '\0' || parsed > G_MAXINT64) {
        return FALSE;
    }
    *offset = parsed;
    return TRUE;
}

static void refresh_devices(AppWidgets *widgets)
{
    GError *error = NULL;
    GPtrArray *devices = device_list_discover(&error);

    if (devices == NULL) {
        if (widgets->devices != NULL) {
            g_ptr_array_unref(widgets->devices);
            widgets->devices = NULL;
        }
        GtkStringList *empty_model = gtk_string_list_new(NULL);
        gtk_drop_down_set_model(GTK_DROP_DOWN(widgets->device_dropdown),
                                G_LIST_MODEL(empty_model));
        g_object_unref(empty_model);
        set_status(widgets, error->message, TRUE);
        g_clear_error(&error);
        return;
    }

    GtkStringList *model = gtk_string_list_new(NULL);
    for (guint i = 0; i < devices->len; ++i) {
        RawDevice *device = g_ptr_array_index(devices, i);
        gtk_string_list_append(model, device->label);
    }
    gtk_drop_down_set_model(GTK_DROP_DOWN(widgets->device_dropdown),
                            G_LIST_MODEL(model));
    g_object_unref(model);
    gtk_drop_down_set_selected(GTK_DROP_DOWN(widgets->device_dropdown), 0);

    if (widgets->devices != NULL) {
        g_ptr_array_unref(widgets->devices);
    }
    widgets->devices = devices;
    set_status(widgets, "Device list refreshed.", FALSE);
}

static void on_refresh_clicked(GtkButton *button, gpointer user_data)
{
    (void)button;
    refresh_devices(user_data);
}

static void on_read_clicked(GtkButton *button, gpointer user_data)
{
    (void)button;
    AppWidgets *widgets = user_data;
    guint selected = gtk_drop_down_get_selected(GTK_DROP_DOWN(widgets->device_dropdown));
    guint64 offset;

    if (widgets->devices == NULL || selected == GTK_INVALID_LIST_POSITION
        || selected >= widgets->devices->len) {
        set_status(widgets, "Select a device first.", TRUE);
        return;
    }
    RawDevice *device = g_ptr_array_index(widgets->devices, selected);
    const gchar *device_path = device->path;
    if (!parse_offset(gtk_editable_get_text(GTK_EDITABLE(widgets->offset_entry)), &offset)) {
        set_status(widgets, "Enter a valid byte offset (zero or greater).", TRUE);
        return;
    }

    gsize requested = (gsize)gtk_spin_button_get_value_as_int(
        GTK_SPIN_BUTTON(widgets->length_spin));
    GtkTextBuffer *text_buffer =
        gtk_text_view_get_buffer(GTK_TEXT_VIEW(widgets->output_view));
    gtk_text_buffer_set_text(text_buffer, "", 0);

    GError *error = NULL;
    GBytes *bytes = raw_reader_read(device_path, offset, requested, &error);
    if (bytes == NULL) {
        set_status(widgets, error->message, TRUE);
        g_clear_error(&error);
        return;
    }

    gsize actual_length = 0;
    const guint8 *data = g_bytes_get_data(bytes, &actual_length);
    gchar *dump = hex_dump_format(data, actual_length, offset);
    gtk_text_buffer_set_text(text_buffer, dump, -1);

    gchar *message = g_strdup_printf("Read %zu of %zu bytes from %s.",
                                     actual_length, requested, device_path);
    set_status(widgets, message, FALSE);

    g_free(message);
    g_free(dump);
    g_bytes_unref(bytes);
}

static void on_scan_fat32_clicked(GtkButton *button, gpointer user_data)
{
    (void)button;
    AppWidgets *widgets = user_data;
    guint selected = gtk_drop_down_get_selected(GTK_DROP_DOWN(widgets->device_dropdown));
    if (widgets->devices == NULL || selected == GTK_INVALID_LIST_POSITION
        || selected >= widgets->devices->len) {
        set_status(widgets, "Select a device first.", TRUE);
        return;
    }

    RawDevice *raw_device = g_ptr_array_index(widgets->devices, selected);
    GError *error = NULL;
    BlockDevice *device = block_device_open(raw_device->path, &error);
    if (device == NULL) {
        set_status(widgets, error->message, TRUE);
        g_clear_error(&error);
        return;
    }

    Fat32Volume *volume = fat32_volume_open(device, &error);
    if (volume == NULL) {
        set_status(widgets, error->message, TRUE);
        g_clear_error(&error);
        block_device_close(device);
        return;
    }

    Fat32Info info = fat32_volume_get_info(volume);
    GPtrArray *entries = fat32_list_root(volume, &error);
    if (entries == NULL) {
        set_status(widgets, error->message, TRUE);
        g_clear_error(&error);
        fat32_volume_close(volume);
        block_device_close(device);
        return;
    }

    GString *output = g_string_new(NULL);
    g_string_append_printf(output,
                           "FAT32 volume\n"
                           "Partition offset: %" G_GUINT64_FORMAT " bytes\n"
                           "Volume size: %" G_GUINT64_FORMAT " bytes\n"
                           "Sector size: %u bytes\n"
                           "Cluster size: %u sectors (%u bytes)\n"
                           "Data clusters: %u\n\n"
                           "Root directory (%u entries)\n",
                           info.partition_offset, info.volume_size,
                           info.bytes_per_sector, info.sectors_per_cluster,
                           info.bytes_per_sector * info.sectors_per_cluster,
                           info.cluster_count, entries->len);
    for (guint i = 0; i < entries->len; ++i) {
        Fat32DirectoryEntry *entry = g_ptr_array_index(entries, i);
        if (entry->is_directory) {
            g_string_append_printf(output, "[DIR]  %s\n", entry->name);
        } else {
            g_string_append_printf(output, "%8u  %s\n", entry->size, entry->name);
        }
    }

    GtkTextBuffer *text_buffer =
        gtk_text_view_get_buffer(GTK_TEXT_VIEW(widgets->output_view));
    gtk_text_buffer_set_text(text_buffer, output->str, (gint)output->len);
    gchar *message = g_strdup_printf("Read-only FAT32 scan completed: %u root entries.",
                                     entries->len);
    set_status(widgets, message, FALSE);

    g_free(message);
    g_string_free(output, TRUE);
    g_ptr_array_unref(entries);
    fat32_volume_close(volume);
    block_device_close(device);
}

static void on_scan_ntfs_clicked(GtkButton *button, gpointer user_data)
{
    (void)button;
    AppWidgets *widgets = user_data;
    guint selected = gtk_drop_down_get_selected(GTK_DROP_DOWN(widgets->device_dropdown));
    if (widgets->devices == NULL || selected == GTK_INVALID_LIST_POSITION
        || selected >= widgets->devices->len) {
        set_status(widgets, "Select a device first.", TRUE);
        return;
    }

    RawDevice *raw_device = g_ptr_array_index(widgets->devices, selected);
    GError *error = NULL;
    BlockDevice *device = block_device_open(raw_device->path, &error);
    if (device == NULL) {
        set_status(widgets, error->message, TRUE);
        g_clear_error(&error);
        return;
    }

    NtfsVolume *volume = ntfs_volume_open(device, &error);
    if (volume == NULL) {
        set_status(widgets, error->message, TRUE);
        g_clear_error(&error);
        block_device_close(device);
        return;
    }

    NtfsInfo info = ntfs_volume_get_info(volume);
    GPtrArray *entries = ntfs_list_root(volume, &error);
    if (entries == NULL) {
        set_status(widgets, error->message, TRUE);
        g_clear_error(&error);
        ntfs_volume_close(volume);
        block_device_close(device);
        return;
    }

    GString *output = g_string_new(NULL);
    g_string_append_printf(output,
                           "NTFS volume\n"
                           "Partition offset: %" G_GUINT64_FORMAT " bytes\n"
                           "Volume size: %" G_GUINT64_FORMAT " bytes\n"
                           "Sector size: %u bytes\n"
                           "Cluster size: %u sectors (%u bytes)\n"
                           "Data clusters: %" G_GUINT64_FORMAT "\n"
                           "$MFT start cluster: %" G_GUINT64_FORMAT "\n\n"
                           "Root directory (%u entries)\n",
                           info.partition_offset, info.volume_size,
                           info.bytes_per_sector, info.sectors_per_cluster,
                           info.bytes_per_sector * info.sectors_per_cluster,
                           info.cluster_count, info.mft_start_cluster, entries->len);
    for (guint i = 0; i < entries->len; ++i) {
        NtfsDirectoryEntry *entry = g_ptr_array_index(entries, i);
        if (entry->is_directory) {
            g_string_append_printf(output, "[DIR]  %s\n", entry->name);
        } else {
            g_string_append_printf(output, "%8" G_GUINT64_FORMAT "  %s\n",
                                   entry->size, entry->name);
        }
    }

    GtkTextBuffer *text_buffer =
        gtk_text_view_get_buffer(GTK_TEXT_VIEW(widgets->output_view));
    gtk_text_buffer_set_text(text_buffer, output->str, (gint)output->len);
    gchar *message = g_strdup_printf("Read-only NTFS scan completed: %u root entries.",
                                     entries->len);
    set_status(widgets, message, FALSE);

    g_free(message);
    g_string_free(output, TRUE);
    g_ptr_array_unref(entries);
    ntfs_volume_close(volume);
    block_device_close(device);
}

static void on_scan_ext4_clicked(GtkButton *button, gpointer user_data)
{
    (void)button;
    AppWidgets *widgets = user_data;
    guint selected = gtk_drop_down_get_selected(GTK_DROP_DOWN(widgets->device_dropdown));
    if (widgets->devices == NULL || selected == GTK_INVALID_LIST_POSITION
        || selected >= widgets->devices->len) {
        set_status(widgets, "Select a device first.", TRUE);
        return;
    }

    RawDevice *raw_device = g_ptr_array_index(widgets->devices, selected);
    GError *error = NULL;
    BlockDevice *device = block_device_open(raw_device->path, &error);
    if (device == NULL) {
        set_status(widgets, error->message, TRUE);
        g_clear_error(&error);
        return;
    }

    Ext4Volume *volume = ext4_volume_open(device, &error);
    if (volume == NULL) {
        set_status(widgets, error->message, TRUE);
        g_clear_error(&error);
        block_device_close(device);
        return;
    }

    Ext4Info info = ext4_volume_get_info(volume);
    GPtrArray *entries = ext4_list_root(volume, &error);
    if (entries == NULL) {
        set_status(widgets, error->message, TRUE);
        g_clear_error(&error);
        ext4_volume_close(volume);
        block_device_close(device);
        return;
    }

    GString *output = g_string_new(NULL);
    g_string_append_printf(output,
                           "ext4 volume\n"
                           "Partition offset: %" G_GUINT64_FORMAT " bytes\n"
                           "Volume size: %" G_GUINT64_FORMAT " bytes\n"
                           "Block size: %u bytes\n"
                           "Blocks: %" G_GUINT64_FORMAT "\n"
                           "Blocks per group: %u\n"
                           "Inodes: %u\n"
                           "Inodes per group: %u\n\n"
                           "Root directory (%u entries)\n",
                           info.partition_offset, info.volume_size,
                           info.block_size, info.block_count,
                           info.blocks_per_group, info.inode_count,
                           info.inodes_per_group, entries->len);
    for (guint i = 0; i < entries->len; ++i) {
        Ext4DirectoryEntry *entry = g_ptr_array_index(entries, i);
        if (entry->is_directory) {
            g_string_append_printf(output, "[DIR]  %s\n", entry->name);
        } else {
            g_string_append_printf(output, "%8" G_GUINT64_FORMAT "  %s\n",
                                   entry->size, entry->name);
        }
    }

    GtkTextBuffer *text_buffer =
        gtk_text_view_get_buffer(GTK_TEXT_VIEW(widgets->output_view));
    gtk_text_buffer_set_text(text_buffer, output->str, (gint)output->len);
    gchar *message = g_strdup_printf("Read-only ext4 scan completed: %u root entries.",
                                     entries->len);
    set_status(widgets, message, FALSE);

    g_free(message);
    g_string_free(output, TRUE);
    g_ptr_array_unref(entries);
    ext4_volume_close(volume);
    block_device_close(device);
}

static GtkWidget *make_labeled_control(const gchar *label_text, GtkWidget *control)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *label = gtk_label_new(label_text);
    gtk_widget_set_halign(label, GTK_ALIGN_START);
    gtk_widget_set_hexpand(control, TRUE);
    gtk_box_append(GTK_BOX(box), label);
    gtk_box_append(GTK_BOX(box), control);
    return box;
}

static GtkWidget *make_output_scroller(AppWidgets *widgets)
{
    GtkWidget *scroller = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroller),
                                   GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);

    widgets->output_view = gtk_text_view_new();
    gtk_text_view_set_editable(GTK_TEXT_VIEW(widgets->output_view), FALSE);
    gtk_text_view_set_cursor_visible(GTK_TEXT_VIEW(widgets->output_view), FALSE);
    gtk_text_view_set_monospace(GTK_TEXT_VIEW(widgets->output_view), TRUE);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroller), widgets->output_view);
    return scroller;
}

static void build_window(GtkApplication *application, AppWidgets *widgets)
{
    GtkWidget *window = gtk_application_window_new(application);
    widgets->window = window;
    gtk_window_set_title(GTK_WINDOW(window), "RawReader");
    gtk_window_set_default_size(GTK_WINDOW(window), 900, 620);

    GtkWidget *root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_widget_set_margin_top(root, 16);
    gtk_widget_set_margin_bottom(root, 16);
    gtk_widget_set_margin_start(root, 16);
    gtk_widget_set_margin_end(root, 16);
    gtk_window_set_child(GTK_WINDOW(window), root);

    GtkWidget *device_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    const char *empty_device_labels[] = { NULL };
    widgets->device_dropdown = gtk_drop_down_new_from_strings(empty_device_labels);
    gtk_widget_set_hexpand(widgets->device_dropdown, TRUE);
    GtkWidget *refresh_button = gtk_button_new_with_label("Refresh");
    GtkWidget *browse_button = gtk_button_new_with_label("Open file browser");
    gtk_box_append(GTK_BOX(device_row), widgets->device_dropdown);
    gtk_box_append(GTK_BOX(device_row), browse_button);
    gtk_box_append(GTK_BOX(device_row), refresh_button);
    gtk_box_append(GTK_BOX(root), device_row);

    GtkWidget *options_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    widgets->offset_entry = gtk_entry_new();
    gtk_editable_set_text(GTK_EDITABLE(widgets->offset_entry), "0");
    gtk_entry_set_input_purpose(GTK_ENTRY(widgets->offset_entry), GTK_INPUT_PURPOSE_DIGITS);
    GtkWidget *offset_control =
        make_labeled_control("Offset (bytes):", widgets->offset_entry);
    gtk_widget_set_hexpand(offset_control, TRUE);
    gtk_box_append(GTK_BOX(options_row), offset_control);

    widgets->length_spin = gtk_spin_button_new_with_range(1, MAX_READ_SIZE, 1);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(widgets->length_spin), DEFAULT_READ_SIZE);
    GtkWidget *length_control =
        make_labeled_control("Read size (bytes):", widgets->length_spin);
    gtk_box_append(GTK_BOX(options_row), length_control);

    GtkWidget *read_button = gtk_button_new_with_label("Read");
    gtk_box_append(GTK_BOX(options_row), read_button);
    GtkWidget *scan_fat32_button = gtk_button_new_with_label("Scan FAT32");
    gtk_box_append(GTK_BOX(options_row), scan_fat32_button);
    GtkWidget *scan_ntfs_button = gtk_button_new_with_label("Scan NTFS");
    gtk_box_append(GTK_BOX(options_row), scan_ntfs_button);
    GtkWidget *scan_ext4_button = gtk_button_new_with_label("Scan ext4");
    gtk_box_append(GTK_BOX(options_row), scan_ext4_button);
    gtk_box_append(GTK_BOX(root), options_row);

    GtkWidget *output_scroller = make_output_scroller(widgets);
    gtk_widget_set_vexpand(output_scroller, TRUE);
    gtk_box_append(GTK_BOX(root), output_scroller);

    widgets->status_label =
        gtk_label_new("Ready. Devices are opened in read-only mode.");
    gtk_label_set_wrap(GTK_LABEL(widgets->status_label), TRUE);
    gtk_widget_set_halign(widgets->status_label, GTK_ALIGN_START);
    gtk_box_append(GTK_BOX(root), widgets->status_label);

    g_signal_connect(refresh_button, "clicked", G_CALLBACK(on_refresh_clicked), widgets);
    g_signal_connect(browse_button, "clicked",
                     G_CALLBACK(on_open_file_browser_clicked), widgets);
    g_signal_connect(read_button, "clicked", G_CALLBACK(on_read_clicked), widgets);
    g_signal_connect(scan_fat32_button, "clicked",
                     G_CALLBACK(on_scan_fat32_clicked), widgets);
    g_signal_connect(scan_ntfs_button, "clicked",
                     G_CALLBACK(on_scan_ntfs_clicked), widgets);
    g_signal_connect(scan_ext4_button, "clicked",
                     G_CALLBACK(on_scan_ext4_clicked), widgets);
    g_object_set_data_full(G_OBJECT(window), "rawreader-widgets", widgets,
                           app_widgets_free);
    gtk_window_present(GTK_WINDOW(window));
}

void rawreader_app_activate(GtkApplication *application, gpointer user_data)
{
    (void)user_data;
    AppWidgets *widgets = g_new0(AppWidgets, 1);
    build_window(application, widgets);
    refresh_devices(widgets);
}
