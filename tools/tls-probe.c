// SPDX-License-Identifier: GPL-3.0-or-later
#include <gio/gio.h>
#include <gnutls/gnutls.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int main(int argc, char **argv)
{
    if (argc != 4) { fprintf(stderr, "usage: tls-probe HOST PORT accept|reject\n"); return 2; }
    gnutls_certificate_credentials_t credentials;
    if (gnutls_certificate_allocate_credentials(&credentials) < 0) return 2;
    int roots = gnutls_certificate_set_x509_system_trust(credentials);
    printf("CA file: %s; trusted roots: %d\n", g_getenv("WPE_BROWSER_CA_FILE"), roots);
    gnutls_certificate_free_credentials(credentials);
    if (roots <= 0) { fputs("no readable trusted roots\n", stderr); return 2; }
    printf("TLS backend: %s\n", G_OBJECT_TYPE_NAME(g_tls_backend_get_default()));
    GSocketClient *client = g_socket_client_new();
    g_socket_client_set_timeout(client, 15);
    g_socket_client_set_tls(client, TRUE);
    GError *error = NULL;
    GSocketConnection *connection = g_socket_client_connect_to_host(client, argv[1], (guint16)strtoul(argv[2], NULL, 10), NULL, &error);
    int accepted = connection != NULL;
    int rejected_certificate = error && g_error_matches(error, G_TLS_ERROR, G_TLS_ERROR_BAD_CERTIFICATE);
    if (error) printf("TLS result: %s\n", error->message);
    if (connection) { g_io_stream_close(G_IO_STREAM(connection), NULL, NULL); g_object_unref(connection); }
    int ok = !strcmp(argv[3], "accept") ? accepted : !strcmp(argv[3], "reject") && rejected_certificate;
    g_clear_error(&error);
    g_object_unref(client);
    puts(ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
