// SPDX-License-Identifier: GPL-3.0-or-later
// A profile folder holding a file this uid cannot write (e.g. a root-owned cookies.sqlite) must not be used.
#include "storage.hpp"
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

namespace fs = std::filesystem;
static int failures = 0;

static void check(bool ok, const std::string &what) {
    std::cout << (ok ? "ok   " : "FAIL ") << what << '\n';
    if (!ok) ++failures;
}

int main() {
    if (::getuid() == 0) {
        std::cout << "skipped: root ignores permission bits\n";
        return 0;
    }
    char pattern[] = "/tmp/wpe-storage-test-XXXXXX";
    const std::string root = ::mkdtemp(pattern);
    const std::string data = root + "/org.example.data";

    check(browser::own_usable_dir(data) == data, "fresh folder is created and used");
    check(browser::make_dir(data + "/webkit/cache"), "nested make_dir");
    std::ofstream(data + "/webkit/cookies.sqlite") << "x";
    check(browser::usable_tree(data), "writable tree is usable");

    ::chmod((data + "/webkit/cookies.sqlite").c_str(), 0444);
    std::string bad;
    check(!browser::usable_tree(data, &bad), "read-only file makes the tree unusable");
    check(bad == data + "/webkit/cookies.sqlite", "the read-only file is reported: " + bad);

    std::string why;
    const std::string alt = data + "-" + std::to_string(::getuid());
    check(browser::own_usable_dir(data, &why) == alt, "falls back to the per-uid folder");
    check(fs::exists(data + "/webkit/cookies.sqlite"), "the other folder is left untouched");
    check(why.find("cookies.sqlite") != std::string::npos, "reason names the file: " + why);

    ::chmod((data + "/webkit").c_str(), 0500);
    check(!browser::usable_tree(data, &bad) && bad == data + "/webkit", "read-only folder is reported: " + bad);
    ::chmod((data + "/webkit").c_str(), 0700);

    std::ofstream(root + "/plain") << "x";
    check(!browser::make_dir(root + "/plain"), "make_dir rejects an existing file");

    ::chmod((data + "/webkit/cookies.sqlite").c_str(), 0644);
    fs::remove_all(root);
    return failures ? 1 : 0;
}
