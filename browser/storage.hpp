// SPDX-License-Identifier: GPL-3.0-or-later
// Profile folders the app can really use: existing folders and files may belong to another user
// (root diagnostics on the TV left root-owned folders and a read-only cookies.sqlite behind).
#pragma once
#include <string>

namespace browser {

// mkdir -p with mode 0700 for new parts. An existing directory counts as success.
bool make_dir(const std::string &path);

// True when `path` is a directory and every directory below it (including itself) is writable and
// searchable, and every file is readable and writable by this process. On failure `bad` names the first
// unusable entry. Symlinks are not followed.
bool usable_tree(const std::string &path, std::string *bad = nullptr);

// make_dir + usable_tree on `preferred`. When the existing tree is unusable, use `preferred-<uid>`
// instead, so data stays persistent without touching the other user's files. Returns "" if neither works.
std::string own_usable_dir(const std::string &preferred, std::string *why = nullptr);

} // namespace browser
