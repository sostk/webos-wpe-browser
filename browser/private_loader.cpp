// SPDX-License-Identifier: GPL-3.0-or-later
#include "private_loader.hpp"
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <elf.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace browser {
namespace {

bool read_at(int fd, uint64_t off, void *buf, size_t n) {
    return ::pread(fd, buf, n, static_cast<off_t>(off)) == static_cast<ssize_t>(n);
}

template <typename T> T fix(T v, bool swap) {
    if (!swap) return v;
    T r;
    auto *s = reinterpret_cast<const unsigned char *>(&v);
    auto *d = reinterpret_cast<unsigned char *>(&r);
    for (size_t i = 0; i < sizeof(T); ++i) d[i] = s[sizeof(T) - 1 - i];
    return r;
}

template <typename Ehdr, typename Phdr> std::string interp(int fd, bool swap) {
    Ehdr eh;
    if (!read_at(fd, 0, &eh, sizeof eh)) return {};
    const uint64_t phoff = fix(eh.e_phoff, swap);
    const unsigned phnum = fix(eh.e_phnum, swap), phentsize = fix(eh.e_phentsize, swap);
    if (phentsize < sizeof(Phdr) || phnum > 256) return {};
    for (unsigned i = 0; i < phnum; ++i) {
        Phdr ph;
        if (!read_at(fd, phoff + static_cast<uint64_t>(i) * phentsize, &ph, sizeof ph)) return {};
        if (fix(ph.p_type, swap) != PT_INTERP) continue;
        const uint64_t size = fix(ph.p_filesz, swap);
        if (size < 2 || size > 4096) return {};
        std::string s(size, '\0');
        if (!read_at(fd, fix(ph.p_offset, swap), &s[0], size)) return {};
        s.resize(std::strlen(s.c_str()));
        return s;
    }
    return {};
}

std::string dirname_of(const std::string &p) {
    const auto slash = p.rfind('/');
    return slash == std::string::npos ? std::string() : p.substr(0, slash);
}

} // namespace

std::string elf_interpreter(const std::string &path) {
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return {};
    unsigned char ident[EI_NIDENT];
    std::string r;
    if (read_at(fd, 0, ident, sizeof ident) && std::memcmp(ident, ELFMAG, SELFMAG) == 0) {
        const bool little_host = [] { const uint16_t one = 1; return *reinterpret_cast<const unsigned char *>(&one) == 1; }();
        const bool swap = (ident[EI_DATA] == ELFDATA2LSB) != little_host;
        if (ident[EI_CLASS] == ELFCLASS32) r = interp<Elf32_Ehdr, Elf32_Phdr>(fd, swap);
        else if (ident[EI_CLASS] == ELFCLASS64) r = interp<Elf64_Ehdr, Elf64_Phdr>(fd, swap);
    }
    ::close(fd);
    return r;
}

bool prepare_private_loader(const std::string &engine_exe, const std::string &engine_lib,
                            const std::string &runtime_dir, bool &private_loader, std::string &error) {
    private_loader = false;
    const std::string want = elf_interpreter(engine_exe);
    if (want.empty()) {
        error = "cannot read the dynamic loader of " + engine_exe;
        return false;
    }
    // Only a loader placed in a folder named for this app's runtime folder is ours to provide.
    const std::string dir = dirname_of(want);
    const auto slash = dir.rfind('/');
    const std::string app_folder = slash == std::string::npos ? dir : dir.substr(slash + 1);
    const auto rslash = runtime_dir.rfind('/');
    const std::string runtime_folder = rslash == std::string::npos ? runtime_dir : runtime_dir.substr(rslash + 1);
    if (dir.compare(0, 5, "/tmp/") != 0 || app_folder != runtime_folder) return true; // the system's loader

    private_loader = true;
    if (dir != runtime_dir) {
        error = "the engine expects its loader in " + dir + " but the app's runtime folder is " +
                (runtime_dir.empty() ? std::string("unavailable") : runtime_dir);
        return false;
    }
    const std::string target = engine_lib + want.substr(dir.size()); // engine_lib + "/<loader>"
    if (::access(target.c_str(), R_OK | X_OK) != 0) {
        error = "missing bundled loader " + target;
        return false;
    }
    char cur[4096];
    const ssize_t n = ::readlink(want.c_str(), cur, sizeof cur - 1);
    if (n > 0 && std::string(cur, static_cast<size_t>(n)) == target) return true;

    // runtime_dir is owned by this uid (the kit checks it), so the link can't be swapped by another user.
    const std::string tmp = want + ".new" + std::to_string(::getpid());
    ::unlink(tmp.c_str());
    if (::symlink(target.c_str(), tmp.c_str()) != 0 || ::rename(tmp.c_str(), want.c_str()) != 0) {
        error = "cannot link " + want + ": " + std::strerror(errno);
        ::unlink(tmp.c_str());
        return false;
    }
    return true;
}

} // namespace browser
