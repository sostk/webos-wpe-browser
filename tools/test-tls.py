#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Loopback-only ARM tests: trusted leaf, untrusted issuer, wrong hostname.
from pathlib import Path
import functools
import http.server
import os
import ssl
import subprocess
import tempfile
import threading
src = Path(__file__).resolve().parents[1]
br = Path(os.environ.get('BR', str(Path.home() / 'webos-browser-build/br-out')))
engine = src / 'build/engine-stage/engine'
class Quiet(http.server.SimpleHTTPRequestHandler):
    def log_message(self, *args): pass
with tempfile.TemporaryDirectory(prefix='wpe-tls-') as temp:
    directory = Path(temp)
    def openssl(*arguments):
        subprocess.run(['openssl', *arguments], check=True, cwd=directory, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
    openssl('req', '-x509', '-newkey', 'rsa:2048', '-nodes', '-keyout', 'ca.key', '-out', 'ca.pem', '-days', '2', '-subj', '/CN=WPE local test CA')
    openssl('req', '-newkey', 'rsa:2048', '-nodes', '-keyout', 'server.key', '-out', 'server.csr', '-subj', '/CN=localhost')
    (directory / 'extensions').write_text('subjectAltName=DNS:localhost\nextendedKeyUsage=serverAuth\n')
    openssl('x509', '-req', '-in', 'server.csr', '-CA', 'ca.pem', '-CAkey', 'ca.key', '-CAcreateserial', '-out', 'server.pem', '-days', '2', '-extfile', 'extensions')
    handler = functools.partial(Quiet, directory=temp)
    with http.server.ThreadingHTTPServer(('127.0.0.1', 0), handler) as server:
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        context.load_cert_chain(directory / 'server.pem', directory / 'server.key')
        server.socket = context.wrap_socket(server.socket, server_side=True)
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        try:
            for name, ca, host, expected in [
                ('trusted issuer', directory / 'ca.pem', 'localhost', 'accept'),
                ('untrusted issuer', engine / 'etc/ssl/certs/ca-certificates.crt', 'localhost', 'reject'),
                ('wrong hostname', directory / 'ca.pem', '127.0.0.1', 'reject'),
            ]:
                print(name, flush=True)
                command = ['qemu-arm-static', '-L', str(br / 'host/arm-buildroot-linux-gnueabi/sysroot')]
                for key, value in {'LD_LIBRARY_PATH': str(engine / 'lib'), 'GIO_MODULE_DIR': str(engine / 'lib/gio/modules'), 'GIO_EXTRA_MODULES': '', 'GIO_USE_TLS': 'gnutls', 'WPE_BROWSER_CA_FILE': str(ca)}.items():
                    command += ['-E', key + '=' + value]
                command += [str(src / 'build/tls-probe'), host, str(server.server_port), expected]
                subprocess.run(command, check=True, timeout=30)
        finally:
            server.shutdown()
            thread.join()
