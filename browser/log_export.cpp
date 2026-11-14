// SPDX-License-Identifier: GPL-3.0-or-later
#include "log_export.hpp"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>
#include <sstream>

namespace browser {

namespace {

bool url_end(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '"' || c == '\'' || c == '<' || c == '>' ||
           c == ')' || c == ']' || c == '}' || c == ',' || c == '`';
}

// The last `max` bytes of a file, starting at a line break; "" with `missing` set if it can't be read.
std::string read_tail(const std::string &path, std::size_t max, bool *missing, bool *cut) {
    *missing = false;
    *cut = false;
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        *missing = true;
        return {};
    }
    in.seekg(0, std::ios::end);
    const std::streamoff size = in.tellg();
    std::streamoff from = 0;
    if (size > 0 && static_cast<std::size_t>(size) > max) {
        from = size - static_cast<std::streamoff>(max);
        *cut = true;
    }
    in.seekg(from);
    std::string s(static_cast<std::size_t>(size - from), '\0');
    in.read(&s[0], static_cast<std::streamsize>(s.size()));
    s.resize(static_cast<std::size_t>(in.gcount()));
    if (*cut) {
        const auto nl = s.find('\n');
        if (nl != std::string::npos) s.erase(0, nl + 1);
    }
    return s;
}

}  // namespace

std::string redact_urls(const std::string &text) {
    static const char *const kSchemes[] = {"https://", "http://", "wss://", "ws://"};
    std::string out;
    out.reserve(text.size());
    std::size_t i = 0;
    while (i < text.size()) {
        std::size_t at = std::string::npos, len = 0;
        for (const char *s : kSchemes) {
            const std::size_t p = text.find(s, i);
            if (p < at) {
                at = p;
                len = std::strlen(s);
            }
        }
        if (at == std::string::npos) {
            out.append(text, i, std::string::npos);
            break;
        }
        std::size_t end = at + len;
        while (end < text.size() && !url_end(text[end])) ++end;
        const std::size_t cut = text.find_first_of("?#", at + len);
        if (cut != std::string::npos && cut < end) {
            out.append(text, i, cut - i);
            out += "?…";
        } else {
            out.append(text, i, end - i);
        }
        i = end;
    }
    return out;
}

std::string export_logs(const LogExportRequest &req, const std::string &dest_dir, std::string *error) {
    const std::time_t now = std::time(nullptr);
    std::tm tm{};
    localtime_r(&now, &tm);
    char stamp[32];
    std::strftime(stamp, sizeof stamp, "%Y%m%d-%H%M%S", &tm);
    char when[64];
    std::strftime(when, sizeof when, "%Y-%m-%d %H:%M:%S %z", &tm);

    std::ostringstream o;
    o << "Wren logs, saved " << when << "\n";
    for (const std::string &h : req.header) o << h << "\n";
    o << "Web addresses below keep their host and path only (query strings and fragments removed).\n"
         "Please read this file before posting it publicly.\n";

    static const char *const kLogs[][2] = {
        {"engine.log", "the web engine, this run"},
        {"engine.prev.log", "the web engine, the run before (the crash, if the app was reopened)"},
        {"events.log", "the browser's interface"},
    };
    for (const auto &l : kLogs) {
        bool missing = false, cut = false;
        const std::string body = read_tail(req.runtime_dir + "/" + l[0], req.max_bytes_per_log, &missing, &cut);
        o << "\n===== " << l[0] << " (" << l[1] << ")" << (missing ? ": not found" : "")
          << (cut ? ": only the end, it is long" : "") << " =====\n";
        o << redact_urls(body);
        if (!body.empty() && body.back() != '\n') o << "\n";
    }

    const std::string path = dest_dir + "/wren-logs-" + stamp + ".txt";
    std::FILE *f = std::fopen(path.c_str(), "wb");
    if (!f) {
        if (error) *error = std::strerror(errno);
        return {};
    }
    const std::string text = o.str();
    const bool ok = std::fwrite(text.data(), 1, text.size(), f) == text.size();
    const bool closed = std::fclose(f) == 0;
    if (!ok || !closed) {
        if (error) *error = "the drive is full or was removed";
        std::remove(path.c_str());
        return {};
    }
    return path;
}

}  // namespace browser
