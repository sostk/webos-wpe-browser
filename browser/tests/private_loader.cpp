// SPDX-License-Identifier: GPL-3.0-or-later
// The hard-float engine's executables name a loader in the app's runtime folder; the UI links it to engine/lib.
#include "private_loader.hpp"
#include <climits>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <unistd.h>

namespace fs = std::filesystem;
static int failures = 0;
static void check(bool ok, const std::string &what) {
    std::cout << (ok ? "ok   " : "FAIL ") << what << '\n';
    if (!ok) ++failures;
}

int main(int, char **argv) {
    const std::string fixture = LOADER_FIXTURE; // linked with --dynamic-linker=LOADER_FIXTURE_INTERP
    const std::string runtime = fs::path(LOADER_FIXTURE_INTERP).parent_path();
    fs::remove_all(runtime);
    fs::create_directories(runtime);
    char tmpl[] = "/tmp/wpe-loader-engine-XXXXXX";
    const std::string engine = ::mkdtemp(tmpl);
    const std::string lib = engine + "/lib";
    fs::create_directories(lib);

    check(browser::elf_interpreter(fixture) == LOADER_FIXTURE_INTERP, "reads PT_INTERP: " + browser::elf_interpreter(fixture));
    const std::string self = browser::elf_interpreter(argv[0]);
    check(!self.empty() && self.rfind("/tmp/", 0) != 0, "system loader of the test itself: " + self);
    check(browser::elf_interpreter(lib).empty(), "a folder has no loader");

    bool priv = true;
    std::string err;
    check(browser::prepare_private_loader(argv[0], lib, runtime, priv, err) && !priv, "system loader needs nothing");

    check(!browser::prepare_private_loader(fixture, lib, runtime, priv, err) && priv &&
              err.find("missing bundled loader") != std::string::npos, "missing loader is reported: " + err);

    const std::string loader = lib + "/" + fs::path(LOADER_FIXTURE_INTERP).filename().string();
    std::ofstream(loader) << "x";
    fs::permissions(loader, fs::perms::owner_all);
    err.clear();
    check(browser::prepare_private_loader(fixture, lib, runtime, priv, err) && priv, "link created " + err);
    check(fs::is_symlink(LOADER_FIXTURE_INTERP) && fs::read_symlink(LOADER_FIXTURE_INTERP) == loader, "link points at engine/lib");
    check(browser::prepare_private_loader(fixture, lib, runtime, priv, err), "second call keeps the link");

    fs::remove(LOADER_FIXTURE_INTERP);
    fs::create_symlink("/bin/true", LOADER_FIXTURE_INTERP);
    check(browser::prepare_private_loader(fixture, lib, runtime, priv, err) &&
              fs::read_symlink(LOADER_FIXTURE_INTERP) == loader, "a wrong link is replaced");

    err.clear();
    check(!browser::prepare_private_loader(fixture, lib, "/somewhere/else/" + fs::path(runtime).filename().string(), priv, err) &&
              err.find("runtime folder") != std::string::npos, "runtime folder mismatch is reported: " + err);

    fs::remove_all(runtime);
    fs::remove_all(engine);
    return failures ? 1 : 0;
}
