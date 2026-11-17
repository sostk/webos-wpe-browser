#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Apply buildroot-external/buildroot-patches/*.patch to the Buildroot checkout (default /build/buildroot, the path
# inside the build container). Idempotent: a patch that is already applied is skipped, one that applies neither way
# stops the script. Run it before the first make after cloning or updating Buildroot; the build scripts call it.
#   sh tools/patch-buildroot.sh [buildroot dir]
set -eu
project=$(cd "$(dirname "$0")/.." && pwd)
buildroot=${1:-/build/buildroot}
test -f "$buildroot/Makefile" || { echo "patch-buildroot: no Buildroot at $buildroot" >&2; exit 1; }
for p in "$project"/buildroot-external/buildroot-patches/*.patch; do
  [ -e "$p" ] || continue
  name=$(basename "$p")
  if patch -d "$buildroot" -p1 -R --dry-run -s -f < "$p" >/dev/null 2>&1; then
    echo "patch-buildroot: $name already applied"
  elif patch -d "$buildroot" -p1 --dry-run -s -f < "$p" >/dev/null 2>&1; then
    patch -d "$buildroot" -p1 -s --no-backup-if-mismatch < "$p"
    echo "patch-buildroot: $name applied"
  else
    echo "patch-buildroot: $name does not apply to $buildroot (Buildroot changed? check package/wpewebkit)" >&2
    exit 1
  fi
done
