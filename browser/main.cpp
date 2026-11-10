// SPDX-License-Identifier: GPL-3.0-or-later
// WPE Browser: a WebKit (WPE) web browser for LG webOS TVs, with tabs, bookmarks, history and settings in
// the style of desktop Chromium.
//
// This process is the UI: the kit's shell, a tab strip, a toolbar with an omnibox, the bookmarks bar, and
// either the native New Tab page or the PageView; Settings, History and Bookmarks are native pages pushed on
// top (screens.cpp). WebKit runs in engine/wpe-engine (built with a glibc 2.28 toolchain), which keeps one
// WebKit view per open tab and renders the active one into shared memory; see engine_client.hpp and
// common/wpe_ipc.h. Settings, bookmarks, history and the session live in profile.cpp.
//
// Remote: in a page, the D-pad moves between links (WebKit spatial navigation) and OK follows them. Back goes
// back in history, or to the omnibox when there is none. Colour keys: Red = omnibox, Green = reload,
// Yellow = next tab, Blue = new tab. CH+/CH- = page up/down. The Magic Remote pointer is a mouse in pages.
#include "chrome.hpp"
#include "direct_owner.hpp"
#include "engine_client.hpp"
#include "keys.hpp"
#include "page_view.hpp"
#include "profile.hpp"
#include "screens.hpp"
#include "storage.hpp"
#include "../engine/webos-backend/protocol.h"

#include "wnk/app/app.hpp"
#include "wnk/log.hpp"
#include "wnk/platform/probe.hpp"
#include "wnk/ui/navigation.hpp"
#include "wnk/ui/ui.hpp"
#include "wnk/ui/widgets.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

#include <sys/stat.h>

namespace {
// A string launch parameter ({"target": "https://..."}), empty when absent or not a string.
std::string launch_param(const std::string &params_json, const char *key) {
    if (params_json.empty()) return {};
    const auto j = nlohmann::json::parse(params_json, nullptr, false);
    if (!j.is_object()) return {};
    const auto it = j.find(key);
    return it != j.end() && it->is_string() ? it->get<std::string>() : std::string();
}


using browser::TabId;
namespace pal = browser::palette;

constexpr const char *kCat = "browser";
constexpr const char *kVersion = "0.2.0";
// Pages are rendered at this fraction of the view's size in 1920x1080 logical units and scaled up by the
// GPU: CPU rendering at about 1280 px wide keeps scrolling usable, and CSS px at that size read well from a sofa.
constexpr float kRenderScale = 2.0f / 3.0f;
constexpr double kChromeSlideMs = 160;     // toolbar slide in/out
constexpr double kChromePeekMs = 2500;   // after Back the toolbar stays over the page this long
constexpr float kChromeRevealEdge = 6.0f;  // logical px at the top of the screen that bring the toolbar back
constexpr int kMaxRestarts = 3;
// Background tabs beyond this many keep only their URL and title (Chromium's tab discarding); they reload
// when shown again. Each live tab costs WebKit memory, and a TV has little to spare.
constexpr size_t kMaxLiveTabs = 4;
constexpr size_t kMaxTabs = 12;

const std::vector<browser::NewTabPage::Shortcut> kShortcuts = {
    {"YouTube", "https://www.youtube.com/"},   {"Wikipedia", "https://www.wikipedia.org/"},
    {"DuckDuckGo", "https://duckduckgo.com/"}, {"Google", "https://www.google.com/"},
    {"Reddit", "https://www.reddit.com/"},     {"GitHub", "https://github.com/"},
    {"BBC News", "https://www.bbc.com/news"},  {"Weather", "https://wttr.in/"},
};

std::string url_encode(const std::string &s) {
    static const char *hex = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out += static_cast<char>(c);
        } else if (c == ' ') {
            out += '+';
        } else {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 15];
        }
    }
    return out;
}

// What the user typed -> a URL: a URL as is, a host name with https://, anything else a search.
std::string to_url(std::string text, const std::string &search_prefix) {
    const auto b = text.find_first_not_of(" \t");
    const auto e = text.find_last_not_of(" \t");
    if (b == std::string::npos) return {};
    text = text.substr(b, e - b + 1);
    if (text.find("://") != std::string::npos || text.rfind("about:", 0) == 0 || text.rfind("data:", 0) == 0) return text;
    const bool hostish = text.find(' ') == std::string::npos &&
                         (text.find('.') != std::string::npos || text.rfind("localhost", 0) == 0);
    return hostish ? "https://" + text : search_prefix + url_encode(text);
}

using browser::make_dir;

struct Tab {
    TabId id = 0;
    std::string url, title;
    float progress = 0;
    bool loading = false;
    bool can_back = false, can_forward = false;
    bool live = false;  // has a WebKit view in the engine
    bool at_top = true;  // the page is scrolled to its top (the toolbar peeks over it then)
    double last_shown_ms = 0;
};

class App;

// The whole window: tab strip, toolbar, bookmarks bar, progress line, content (New Tab page or PageView).
class BrowserScreen : public wnk::ui::Screen {
 public:
    BrowserScreen(App &app, browser::EngineClient &engine);
    // Back in the chrome returns to the content instead of starting the shell's double-Back exit.
    bool on_back(wnk::ui::Ui &ui) override;

    App &app_;
    browser::TabStrip *strip_ = nullptr;
    browser::IconButton *back_ = nullptr, *forward_ = nullptr, *reload_ = nullptr, *menu_ = nullptr;
    browser::Omnibox *omnibox_ = nullptr;
    browser::BookmarksBar *bookmarks_ = nullptr;
    browser::ProgressLine *progress_ = nullptr;
    wnk::ui::Column *chrome_ = nullptr;             // tabs, toolbar, bookmarks: drawn over the page
    wnk::ui::Row *toolbar_ = nullptr;
    browser::ProgressLine *thin_progress_ = nullptr;  // load progress while the chrome is hidden
    browser::PageView *page_ = nullptr;
    browser::NewTabPage *ntp_ = nullptr;
    wnk::ui::Label *message_ = nullptr;
};

class App : public wnk::app::Delegate {
 public:
    App() : engine_(callbacks()) {}

    // ---- tab operations (also used by the chrome's handlers) ---------------------------------------------

    void new_tab(const std::string &url = {}) {
        if (tabs_.size() >= kMaxTabs) {
            ctx_->ui().toast("Close a tab to open another (" + std::to_string(kMaxTabs) + " at most)");
            return;
        }
        Tab t;
        t.id = next_id_++;
        t.url = url;
        tabs_.push_back(t);
        activate(t.id);
    }

    void close_tab(TabId id) {
        auto it = find(id);
        if (it == tabs_.end()) return;
        if (it->live) engine_.tab_close(id);
        const size_t index = static_cast<size_t>(it - tabs_.begin());
        tabs_.erase(it);
        if (tabs_.empty()) {
            active_ = 0;
            new_tab();  // a TV browser never closes its window: start over with a New Tab page
            return;
        }
        if (id == active_) {
            active_ = 0;
            activate(tabs_[std::min(index, tabs_.size() - 1)].id);
        } else {
            refresh_chrome();
        }
    }

    void activate(TabId id) {
        Tab *t = tab(id);
        if (t == nullptr) return;
        const bool changed = id != active_;
        active_ = id;
        t->last_shown_ms = ctx_->ui().now_ms();
        if (!t->url.empty() && !t->live) open_in_engine(*t, t->url);
        if (t->live) engine_.tab_activate(id);
        screen_->page_->show_tab(t->live ? id : 0);
        screen_->page_->ime(ctx_->ui(), false);
        refresh_chrome();
        if (changed) focus_content();
    }

    void next_tab() {
        if (tabs_.size() < 2) return;
        auto it = find(active_);
        const size_t i = it == tabs_.end() ? 0 : static_cast<size_t>(it - tabs_.begin());
        activate(tabs_[(i + 1) % tabs_.size()].id);
    }

    // Loads `text` (a URL or a search) in the active tab.
    void navigate(const std::string &text) {
        const std::string url = to_url(text, browser::kSearchEngines[static_cast<size_t>(profile_.settings.search_engine)].query_prefix);
        Tab *t = tab(active_);
        if (url.empty() || t == nullptr) return;
        if (t->live) {
            t->url = url;
            engine_.load_uri(t->id, url);
        } else {
            open_in_engine(*t, url);
            engine_.tab_activate(t->id);
            screen_->page_->show_tab(t->id);
        }
        refresh_chrome();
        ctx_->ui().focus(screen_->page_);
    }

    void go_back() {
        if (Tab *t = tab(active_); t && t->live) engine_.go_back(t->id);
    }
    void go_forward() {
        if (Tab *t = tab(active_); t && t->live) engine_.go_forward(t->id);
    }
    void reload_or_stop() {
        Tab *t = tab(active_);
        if (t == nullptr || !t->live) return;
        if (t->loading)
            engine_.stop_loading(t->id);
        else
            engine_.reload(t->id);
    }
    void go_home() {
        // Home = this tab's New Tab page, as in Chromium with the default home page.
        Tab *t = tab(active_);
        if (t == nullptr) return;
        if (t->live) engine_.tab_close(t->id);
        Tab fresh;
        fresh.id = t->id;
        fresh.last_shown_ms = ctx_->ui().now_ms();
        *t = fresh;
        screen_->page_->show_tab(0);
        refresh_chrome();
        focus_content();
    }

    // The star in the omnibox: bookmark the page, or remove the bookmark.
    void toggle_bookmark() {
        Tab *t = tab(active_);
        if (t == nullptr || t->url.empty()) return;
        if (profile_.is_bookmarked(t->url)) {
            profile_.remove_bookmark(t->url);
            ctx_->ui().toast("Bookmark removed");
        } else {
            profile_.add_bookmark(t->title, t->url);
            ctx_->ui().toast("Bookmark added");
        }
        refresh_bookmarks();
        refresh_chrome();
    }

    void show_menu() {
        auto &ui = ctx_->ui();
        std::vector<browser::MenuItem> items = {
            {"add", "New tab", "Blue", [this](wnk::ui::Ui &) { new_tab(); }},
            {"history", "History", "", [this](wnk::ui::Ui &u) { push_page(u, browser::make_history_screen(profile_, hooks())); }},
            {"bookmarks", "Bookmarks", "", [this](wnk::ui::Ui &u) { push_page(u, browser::make_bookmarks_screen(profile_, hooks())); }},
            {"delete", "Clear browsing data", "", [this](wnk::ui::Ui &u) { push_page(u, browser::make_clear_data_screen(hooks())); }},
            {"settings", "Settings", "", [this](wnk::ui::Ui &u) { push_page(u, browser::make_settings_screen(profile_, hooks())); }},
            {"close", "Close tab", "", [this](wnk::ui::Ui &) { close_tab(active_); }},
            {"exit", "Exit", "", [this](wnk::ui::Ui &) { ctx_->quit("menu exit"); }},
        };
        std::string footer = std::string("WPE Browser ") + kVersion;
        if (!engine_version_.empty()) footer += "  ·  WebKit " + engine_version_;
        browser::show_menu(ui, std::move(items), footer);
    }

    void show_bookmarks_page(wnk::ui::Ui &ui) { push_page(ui, browser::make_bookmarks_screen(profile_, hooks())); }

    // The chrome is shown unless the page itself has focus (Red, or Back with no history, moves focus to it; so
    // does the pointer at the top edge). While the page has focus the chrome still peeks over it when the page is
    // scrolled to its top and for kChromePeekMs after Back (focus stays on the page). It slides up out of view in
    // kChromeSlideMs and is then hidden, so its buttons can't take focus.
    bool chrome_wanted(wnk::ui::Ui &ui) {
        const Tab *t = tab(active_);
        const bool page_mode = ui.focused() == screen_->page_ && t != nullptr && t->live && !t->url.empty() &&
                               !screen_->message_->visible() && ui.top_dialog() == nullptr && nav_->depth() == 1;
        if (!page_mode) return true;
        return t->at_top || ui.now_ms() < peek_until_ms_;
    }

    void peek_chrome() { peek_until_ms_ = ctx_->ui().now_ms() + kChromePeekMs; }

    void update_chrome(wnk::ui::Ui &ui) {
        auto *chrome = screen_->chrome_;
        // The column itself spans the screen (a plain View places it with the full height); the chrome ends at the
        // bottom of its last visible row.
        float bottom = screen_->progress_->frame().y + screen_->progress_->frame().h;
        if (screen_->message_->visible()) bottom = std::max(bottom, screen_->message_->frame().y + screen_->message_->frame().h);
        const float h = std::max(0.0f, bottom - chrome->frame().y);
        chrome_h_ = h;
        // The New Tab page starts below the chrome; a web page lies under it.
        const float pad = h > 0 ? h : 0;
        if (screen_->ntp_->padding().top != pad) screen_->ntp_->set_padding({0, pad, 0, 0});
        const bool want = chrome_wanted(ui);
        const double now = ui.now_ms();
        const double dt = last_chrome_ms_ > 0 ? now - last_chrome_ms_ : 0;
        last_chrome_ms_ = now;
        const float step = static_cast<float>(dt / kChromeSlideMs);
        chrome_shown_ = want ? std::min(1.0f, chrome_shown_ + step) : std::max(0.0f, chrome_shown_ - step);
        if (want && !chrome->visible()) chrome->set_visible(true);
        chrome->set_translation({0, -(1.0f - chrome_shown_) * h});
        if (!want && chrome_shown_ <= 0.0f && chrome->visible()) chrome->set_visible(false);
        screen_->thin_progress_->set_visible(!chrome->visible());
    }

    // Hidden views can't take focus: show the chrome first (it slides in from its current position).
    void focus_omnibox(wnk::ui::Ui &ui) {
        screen_->chrome_->set_visible(true);
        ui.focus(&omnibox_field());
    }

    // Over the visible chrome, pointer events belong to the chrome's widgets, not to the page underneath.
    bool over_chrome(wnk::Point l) const {
        if (!screen_->chrome_->visible()) return false;
        const wnk::Rect r = screen_->chrome_->screen_rect();
        return l.y >= r.y && l.y < r.y + chrome_h_ && l.x >= r.x && l.x < r.x + r.w;
    }

    bool content_focused() {
        auto *f = ctx_->ui().focused();
        return f == screen_->page_ || (f != nullptr && screen_->ntp_->is_ancestor_of(f));
    }

    void focus_content() {
        const Tab *t = tab(active_);
        if (t != nullptr && t->url.empty())
            ctx_->ui().focus(&screen_->ntp_->search().field());
        else
            ctx_->ui().focus(screen_->page_);
    }

    // ---- kit delegate ------------------------------------------------------------------------------------

    void on_start(wnk::app::Context &ctx) override {
        ctx_ = &ctx;
        browser::set_icon_dir("file://" + ctx.launch().app_dir + "/icons/");
        data_dir_ = data_dir(ctx);
        profile_.load(data_dir_ + "/profile");
        auto &ui = ctx.ui();
        nav_ = &ui.root().add<wnk::ui::Navigator>();
        auto screen = std::make_unique<BrowserScreen>(*this, engine_);
        screen_ = screen.get();
        nav_->push(ui, std::move(screen));
        keymap_ = wnk::input::default_keymap();
        refresh_bookmarks();
        start_engine();
        restore_or_new_tab();
        open_launch_target(ctx.launch().params_json);
        WNK_LOGI(kCat, "browser ui ready");
    }

    // Launched (or relaunched) with {"target": url} (what webOS sends a browser) or {"url": url}.
    void on_relaunch(wnk::app::Context &, const std::string &params_json) override { open_launch_target(params_json); }

    void open_launch_target(const std::string &params_json) {
        std::string url = launch_param(params_json, "target");
        if (url.empty()) url = launch_param(params_json, "url");
        if (url.empty()) return;
        WNK_LOGI(kCat, "launch target %s", url.c_str());
        const Tab *t = tab(active_);
        if (t == nullptr || !t->url.empty()) {
            new_tab();
            if (tab(active_) == nullptr || !tab(active_)->url.empty()) return;  // tab limit reached
        }
        navigate(url);
    }

    void on_frame(wnk::app::Context &ctx, float) override {
        engine_.poll();
        direct_.dispatch();
        if (direct_fallback_pending_) {
            // Direct presentation failed: start again with SHM frames (outside engine_.poll()).
            direct_fallback_pending_ = false;
            engine_.stop();
            direct_.close();
            screen_->page_->set_direct(false);
            for (Tab &t : tabs_) t.live = false, t.loading = false;
            screen_->page_->show_tab(0);
            start_engine();
            if (Tab *t = tab(active_)) activate(t->id);
        }
        if (!focused_once_) {
            // Focus set in on_start lands before the first layout; settle it on the first frame.
            focused_once_ = true;
            focus_content();
        }
        // Keep the page size in step with the content area.
        const wnk::Rect r = screen_->page_->screen_rect();
        if (r.w > 0 && r.h > 0) {
            const auto w = static_cast<uint32_t>(r.w * kRenderScale), h = static_cast<uint32_t>(r.h * kRenderScale);
            if (w != screen_->page_->page_width() || h != screen_->page_->page_height()) {
                screen_->page_->set_page_size(w, h);
                engine_.resize(w, h);
            }
        }
        if (Tab *t = tab(active_)) {
            screen_->progress_->set(t->loading, t->progress);
            screen_->thin_progress_->set(t->loading, t->progress);
        }
        update_chrome(ctx.ui());
        if (restart_pending_ && ctx_->ui().now_ms() >= restart_at_ms_) {
            restart_pending_ = false;
            start_engine();
            if (Tab *t = tab(active_)) activate(t->id);
        }
        profile_.flush(ctx.pool(), ctx.ui().now_ms());
    }

    bool on_key(wnk::app::Context &ctx, const wnk::input::KeyEvent &e) override {
        auto &ui = ctx.ui();
        using A = wnk::input::Action;
        const A action = keymap_.lookup(e.scancode, e.keycode);
        // Colour keys work on the browser screen, except while typing or in a dialog.
        if (nav_->depth() == 1 && ui.text_input_view() == nullptr && ui.top_dialog() == nullptr && e.down && !e.repeat) {
            switch (action) {
            case A::Red: focus_omnibox(ui); return true;
            case A::Green: reload_or_stop(); return true;
            case A::Yellow: next_tab(); return true;
            case A::Blue: new_tab(); return true;
            default: break;
            }
        }
        // The page lies under the chrome, so spatial navigation never finds it "below": Down from the toolbar
        // or the bookmarks bar goes back to the page (or the New Tab page), which hides the chrome again.
        if (action == A::Down && e.down && nav_->depth() == 1 && ui.text_input_view() == nullptr &&
            ui.top_dialog() == nullptr && ui.focused() != nullptr &&
            (screen_->toolbar_->is_ancestor_of(ui.focused()) || screen_->bookmarks_->is_ancestor_of(ui.focused()))) {
            const bool bookmarks_below = screen_->bookmarks_->visible() && screen_->toolbar_->is_ancestor_of(ui.focused()) &&
                                         screen_->bookmarks_->has_items();
            if (!bookmarks_below) {
                focus_content();
                return true;
            }
        }
        const Tab *t = tab(active_);
        if (ui.focused() != screen_->page_ || screen_->page_->editing() || t == nullptr || !t->live ||
            !engine_.running())
            return false;
        switch (action) {
        case A::Back:
            if (e.down && !e.repeat) {
                if (t->can_back) {
                    engine_.go_back(t->id);
                    peek_chrome();  // show where we are (the address) while the page stays focused
                } else
                    focus_omnibox(ui);
            }
            return true;
        case A::Exit:
        case A::Home:
            return false;  // the shell and the TV handle these
        case A::Red:
        case A::Green:
        case A::Yellow:
        case A::Blue:
            return true;  // handled above on press
        case A::ChannelUp: engine_.key(e.down, 0xff55, 104 + 8, 0); return true;    // Page_Up
        case A::ChannelDown: engine_.key(e.down, 0xff56, 109 + 8, 0); return true;  // Page_Down
        default: break;
        }
        const browser::WebKey k = browser::web_key_from_sdl(e.keycode);
        if (k.keyval == 0) return false;
        engine_.key(e.down, k.keyval, k.keycode, 0);
        return true;
    }

    bool on_pointer(wnk::app::Context &ctx, const wnk::input::PointerEvent &p) override {
        using T = wnk::input::PointerEvent::Type;
        const Tab *t = tab(active_);
        if (!engine_.running() || t == nullptr || !t->live || !screen_->page_->visible() || nav_->depth() != 1) return false;
        if (p.type == T::Hidden || p.type == T::Left) {
            leave_page();
            return false;
        }
        if (p.type == T::Wheel) {
            if (!pointer_in_page_) return false;
            engine_.scroll(last_x_, last_y_, 0, static_cast<int32_t>(-p.wheel * 120.0f));
            return true;
        }
        if (p.type == T::Shown) return false;
        const wnk::Point l = ctx.renderer().mapping().to_logical({p.x, p.y});
        int32_t x = 0, y = 0;
        if (!screen_->chrome_->visible() && l.y < kChromeRevealEdge && p.type == T::Move) {
            leave_page();
            focus_omnibox(ctx.ui());  // pointer at the top edge: bring the chrome back
            return true;
        }
        if (ctx.ui().top_dialog() != nullptr || over_chrome(l) || !screen_->page_->to_page(l, x, y)) {
            leave_page();
            return false;  // over the chrome or a dialog: the kit's widgets handle it
        }
        pointer_in_page_ = true;
        last_x_ = x;
        last_y_ = y;
        switch (p.type) {
        case T::Move: engine_.pointer(WPE_POINTER_MOVE, 0, x, y, p.time_ms); break;
        case T::Press:
            ctx.ui().focus(screen_->page_);
            engine_.pointer(WPE_POINTER_DOWN, static_cast<uint32_t>(p.button), x, y, p.time_ms);
            break;
        case T::Release: engine_.pointer(WPE_POINTER_UP, static_cast<uint32_t>(p.button), x, y, p.time_ms); break;
        default: break;
        }
        return true;
    }

    void on_lifecycle(wnk::app::Context &, const wnk::app::Lifecycle::Transition &t) override {
        using S = wnk::app::State;
        if (t.to == S::Suspended) {
            engine_.set_visible(false);
            save_session();
            profile_.flush_now();  // the TV may close a backgrounded app without warning
        }
        if (t.to == S::Running) engine_.set_visible(true);
    }

    void on_stop(wnk::app::Context &) override {
        save_session();
        profile_.flush_now();
        engine_.stop();
    }

 private:
    std::vector<Tab>::iterator find(TabId id) {
        return std::find_if(tabs_.begin(), tabs_.end(), [id](const Tab &t) { return t.id == id; });
    }
    Tab *tab(TabId id) {
        auto it = find(id);
        return it == tabs_.end() ? nullptr : &*it;
    }

    wnk::ui::TextField &omnibox_field() { return screen_->omnibox_->field(); }

    void push_page(wnk::ui::Ui &ui, std::unique_ptr<wnk::ui::Screen> screen) {
        screen_->page_->ime(ui, false);
        leave_page();
        nav_->push(ui, std::move(screen));
    }

    browser::PageHooks hooks() {
        browser::PageHooks h;
        h.close = [this](wnk::ui::Ui &ui) { nav_->pop(ui); };
        h.open = [this](wnk::ui::Ui &ui, const std::string &url) {
            nav_->pop_to_root(ui);
            navigate(url);
        };
        h.open_new_tab = [this](wnk::ui::Ui &ui, const std::string &url) {
            nav_->pop_to_root(ui);
            new_tab(url);
        };
        h.settings_changed = [this](wnk::ui::Ui &) { apply_settings(); };
        h.bookmarks_changed = [this](wnk::ui::Ui &) {
            refresh_bookmarks();
            refresh_chrome();
        };
        h.show_clear_data = [this](wnk::ui::Ui &ui) { nav_->push(ui, browser::make_clear_data_screen(hooks())); };
        h.show_history = [this](wnk::ui::Ui &ui) { nav_->push(ui, browser::make_history_screen(profile_, hooks())); };
        h.show_bookmarks = [this](wnk::ui::Ui &ui) { nav_->push(ui, browser::make_bookmarks_screen(profile_, hooks())); };
        h.clear = [this](wnk::ui::Ui &ui, bool history, bool cookies, bool cache, uint32_t seconds) {
            clear_browsing_data(ui, history, cookies, cache, seconds);
        };
        h.engine_version = [this]() { return engine_version_; };
        return h;
    }

    void clear_browsing_data(wnk::ui::Ui &ui, bool history, bool cookies, bool cache, uint32_t seconds) {
        if (history) {
            const size_t n = profile_.clear_history(seconds);
            WNK_LOGI(kCat, "cleared %zu history entries", n);
        }
        const uint32_t what = (cookies ? WPE_CLEAR_COOKIES_AND_SITE_DATA : 0u) | (cache ? WPE_CLEAR_CACHE : 0u);
        if (what != 0 && engine_.running()) {
            engine_.clear_data(what, seconds);  // the toast comes with the engine's answer
        } else {
            if (what != 0) WNK_LOGW(kCat, "engine not running: cookies/cache not cleared");
            ui.toast(what != 0 ? "History cleared. The web engine is not running, so cookies and cache were kept."
                               : "Browsing data cleared");
        }
    }

    void apply_settings() {
        screen_->bookmarks_->set_visible(profile_.settings.show_bookmarks_bar);
        engine_.set_zoom(static_cast<uint32_t>(browser::kZoomLevels[static_cast<size_t>(profile_.settings.zoom)]));
        // Pages lay out at 2/3 of the screen (kRenderScale); 1/kRenderScale renders them at full resolution.
        // Direct presentation always does: the page surface is shown 1:1, not scaled to the page area.
        const bool full = profile_.settings.sharp_text || screen_->page_->direct();
        engine_.set_scale(full ? static_cast<uint32_t>(1000.0f / kRenderScale + 0.5f) : 1000u);
    }

    void refresh_bookmarks() {
        std::vector<browser::BookmarksBar::Item> items;
        for (const auto &b : profile_.bookmarks()) items.push_back({b.title, b.url});
        screen_->bookmarks_->rebuild(ctx_->ui(), items);
        screen_->bookmarks_->set_visible(profile_.settings.show_bookmarks_bar);
    }

    void restore_or_new_tab() {
        const auto &session = profile_.session();
        if (profile_.settings.restore_session && !session.empty()) {
            // Tabs come back discarded (URL and title only); only the active one loads.
            for (const auto &s : session) {
                Tab t;
                t.id = next_id_++;
                t.url = s.url;
                t.title = s.title;
                tabs_.push_back(t);
            }
            activate(tabs_[std::min(profile_.session_active(), tabs_.size() - 1)].id);
            WNK_LOGI(kCat, "restored %zu tabs", tabs_.size());
        } else {
            new_tab();
        }
    }

    void save_session() {
        std::vector<browser::SessionTab> s;
        size_t active = 0;
        for (size_t i = 0; i < tabs_.size(); ++i) {
            s.push_back({tabs_[i].title, tabs_[i].url});
            if (tabs_[i].id == active_) active = i;
        }
        profile_.set_session(std::move(s), active);
    }

    void open_in_engine(Tab &t, const std::string &url) {
        // Discard the least recently shown background tabs first (they keep URL and title).
        size_t live = static_cast<size_t>(std::count_if(tabs_.begin(), tabs_.end(), [](const Tab &x) { return x.live; }));
        while (live >= kMaxLiveTabs) {
            Tab *victim = nullptr;
            for (Tab &x : tabs_) {
                if (x.live && x.id != t.id && x.id != active_ &&
                    (victim == nullptr || x.last_shown_ms < victim->last_shown_ms))
                    victim = &x;
            }
            if (victim == nullptr) break;
            WNK_LOGI(kCat, "discarding background tab %u (%s)", victim->id, victim->url.c_str());
            engine_.tab_close(victim->id);
            victim->live = false;
            victim->loading = false;
            --live;
        }
        engine_.tab_open(t.id, url);
        t.live = true;
        t.url = url;
    }

    void refresh_chrome() {
        std::vector<browser::TabInfo> infos;
        for (const Tab &t : tabs_) infos.push_back({t.id, t.title, t.url, t.loading});
        auto &ui = ctx_->ui();
        const bool strip_focused = ui.focused() != nullptr && screen_->strip_->is_ancestor_of(ui.focused());
        screen_->strip_->rebuild(ui, infos, active_);
        if (strip_focused) {
            if (auto *tv = screen_->strip_->tab_view(active_)) ui.focus(tv);
        }
        const Tab *t = tab(active_);
        const bool page = t != nullptr && !t->url.empty();
        screen_->ntp_->set_visible(!page);
        screen_->page_->set_visible(page);
        screen_->back_->set_active(t != nullptr && t->can_back);
        screen_->forward_->set_active(t != nullptr && t->can_forward);
        screen_->reload_->set_icon(t != nullptr && t->loading ? "close" : "reload");
        screen_->reload_->set_active(page);
        screen_->omnibox_->set_star(page, page && profile_.is_bookmarked(t->url));
        if (ui.focused() != &omnibox_field()) screen_->omnibox_->show_url(t != nullptr ? t->url : std::string());
        save_session();  // cheap: written at most every 2 s by Profile::flush
    }

    browser::EngineCallbacks callbacks() {
        browser::EngineCallbacks cb;
        cb.ready = [this](const WpeEngineReady &r) {
            engine_version_ = std::to_string(r.webkit_major) + "." + std::to_string(r.webkit_minor) + "." +
                              std::to_string(r.webkit_micro);
            WNK_LOGI(kCat, "engine ready: WPE WebKit %s", engine_version_.c_str());
            screen_->message_->set_visible(false);
            apply_settings();
        };
        cb.uri = [this](TabId id, const std::string &uri) {
            if (Tab *t = tab(id); t && t->live) {
                t->url = uri;
                if (id == active_) refresh_chrome();
            }
        };
        cb.title = [this](TabId id, const std::string &title) {
            if (Tab *t = tab(id); t && t->live) {
                t->title = title;
                profile_.set_history_title(t->url, title);
                refresh_chrome();
            }
        };
        cb.progress = [this](TabId id, float v) {
            if (Tab *t = tab(id)) t->progress = v;
        };
        cb.load = [this](TabId id, WpeLoadEvent e) {
            Tab *t = tab(id);
            if (t == nullptr) return;
            if (e == WPE_LOAD_FINISHED) profile_.add_visit(t->title, t->url);
            const bool loading = e == WPE_LOAD_STARTED || e == WPE_LOAD_COMMITTED;
            if (loading != t->loading) {
                t->loading = loading;
                if (loading) t->progress = 0;
                refresh_chrome();
            }
        };
        cb.scroll = [this](TabId id, bool at_top) {
            if (Tab *t = tab(id)) t->at_top = at_top;
        };
        cb.history = [this](TabId id, bool back, bool forward) {
            if (Tab *t = tab(id)) {
                t->can_back = back;
                t->can_forward = forward;
                if (id == active_) refresh_chrome();
            }
        };
        cb.ime = [this](TabId id, const WpeIme &m) {
            if (id == active_) screen_->page_->ime(ctx_->ui(), m.focused != 0);
        };
        cb.error = [this](TabId id, WpeErrorKind kind, uint32_t code, const std::string &msg) {
            WNK_LOGW(kCat, "engine error tab %u %d/%u: %s", id, static_cast<int>(kind), code, msg.c_str());
            if (kind == WPE_ERROR_WEB_PROCESS_CRASHED && id == active_)
                ctx_->ui().toast("This page stopped (" + msg + "). Press Green to reload.", 5000);
        };
        cb.data_cleared = [this](uint32_t, bool ok) {
            ctx_->ui().toast(ok ? "Browsing data cleared" : "Some browsing data could not be cleared");
        };
        cb.present = [this](TabId id, const WpePresent &m) {
            if (id == 0) {  // sent once at engine start: the mode it actually uses
                const bool direct = m.mode == WPE_PRESENT_DIRECT;
                if (!direct && direct_.active()) WNK_LOGW(kCat, "the engine could not load direct presentation");
                screen_->page_->set_direct(direct);
                apply_settings();
                return;
            }
            if (m.status == WPEB_WEBOS_STATUS_FIRST_FRAME) {
                WNK_LOGI(kCat, "tab %u: page surface %ux%u on screen", id, m.width, m.height);
                screen_->page_->direct_shown(id, true);
            } else if (m.status == WPEB_WEBOS_STATUS_DETACHED || m.status == WPEB_WEBOS_STATUS_OWNER_GONE) {
                screen_->page_->direct_shown(id, false);
            }
            if (m.status < 0 && !direct_failed_) {
                WNK_LOGE(kCat, "direct presentation failed for tab %u (status %d); using normal drawing", id, m.status);
                direct_failed_ = true;
                direct_fallback_pending_ = true;
                ctx_->ui().toast("GPU page drawing failed; switching to normal drawing", 4000);
            }
        };
        cb.exited = [this](const std::string &why) {
            screen_->page_->ime(ctx_->ui(), false);
            for (Tab &t : tabs_) t.live = false, t.loading = false;
            screen_->page_->show_tab(0);
            refresh_chrome();
            if (restarts_ < kMaxRestarts) {
                ++restarts_;
                ctx_->ui().toast("The web engine stopped (" + why + "). Restarting...", 4000);
                restart_pending_ = true;
                restart_at_ms_ = ctx_->ui().now_ms() + 1000;
            } else {
                show_message("The web engine keeps stopping (" + why + "). Restart the app to try again.");
            }
        };
        return cb;
    }

    void start_engine() {
        auto &ctx = *ctx_;
        browser::EngineLaunch l;
        const char *override_dir = std::getenv("WPE_BROWSER_ENGINE_DIR");
        l.engine_dir = override_dir && *override_dir ? override_dir : ctx.launch().app_dir + "/engine";
        l.runtime_dir = ctx.runtime_dir().empty() ? "/tmp" : ctx.runtime_dir();
        // WPE_BROWSER_RENDERER=gpu selects the TV's Mali through WebKit's default-EGL-display patch
        // (root probes passed; installed-app validation remains: see NOTES.md). Software stays the default.
        // Launch parameters {"wpebRenderer":"gpu","wpebPresent":"direct"} do the same as the environment (an
        // installed app gets no environment from the launcher); a parameter wins.
        const std::string &params = ctx.launch().params_json;
        std::string renderer = launch_param(params, "wpebRenderer");
        if (renderer.empty() && std::getenv("WPE_BROWSER_RENDERER")) renderer = std::getenv("WPE_BROWSER_RENDERER");
        l.gpu = renderer == "gpu";
        // WPE_BROWSER_PRESENT=direct (with gpu): pages draw straight into a surface below this window
        // (docs/webkit-like-chromium.md, part 2). Experimental until checked on the TV; falls back by itself.
        std::string present = launch_param(params, "wpebPresent");
        if (present.empty() && std::getenv("WPE_BROWSER_PRESENT")) present = std::getenv("WPE_BROWSER_PRESENT");
        WNK_LOGI(kCat, "renderer %s, presentation %s", l.gpu ? "gpu" : "software", present.empty() ? "shm" : present.c_str());
        if (l.gpu && !direct_failed_ && present == "direct") {
            std::string why;
            if (!direct_.active() && !direct_.open(l.engine_dir, why))
                WNK_LOGW(kCat, "direct presentation unavailable: %s", why.c_str());
            if (direct_.active()) {
                l.surface_group = direct_.group();
                l.surface_layer = direct_.layer();
                l.wayland_socket = direct_.socket();
            }
        }
        l.data_dir = data_dir_ + "/webkit";
        l.cache_dir = l.data_dir + "/cache";
        make_dir(l.data_dir);
        make_dir(l.cache_dir);
        const wnk::Rect r = screen_->page_->screen_rect();
        l.width = static_cast<uint32_t>((r.w > 0 ? r.w : 1920) * kRenderScale);
        l.height = static_cast<uint32_t>((r.h > 0 ? r.h : 880) * kRenderScale);
        screen_->page_->set_page_size(l.width, l.height);
        std::string error;
        if (!engine_.start(l, error)) {
            WNK_LOGE(kCat, "engine: %s", error.c_str());
            show_message("The web engine could not start: " + error);
        }
    }

    // The app's own folder inside the first writable persistent location the kit knows. The kit's candidates
    // are shared base folders (/media/developer, $XDG_DATA_HOME, ...), so always add a folder named for the app.
    // The whole folder must be usable by this uid: an existing one may hold another user's files (root test
    // runs on the TV left a root-owned cookies.sqlite, and libsoup then failed with "readonly database").
    // own_usable_dir() switches to "<folder>-<uid>" then, leaving the other user's files alone.
    std::string data_dir(wnk::app::Context &ctx) {
        const std::string own = "/" + ctx.launch().app_id + ".data";
        for (const auto &c :
             wnk::platform::storage_candidates(ctx.launch().app_id, ctx.launch().app_dir, ctx.runtime_dir())) {
            if (c.purpose != "data" || !wnk::platform::probe_writable(c).writable) continue;
            std::string why;
            const std::string dir = browser::own_usable_dir(c.directory + own, &why);
            if (!why.empty()) WNK_LOGW(kCat, "profile folder %s", why.c_str());
            if (!dir.empty()) return dir;
        }
        const std::string fallback = (ctx.runtime_dir().empty() ? std::string("/tmp") : ctx.runtime_dir()) + own;
        std::string why;
        const std::string dir = browser::own_usable_dir(fallback, &why);
        WNK_LOGE(kCat, "no persistent profile folder; using %s%s%s", dir.empty() ? fallback.c_str() : dir.c_str(),
            why.empty() ? "" : " (", why.empty() ? "" : (why + ")").c_str());
        return dir.empty() ? fallback : dir;
    }

    void show_message(const std::string &text) {
        screen_->message_->set_text(text);
        screen_->message_->set_visible(true);
    }

    void leave_page() {
        if (!pointer_in_page_) return;
        pointer_in_page_ = false;
        engine_.pointer(WPE_POINTER_LEAVE, 0, last_x_, last_y_, 0);
    }

    browser::EngineClient engine_;
    browser::Profile profile_;
    wnk::app::Context *ctx_ = nullptr;
    wnk::ui::Navigator *nav_ = nullptr;
    BrowserScreen *screen_ = nullptr;
    wnk::input::KeyMap keymap_;
    std::string data_dir_;
    std::vector<Tab> tabs_;
    TabId active_ = 0;
    TabId next_id_ = 1;
    std::string engine_version_;
    bool pointer_in_page_ = false;
    bool focused_once_ = false;
    int32_t last_x_ = 0, last_y_ = 0;
    int restarts_ = 0;
    bool restart_pending_ = false;
    double restart_at_ms_ = 0;
    float chrome_shown_ = 1.0f;  // 0 hidden .. 1 shown (slide position)
    float chrome_h_ = 0;         // height of the chrome's rows
    double last_chrome_ms_ = 0;
    double peek_until_ms_ = 0;
    browser::DirectOwner direct_;
    bool direct_failed_ = false;           // stays in SHM mode for the rest of this run
    bool direct_fallback_pending_ = false;
};

BrowserScreen::BrowserScreen(App &app, browser::EngineClient &engine) : Screen("browser"), app_(app) {
    name = "browser";
    // The page fills the screen (with direct presentation the TV shows the page surface at full window size
    // anyway); the chrome lies over its top and slides away while the page has focus (App::update_chrome).
    auto &stack = add<wnk::ui::View>();
    stack.set_width(wnk::ui::Dim::fill()).set_height(wnk::ui::Dim::fill());
    auto &content = stack.add<wnk::ui::View>();
    content.set_width(wnk::ui::Dim::fill()).set_height(wnk::ui::Dim::fill());
    page_ = &content.add<browser::PageView>(engine);
    page_->set_width(wnk::ui::Dim::fill()).set_height(wnk::ui::Dim::fill());
    page_->set_focus_ring(false);
    ntp_ = &content.add<browser::NewTabPage>(
        kShortcuts, [&app](wnk::ui::Ui &, const std::string &url) { app.navigate(url); });
    ntp_->set_width(wnk::ui::Dim::fill()).set_height(wnk::ui::Dim::fill());
    thin_progress_ = &stack.add<browser::ProgressLine>();
    thin_progress_->set_width(wnk::ui::Dim::fill());
    thin_progress_->set_visible(false);

    auto &col = stack.add<wnk::ui::Column>(0);
    col.set_width(wnk::ui::Dim::fill());
    chrome_ = &col;

    strip_ = &col.add<browser::TabStrip>(browser::TabStrip::Handlers{
        [&app](uint32_t id) { app.activate(id); },
        [&app](uint32_t id) { app.close_tab(id); },
        [&app]() { app.new_tab(); },
    });

    auto &bar = col.add<wnk::ui::Row>(8);
    toolbar_ = &bar;
    bar.set_padding({16, 10, 16, 10});
    bar.set_width(wnk::ui::Dim::fill());
    bar.set_background(pal::kToolbar);
    back_ = &bar.add<browser::IconButton>("back", "Back", [&app](wnk::ui::Ui &) { app.go_back(); });
    forward_ = &bar.add<browser::IconButton>("forward", "Forward", [&app](wnk::ui::Ui &) { app.go_forward(); });
    reload_ = &bar.add<browser::IconButton>("reload", "Reload", [&app](wnk::ui::Ui &) { app.reload_or_stop(); });
    bar.add<browser::IconButton>("home", "Home", [&app](wnk::ui::Ui &) { app.go_home(); });
    omnibox_ = &bar.add<browser::Omnibox>([&app](wnk::ui::Ui &, const std::string &text) { app.navigate(text); },
                                          "Search or type a URL", 64, pal::kOmnibox, pal::kOmniboxFocused,
                                          [&app](wnk::ui::Ui &) { app.toggle_bookmark(); });
    omnibox_->set_width(wnk::ui::Dim::fill());
    omnibox_->set_align(wnk::ui::Align::Start, wnk::ui::Align::Center);
    menu_ = &bar.add<browser::IconButton>("menu", "Menu", [&app](wnk::ui::Ui &) { app.show_menu(); });

    bookmarks_ = &col.add<browser::BookmarksBar>(
        [&app](wnk::ui::Ui &, const std::string &url) { app.navigate(url); },
        [&app](wnk::ui::Ui &ui) { app.show_bookmarks_page(ui); });

    progress_ = &col.add<browser::ProgressLine>();

    message_ = &col.add<wnk::ui::Label>("");
    message_->set_padding(wnk::ui::Insets::all(32));
    message_->set_width(wnk::ui::Dim::fill());
    message_->set_background(pal::kFrame);
    message_->set_visible(false);
}

bool BrowserScreen::on_back(wnk::ui::Ui &) {
    if (app_.content_focused()) return false;  // the shell's double-Back exit
    app_.focus_content();
    return true;
}

}  // namespace

int main(int argc, char **argv) {
    wnk::app::AppConfig config;
    config.fallback_app_id = "org.sostk.wpebrowser";
    config.title = "WPE Browser";
    config.script_env_prefix = "APP_WPEBROWSER";
    config.double_back_exit = true;
    // Chromium dark: kit widgets (TextField, forms, dialogs, toasts) pick these up.
    config.theme.background = pal::kFrame;
    config.theme.surface = pal::kToolbar;
    // Form rows (Settings) sit on cards: same colour at rest, a lighter hover when focused.
    config.theme.raised = pal::kCard;
    config.theme.raised_focused = pal::kHover;
    config.theme.text = pal::kText;
    config.theme.text_muted = pal::kMuted;
    config.theme.accent = pal::kAccent;
    config.theme.focus_ring = pal::kAccent;
    config.theme.placeholder = pal::kHover;
    config.theme.ring_width = 3;
    config.theme.ring_gap = 3;
    config.theme.text_size = 28;
    return wnk::app::run(argc, argv, config, [] { return std::make_unique<App>(); });
}
