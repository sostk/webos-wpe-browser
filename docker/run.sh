#!/bin/sh
# Run a command in the build container as the host user.
# /src = project folder, /build = ~/webos-browser-build (toolchain, tarballs, buildroot output).
project=$(cd "$(dirname "$0")/.." && pwd)
exec docker run --rm -i -u "$(id -u):$(id -g)" -e HOME=/build -e MAKEFLAGS \
  -v "$project":/src -v "${WPE_BUILD_ROOT:-$HOME/webos-browser-build}":/build -w /build \
  webos-browser-build:1 sh -c "$*"
