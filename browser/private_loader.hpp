// SPDX-License-Identifier: GPL-3.0-or-later
// The engine can bring its own glibc. Its executables then name a dynamic loader (PT_INTERP) at a fixed path inside
// the app's runtime folder, e.g. /tmp/org.sostk.wpebrowser/ld-linux.so.3, because WebKit and GStreamer exec their
// helpers by path and every process must start with the same loader. Before spawning the engine, the UI points that
// path at a loader:
//   - softfp engine with engine/lib/glibc/ (bundled glibc 2.28): the bundled loader when the TV's glibc is older
//     than 2.28 (webOS 4 and older), else the TV's own /lib/ld-linux.so.3 and the bundled glibc is not used.
//     `policy` can force either (launch parameter {"wpebGlibc": "bundled" | "system"}).
//   - hard-float engine (engine/lib/ld-linux-armhf.so.3): always the bundled loader; the TV has none.
// An engine whose PT_INTERP is the system's own loader needs nothing.
#pragma once
#include <string>

namespace browser {

// PT_INTERP of an ELF executable (32- or 64-bit, either byte order), or "" when it has none / can't be read.
std::string elf_interpreter(const std::string &path);

// "2.24" < "2.28": dotted version numbers compared numerically.
int compare_versions(const std::string &a, const std::string &b);

enum class GlibcPolicy { Auto, Bundled, System };
GlibcPolicy glibc_policy_from(const std::string &s);  // "bundled", "system", anything else Auto

struct LoaderChoice {
    bool linked = false;         // the engine's loader is a link in the runtime folder (made by this call)
    bool bundled_glibc = false;  // the link points at the engine's own glibc
    bool hard_float = false;     // the engine is hard-float (no TV GPU libraries can load into it)
    std::string glibc_dir;       // with bundled_glibc: the folder with the bundled loader, libc and gconv/
    std::string target;          // what the link points at
    std::string why;             // for the log
};

// Makes the engine's loader link (see above). `system_glibc` is the TV's glibc version (gnu_get_libc_version())
// and `system_lib` the folder of its loader ("/lib"; tests pass another). Returns false (with `error`) when the
// link can't be made safely.
bool prepare_private_loader(const std::string &engine_exe, const std::string &engine_lib,
                            const std::string &runtime_dir, GlibcPolicy policy, const std::string &system_glibc,
                            const std::string &system_lib, LoaderChoice &choice, std::string &error);

} // namespace browser
