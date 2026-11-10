# Local media validation content

Run `sh tools/media-tests/generate.sh` on the PC (requires FFmpeg with libx264,
libvpx and libopus), then serve this directory with `python3 -m http.server 8765
--directory tools/media-tests --bind 127.0.0.1` for local browser tests.

The page tests MP4/H.264/AAC and WebM/VP9/Opus as both direct URLs and appended
MediaSource bytes, plus direct HLS and DASH. Check audible tones, moving patterns,
seek, pause/resume, end-of-stream and fullscreen. The MSE test buffers a short
complete file; it is not an adaptive streaming, eviction, live or DRM conformance
test. Test real site players separately. It reports browser events, not a device
certification. Small 320x180 clips deliberately establish functionality before
performance work. Generated files contain no third-party content.

No TV tests are authorized at present. Do not start a TV browser, copy these files
to the TV or expose a test server to it until the user resumes testing.

For a headless ARM decoder/seek check after staging multimedia:

```sh
docker/run.sh 'sh /src/tools/media-tests/build-probe.sh'
bash tools/media-tests/qemu.sh
```

This uses host `qemu-arm-static`, the glibc 2.28 sysroot and the **staged** ARM
plugins. Both video and audio must produce buffers before and after a 4s seek.
No GPU, speaker, browser process or MSE API is exercised by this decoder check.

`python3 tools/media-tests/url-tests.py` additionally checks HTTP MP4/WebM,
HLS and DASH with the staged `playbin3` stack, including audio/video sample
counts and seeking. It starts an ephemeral server bound only to loopback, and
shuts it down afterward. These are GStreamer tests, not WebKit/MSE tests.
