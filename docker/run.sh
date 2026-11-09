#!/bin/sh
# Run a command in the build container as the host user.
# /src = project folder, /build = ~/webos-browser-build (toolchain, tarballs, buildroot output).
exec docker run --rm -i -u "$(id -u):$(id -g)" -e HOME=/build -e MAKEFLAGS \
  -v /home/sostk/Desktop/waydroid-test:/src -v "$HOME/webos-browser-build":/build -w /build \
  webos-browser-build:1 sh -c "$*"
