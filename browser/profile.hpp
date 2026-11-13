// SPDX-License-Identifier: GPL-3.0-or-later
// The browser profile: settings, bookmarks, history and the last session, kept in memory and saved as JSON
// files in the app's persistent data folder (<data>/profile/*.json, each replaced atomically).
//
// Threading: main thread only. Changes mark a part dirty; flush() (called every frame) writes dirty parts on
// the kit's worker pool at most once every two seconds; flush_now() writes synchronously (shutdown).
#pragma once

#include <cstdint>
#include <ctime>
#include <map>
#include <string>
#include <vector>

namespace wnk::async {
class WorkerPool;
}

namespace browser {

struct Settings {
    bool show_bookmarks_bar = true;
    int search_engine = 0;   // index into kSearchEngines
    int zoom = 0;            // index into kZoomLevels
    bool restore_session = false;  // "Continue where you left off"
    bool sharp_text = true;  // render pages at the screen's resolution (1.5x), not 2/3 of it
};

struct SearchEngine {
    const char *name;
    const char *query_prefix;  // the query is appended, URL-encoded
};
extern const std::vector<SearchEngine> kSearchEngines;
extern const std::vector<int> kZoomLevels;  // permille: 1000 = 100 %

struct Bookmark {
    std::string title, url;
    int64_t added = 0;  // unix seconds
};

struct HistoryEntry {
    std::string title, url;
    int64_t time = 0;  // unix seconds of the last visit
};

struct SessionTab {
    std::string title, url;
};

class Profile {
 public:
    // Reads everything under `dir` (created if missing). Missing or broken files give defaults.
    void load(const std::string &dir);

    Settings settings;
    const std::vector<Bookmark> &bookmarks() const { return bookmarks_; }
    const std::vector<HistoryEntry> &history() const { return history_; }  // newest first
    const std::vector<SessionTab> &session() const { return session_; }
    size_t session_active() const { return session_active_; }

    void settings_changed() { dirty_ |= kSettings; }

    bool is_bookmarked(const std::string &url) const;
    void add_bookmark(const std::string &title, const std::string &url);
    void remove_bookmark(const std::string &url);
    void move_bookmark(size_t from, size_t to);

    // A finished page load. Consecutive visits to one URL update the entry instead of adding one.
    void add_visit(const std::string &title, const std::string &url);
    void set_history_title(const std::string &url, const std::string &title);
    void remove_history(const std::string &url, int64_t time);
    // Removes visits from the last `seconds` (0 = everything); returns how many went.
    size_t clear_history(uint32_t seconds);

    void set_session(std::vector<SessionTab> tabs, size_t active);

    // Site icons (WPE has no favicon store): the icon URL a page declared, per host ("www." dropped). Kept for tiles,
    // bookmarks and history after the page is gone.
    const std::string &icon_for(const std::string &url) const;
    void set_icon(const std::string &page_url, const std::string &icon_url);

    void flush(wnk::async::WorkerPool &pool, double now_ms);
    void flush_now();

 private:
    enum : unsigned { kSettings = 1, kBookmarks = 2, kHistory = 4, kSession = 8, kIcons = 16 };
    std::string serialize(unsigned part) const;
    std::string path(unsigned part) const;

    std::string dir_;
    std::vector<Bookmark> bookmarks_;
    std::vector<HistoryEntry> history_;
    std::vector<SessionTab> session_;
    size_t session_active_ = 0;
    std::map<std::string, std::string> icons_;  // host -> icon URL
    unsigned dirty_ = 0;
    double last_flush_ms_ = 0;
};

// "Today", "Yesterday" or a date like "Monday 10 November 2026" (local time).
std::string day_label(int64_t unix_seconds);
// "14:05" (local time).
std::string time_label(int64_t unix_seconds);

}  // namespace browser
