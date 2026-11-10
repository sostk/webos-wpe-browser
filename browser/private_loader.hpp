// SPDX-License-Identifier: GPL-3.0-or-later
// The hard-float engine (JavaScript JIT) ships its own glibc. Its executables name a dynamic loader
// (PT_INTERP) at a fixed path inside the app's runtime folder, e.g. /tmp/org.sostk.wpebrowser/ld-linux-armhf.so.3,
// because WebKit and GStreamer exec their helpers by path and the TV's /lib has no hard-float loader.
// Before spawning the engine, the UI points that path at engine/lib/<loader>.
#pragma once
#include <string>

namespace browser {

// PT_INTERP of an ELF executable (32- or 64-bit, either byte order), or "" when it has none / can't be read.
std::string elf_interpreter(const std::string &path);

// If `engine_exe` names a loader inside `runtime_dir`, make that path a symlink to `engine_lib/<loader name>`.
// `private_loader` is set when the engine brings its own loader. Returns false (with `error`) when the
// link can't be made safely; a system loader (softfp engine) needs nothing and returns true.
bool prepare_private_loader(const std::string &engine_exe, const std::string &engine_lib,
                            const std::string &runtime_dir, bool &private_loader, std::string &error);

} // namespace browser
