#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Exercise target GStreamer URL/adaptive playback over loopback only, through QEMU."""
import functools
import http.server
import os
import re
from pathlib import Path
import subprocess
import tempfile
import threading

src = Path(__file__).resolve().parents[2]
br = Path(os.environ.get("BR", str(Path.home() / "webos-browser-build/br-out")))
engine = Path(os.environ.get("ENGINE_STAGE", str(src / "build/engine-stage/engine")))
class QuietHandler(http.server.SimpleHTTPRequestHandler):
    def log_message(self, *args):
        pass

    def do_GET(self):
        path = Path(self.translate_path(self.path))
        if not path.is_file():
            return super().do_GET()
        size = path.stat().st_size
        start, end, status = 0, size - 1, 200
        requested = self.headers.get("Range")
        if requested:
            match = re.fullmatch(r"bytes=(\d+)-(\d*)", requested)
            if not match:
                self.send_error(416)
                return
            start = int(match[1])
            end = min(int(match[2]) if match[2] else end, end)
            if start > end:
                self.send_error(416)
                return
            status = 206
        self.send_response(status)
        self.send_header("Content-Type", self.guess_type(str(path)))
        self.send_header("Accept-Ranges", "bytes")
        self.send_header("Content-Length", str(end - start + 1))
        if status == 206:
            self.send_header("Content-Range", f"bytes {start}-{end}/{size}")
        self.end_headers()
        if self.command == "HEAD":
            return
        with path.open("rb") as data:
            data.seek(start)
            remaining = end - start + 1
            try:
                while remaining:
                    chunk = data.read(min(65536, remaining))
                    if not chunk:
                        break
                    self.wfile.write(chunk)
                    remaining -= len(chunk)
            except (BrokenPipeError, ConnectionResetError):
                pass

    def do_HEAD(self):
        return self.do_GET()
handler = functools.partial(QuietHandler, directory=str(src / "tools/media-tests"))
with http.server.ThreadingHTTPServer(("127.0.0.1", 0), handler) as server, tempfile.TemporaryDirectory(prefix="wpe-url-") as cache:
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    try:
        for filename in ("indexed-avc-aac.mp4", "vp9-opus.webm", "stream.m3u8", "stream.mpd"):
            print("URL:", filename, flush=True)
            command = ["timeout", "90", "qemu-arm-static", "-L", str(br / "host/arm-buildroot-linux-gnueabi/sysroot")]
            env = {
                "LD_LIBRARY_PATH": str(engine / "lib") + ":" + str(engine / "lib/pulseaudio"),
                "GST_PLUGIN_SYSTEM_PATH_1_0": str(engine / "lib/gstreamer-1.0"),
                "GST_PLUGIN_SYSTEM_PATH": str(engine / "lib/gstreamer-1.0"),
                "GST_PLUGIN_PATH": "", "GST_PLUGIN_PATH_1_0": "", "GST_REGISTRY_FORK": "no",
                "GST_REGISTRY": cache + "/registry.bin", "GST_REGISTRY_1_0": cache + "/registry.bin",
                "GIO_MODULE_DIR": str(engine / "lib/gio/modules"), "GIO_EXTRA_MODULES": "",
            }
            for name, value in env.items():
                command += ["-E", name + "=" + value]
            command += [str(src / "build/media-tests/decode-probe"), "url", f"http://127.0.0.1:{server.server_port}/generated/{filename}"]
            subprocess.run(command, check=True, timeout=100)
    finally:
        server.shutdown()
        thread.join()
