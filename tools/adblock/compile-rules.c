/* SPDX-License-Identifier: GPL-3.0-or-later
 * Compiles a WebKit content-blocker JSON file into a content rule list store with the engine's own WPE WebKit:
 *     compile-rules RULES.json STORE_DIR IDENTIFIER
 * The store format is versioned per WebKit release (WPE 2.50: 20), so the store the TV loads must come from the same
 * WebKit as the engine. tools/adblock/build.sh runs this ARM build under QEMU at build time: compiling EasyList on a
 * TV takes several hundred MB, more than a 2 GB TV has free. */
#include <stdio.h>
#include <stdlib.h>
#include <wpe/webkit.h>

static GMainLoop *loop;
static int status = 1;

static void saved(GObject *source, GAsyncResult *result, gpointer data)
{
    GError *error = NULL;
    WebKitUserContentFilter *filter =
        webkit_user_content_filter_store_save_finish(WEBKIT_USER_CONTENT_FILTER_STORE(source), result, &error);
    if (filter) {
        printf("compiled %s in %.1f s\n", webkit_user_content_filter_get_identifier(filter),
               (g_get_monotonic_time() - *(gint64 *)data) / 1e6);
        webkit_user_content_filter_unref(filter);
        status = 0;
    } else {
        fprintf(stderr, "compile failed: %s\n", error ? error->message : "?");
        g_clear_error(&error);
    }
    g_main_loop_quit(loop);
}

int main(int argc, char **argv)
{
    if (argc != 4) {
        fprintf(stderr, "usage: compile-rules RULES.json STORE_DIR IDENTIFIER\n");
        return 2;
    }
    gchar *json = NULL;
    gsize size = 0;
    GError *error = NULL;
    if (!g_file_get_contents(argv[1], &json, &size, &error)) {
        fprintf(stderr, "%s\n", error->message);
        return 1;
    }
    GBytes *bytes = g_bytes_new_take(json, size);
    WebKitUserContentFilterStore *store = webkit_user_content_filter_store_new(argv[2]);
    loop = g_main_loop_new(NULL, FALSE);
    gint64 start = g_get_monotonic_time();
    webkit_user_content_filter_store_save(store, argv[3], bytes, NULL, saved, &start);
    g_main_loop_run(loop);
    g_bytes_unref(bytes);
    g_object_unref(store);
    return status;
}
