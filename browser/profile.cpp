// SPDX-License-Identifier: GPL-3.0-or-later
#include "profile.hpp"

#include "wnk/async/async.hpp"
#include "wnk/log.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>

#include <sys/stat.h>
#include <unistd.h>

namespace browser {
namespace {

constexpr const char *kCat = "profile";
constexpr size_t kMaxHistory = 5000;
constexpr size_t kMaxBookmarks = 1000;
constexpr double kFlushIntervalMs = 2000;

int64_t now_s() { return static_cast<int64_t>(std::time(nullptr)); }

std::string read_file(const std::string &path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return {};
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// Writes `data` next to `path` and renames it over: a crash never leaves a half-written profile.
bool write_atomic(const std::string &path, const std::string &data) {
    const std::string tmp = path + ".tmp";
    FILE *f = std::fopen(tmp.c_str(), "wb");
    if (f == nullptr) return false;
    const bool ok = std::fwrite(data.data(), 1, data.size(), f) == data.size() && std::fflush(f) == 0 &&
                    ::fsync(fileno(f)) == 0;
    std::fclose(f);
    if (!ok || std::rename(tmp.c_str(), path.c_str()) != 0) {
        std::remove(tmp.c_str());
        return false;
    }
    return true;
}

std::string str(const nlohmann::json &j, const char *key) {
    auto it = j.find(key);
    return it != j.end() && it->is_string() ? it->get<std::string>() : std::string();
}

int64_t num(const nlohmann::json &j, const char *key) {
    auto it = j.find(key);
    return it != j.end() && it->is_number_integer() ? it->get<int64_t>() : 0;
}

}  // namespace

const std::vector<SearchEngine> kSearchEngines = {
    {"DuckDuckGo", "https://duckduckgo.com/?q="},
    {"Google", "https://www.google.com/search?q="},
    {"Bing", "https://www.bing.com/search?q="},
    {"Startpage", "https://www.startpage.com/do/search?q="},
    {"Ecosia", "https://www.ecosia.org/search?q="},
};

const std::vector<int> kZoomLevels = {1000, 1100, 1250, 1500, 1750, 2000};

void Profile::load(const std::string &dir) {
    dir_ = dir;
    ::mkdir(dir_.c_str(), 0700);

    auto parse = [](const std::string &text) { return nlohmann::json::parse(text, nullptr, false); };
    if (auto j = parse(read_file(path(kSettings))); j.is_object()) {
        settings.show_bookmarks_bar = j.value("show_bookmarks_bar", settings.show_bookmarks_bar);
        settings.search_engine = std::clamp(j.value("search_engine", 0), 0, static_cast<int>(kSearchEngines.size()) - 1);
        settings.zoom = std::clamp(j.value("zoom", 0), 0, static_cast<int>(kZoomLevels.size()) - 1);
        settings.restore_session = j.value("restore_session", false);
    }
    if (auto j = parse(read_file(path(kBookmarks))); j.is_array()) {
        for (const auto &b : j) {
            if (!b.is_object() || str(b, "url").empty()) continue;
            bookmarks_.push_back({str(b, "title"), str(b, "url"), num(b, "added")});
        }
    }
    if (auto j = parse(read_file(path(kHistory))); j.is_array()) {
        for (const auto &h : j) {
            if (!h.is_object() || str(h, "url").empty()) continue;
            history_.push_back({str(h, "title"), str(h, "url"), num(h, "time")});
        }
        std::stable_sort(history_.begin(), history_.end(), [](const HistoryEntry &a, const HistoryEntry &b) { return a.time > b.time; });
    }
    if (auto j = parse(read_file(path(kSession))); j.is_object() && j["tabs"].is_array()) {
        for (const auto &t : j["tabs"]) {
            if (t.is_object()) session_.push_back({str(t, "title"), str(t, "url")});
        }
        session_active_ = static_cast<size_t>(std::max<int64_t>(0, num(j, "active")));
    }
    WNK_LOGI(kCat, "profile %s: %zu bookmarks, %zu history entries, %zu session tabs", dir_.c_str(), bookmarks_.size(),
             history_.size(), session_.size());
}

std::string Profile::path(unsigned part) const {
    switch (part) {
    case kSettings: return dir_ + "/settings.json";
    case kBookmarks: return dir_ + "/bookmarks.json";
    case kHistory: return dir_ + "/history.json";
    default: return dir_ + "/session.json";
    }
}

std::string Profile::serialize(unsigned part) const {
    nlohmann::json j;
    switch (part) {
    case kSettings:
        j = {{"show_bookmarks_bar", settings.show_bookmarks_bar},
             {"search_engine", settings.search_engine},
             {"zoom", settings.zoom},
             {"restore_session", settings.restore_session}};
        break;
    case kBookmarks:
        j = nlohmann::json::array();
        for (const auto &b : bookmarks_) j.push_back({{"title", b.title}, {"url", b.url}, {"added", b.added}});
        break;
    case kHistory:
        j = nlohmann::json::array();
        for (const auto &h : history_) j.push_back({{"title", h.title}, {"url", h.url}, {"time", h.time}});
        break;
    default:
        j = {{"active", session_active_}, {"tabs", nlohmann::json::array()}};
        for (const auto &t : session_) j["tabs"].push_back({{"title", t.title}, {"url", t.url}});
        break;
    }
    // Invalid UTF-8 from a page title must not throw: replace it.
    return j.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
}

bool Profile::is_bookmarked(const std::string &url) const {
    return std::any_of(bookmarks_.begin(), bookmarks_.end(), [&](const Bookmark &b) { return b.url == url; });
}

void Profile::add_bookmark(const std::string &title, const std::string &url) {
    if (url.empty() || is_bookmarked(url) || bookmarks_.size() >= kMaxBookmarks) return;
    bookmarks_.push_back({title.empty() ? url : title, url, now_s()});
    dirty_ |= kBookmarks;
}

void Profile::remove_bookmark(const std::string &url) {
    const auto before = bookmarks_.size();
    bookmarks_.erase(std::remove_if(bookmarks_.begin(), bookmarks_.end(), [&](const Bookmark &b) { return b.url == url; }),
                     bookmarks_.end());
    if (bookmarks_.size() != before) dirty_ |= kBookmarks;
}

void Profile::move_bookmark(size_t from, size_t to) {
    if (from >= bookmarks_.size() || to >= bookmarks_.size() || from == to) return;
    Bookmark b = bookmarks_[from];
    bookmarks_.erase(bookmarks_.begin() + static_cast<long>(from));
    bookmarks_.insert(bookmarks_.begin() + static_cast<long>(to), b);
    dirty_ |= kBookmarks;
}

void Profile::add_visit(const std::string &title, const std::string &url) {
    if (url.empty() || url.rfind("about:", 0) == 0) return;
    if (!history_.empty() && history_.front().url == url) {
        history_.front().time = now_s();
        if (!title.empty()) history_.front().title = title;
    } else {
        history_.insert(history_.begin(), {title, url, now_s()});
        if (history_.size() > kMaxHistory) history_.resize(kMaxHistory);
    }
    dirty_ |= kHistory;
}

void Profile::set_history_title(const std::string &url, const std::string &title) {
    // Titles often arrive after the load finished: update the newest visit of that URL.
    for (auto &h : history_) {
        if (h.url == url) {
            if (h.title != title && !title.empty()) {
                h.title = title;
                dirty_ |= kHistory;
            }
            return;
        }
    }
}

void Profile::remove_history(const std::string &url, int64_t time) {
    const auto before = history_.size();
    history_.erase(std::remove_if(history_.begin(), history_.end(),
                                  [&](const HistoryEntry &h) { return h.url == url && h.time == time; }),
                   history_.end());
    if (history_.size() != before) dirty_ |= kHistory;
}

size_t Profile::clear_history(uint32_t seconds) {
    const auto before = history_.size();
    if (seconds == 0) {
        history_.clear();
    } else {
        const int64_t since = now_s() - seconds;
        history_.erase(std::remove_if(history_.begin(), history_.end(), [&](const HistoryEntry &h) { return h.time >= since; }),
                       history_.end());
    }
    dirty_ |= kHistory;
    return before - history_.size();
}

void Profile::set_session(std::vector<SessionTab> tabs, size_t active) {
    session_ = std::move(tabs);
    session_active_ = active;
    dirty_ |= kSession;
}

void Profile::flush(wnk::async::WorkerPool &pool, double now_ms) {
    if (dirty_ == 0 || dir_.empty() || now_ms - last_flush_ms_ < kFlushIntervalMs) return;
    last_flush_ms_ = now_ms;
    // Serialise here (main thread owns the data), write on a worker.
    std::vector<std::pair<std::string, std::string>> files;
    for (unsigned part : {kSettings, kBookmarks, kHistory, kSession}) {
        if (dirty_ & part) files.emplace_back(path(part), serialize(part));
    }
    dirty_ = 0;
    pool.submit([files = std::move(files)](const wnk::async::CancellationToken &) {
        for (const auto &[p, data] : files) {
            if (!write_atomic(p, data)) WNK_LOGW(kCat, "could not save %s: %s", p.c_str(), std::strerror(errno));
        }
    });
}

void Profile::flush_now() {
    if (dir_.empty()) return;
    for (unsigned part : {kSettings, kBookmarks, kHistory, kSession}) {
        if ((dirty_ & part) && !write_atomic(path(part), serialize(part)))
            WNK_LOGW(kCat, "could not save %s", path(part).c_str());
    }
    dirty_ = 0;
}

std::string day_label(int64_t t) {
    const time_t tt = static_cast<time_t>(t), now = std::time(nullptr);
    tm a{}, b{};
    localtime_r(&tt, &a);
    localtime_r(&now, &b);
    if (a.tm_year == b.tm_year && a.tm_yday == b.tm_yday) return "Today";
    const time_t yesterday = now - 86400;
    tm y{};
    localtime_r(&yesterday, &y);
    if (a.tm_year == y.tm_year && a.tm_yday == y.tm_yday) return "Yesterday";
    char buf[64];
    std::strftime(buf, sizeof(buf), "%A %d %B %Y", &a);
    return buf;
}

std::string time_label(int64_t t) {
    const time_t tt = static_cast<time_t>(t);
    tm a{};
    localtime_r(&tt, &a);
    char buf[16];
    std::strftime(buf, sizeof(buf), "%H:%M", &a);
    return buf;
}

}  // namespace browser
