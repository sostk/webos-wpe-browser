// SPDX-License-Identifier: GPL-3.0-or-later
#include "storage.hpp"
#include <cerrno>
#include <ftw.h>
#include <sys/stat.h>
#include <unistd.h>

namespace browser {

bool make_dir(const std::string &path) {
    for (size_t i = 1; i <= path.size(); ++i) {
        if (i == path.size() || path[i] == '/') {
            const std::string part = path.substr(0, i);
            if (::mkdir(part.c_str(), 0700) != 0 && errno != EEXIST) return false;
        }
    }
    struct stat st {};
    return ::stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

namespace {
thread_local std::string *g_bad = nullptr;

int check_entry(const char *path, const struct stat *st, int type, struct FTW *) {
    bool ok = true;
    if (type == FTW_D)
        ok = ::access(path, R_OK | W_OK | X_OK) == 0;
    else if (type == FTW_DNR || type == FTW_NS)
        ok = false;
    else if (type == FTW_F && S_ISREG(st->st_mode))
        ok = ::access(path, R_OK | W_OK) == 0;
    if (!ok && g_bad) *g_bad = path;
    return ok ? 0 : 1;
}
} // namespace

bool usable_tree(const std::string &path, std::string *bad) {
    struct stat st {};
    if (::lstat(path.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) {
        if (bad) *bad = path;
        return false;
    }
    g_bad = bad;
    const int rc = ::nftw(path.c_str(), check_entry, 16, FTW_PHYS);
    g_bad = nullptr;
    return rc == 0;
}

std::string own_usable_dir(const std::string &preferred, std::string *why) {
    std::string bad;
    if (make_dir(preferred) && usable_tree(preferred, &bad)) return preferred;
    if (why) *why = "not usable: " + (bad.empty() ? preferred : bad);
    const std::string alt = preferred + "-" + std::to_string(::getuid());
    if (make_dir(alt) && usable_tree(alt, &bad)) return alt;
    if (why) *why += "; not usable: " + (bad.empty() ? alt : bad);
    return {};
}

} // namespace browser
