# Offscreen EGL probe

Build locally, without contacting the TV:

```sh
docker/run.sh 'sh /src/tools/egl-probe/build.sh'
```

Output: `build/egl-probe` (32-bit ARM softfp, glibc 2.28).
It dlopens EGL/GLES, prints the actual library paths and GL renderer, tests the
RGBA8/stencil8 pbuffer-capable configuration WebKit requests, and verifies an FBO
pixel. Default mode tests a surfaceless context if advertised, then a pbuffer.
`--pbuffer` forces the alternative path. `--no-bench` skips the 720p clear/readback
benchmark. `--platform surfaceless` is useful on a desktop Mesa host.
Exit 0 means the pixel check passed (and the benchmark had no GL errors, if run).
It does not certify WebKit, Skia, WebGL or video playback. A software renderer can
pass; inspect the printed renderer. RGBA4 fallback timings are explicitly labelled.

TV testing is paused at the user's request. Do not deploy or run this until the
user resumes TV testing. Root-process measurements are recorded below;
default-display access inside the app jail and GPU memory use remain unverified.
Test both default and forced-pbuffer modes under the same user/jail as the
installed browser when testing is authorized.
Mesa stays bundled, and software stays the browser's default. The opt-in
`WPE_BROWSER_RENDERER=gpu` selects firmware EGL/GLES for the engine's children.

Recorded C1 measurement (2026-11-10, webOS 6.5.3): default-display and pbuffer
pixel checks passed on Mali-G51; 720p readback averaged about 16 ms. See NOTES.md
for exact results, root-versus-jail limitations and the separate Skia painting
failure. The user subsequently stopped TV testing again.
