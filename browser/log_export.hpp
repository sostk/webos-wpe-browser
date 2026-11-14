// SPDX-License-Identifier: GPL-3.0-or-later
// "Save logs to USB" (Settings → Help and feedback): the browser's logs in one text file on a USB drive, for a
// tester to attach to a bug report. The logs live in the app's runtime folder (/tmp/org.sostk.wpebrowser on
// the TV, cleared at reboot): engine.log (this run), engine.prev.log (the run before, the one that crashed
// when the app was reopened) and events.log (the UI). Web addresses keep their host and path only: query
// strings and fragments (search terms, tokens, session ids) are cut before anything is written.
#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace browser {

// Every http(s)/ws(s) URL in `text` loses its query and fragment: "https://a.b/p?q=1#x" → "https://a.b/p?…".
std::string redact_urls(const std::string &text);

struct LogExportRequest {
    std::string runtime_dir;               // where engine.log etc. are
    std::vector<std::string> header;       // "Wren 0.3.0", "TV: ...": written first, one per line
    std::size_t max_bytes_per_log = 1536 * 1024;  // the end of a longer log is kept
};

// Writes wren-logs-<YYYYMMDD-HHMMSS>.txt into `dest_dir`. Returns its path, or "" with `error` set.
std::string export_logs(const LogExportRequest &req, const std::string &dest_dir, std::string *error);

}  // namespace browser
