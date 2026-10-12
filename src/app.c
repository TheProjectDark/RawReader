#include "app.h"

#include "block_device.h"
#include "device.h"
#include "fat32.h"
#include "hex_dump.h"
#include "ntfs.h"
#include "raw_reader.h"

#include <errno.h>

#define DEFAULT_READ_SIZE 512
#define MAX_READ_SIZE (64 * 1024)

typedef struct {
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
    gtk_box_append(GTK_BOX(device_row), widgets->device_dropdown);
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
    gtk_box_append(GTK_BOX(root), options_row);

    GtkWidget *output_scroller = make_output_scroller(widgets);
    gtk_widget_set_vexpand(output_scroller, TRUE);
    gtk_box_append(GTK_BOX(root), output_scroller);

    widgets->status_label =
        gtk_label_new("Ready. Devices are opened in read-only mode.");
    gtk_widget_set_halign(widgets->status_label, GTK_ALIGN_START);
    gtk_box_append(GTK_BOX(root), widgets->status_label);

    g_signal_connect(refresh_button, "clicked", G_CALLBACK(on_refresh_clicked), widgets);
    g_signal_connect(read_button, "clicked", G_CALLBACK(on_read_clicked), widgets);
    g_signal_connect(scan_fat32_button, "clicked",
                     G_CALLBACK(on_scan_fat32_clicked), widgets);
    g_signal_connect(scan_ntfs_button, "clicked",
                     G_CALLBACK(on_scan_ntfs_clicked), widgets);
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
