#!/bin/sh
# Apply the local webos-native-app-kit changes (see NOTES.md; 0005: check-package --system-lib) on top of the
# submodule's upstream commit. Run after `git submodule update --init`. Idempotent: patches that
# are already applied are skipped.
set -e
here=$(cd "$(dirname "$0")" && pwd)
kit="$here/../webos-native-app-kit"
for p in "$here"/webos-native-app-kit/*.patch; do
    if git -C "$kit" apply --check --reverse "$p" >/dev/null 2>&1; then
        echo "already applied: $(basename "$p")"
    else
        echo "applying: $(basename "$p")"
        git -C "$kit" apply "$p"
    fi
done
