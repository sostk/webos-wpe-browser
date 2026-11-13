// SPDX-License-Identifier: GPL-3.0-or-later
// Wren: a WebKit (WPE) web browser for LG webOS TVs, with tabs, bookmarks, history and settings, in a glass,
// D-pad-first TV interface (wren_ui.hpp).
//
// This process is the UI: the kit's shell, the Home page (New Tab) or the PageView, and over a page a glass panel
// with the tab row and the navigation row; overlays for all tabs, the side panel (bookmarks, history, zoom) and
// quick actions; Settings, History and Bookmarks are native pages pushed on top (screens.cpp). WebKit runs in
// engine/wpe-engine (built with a glibc 2.28 toolchain), which keeps one WebKit view per open tab; see
// engine_client.hpp and common/wpe_ipc.h. Settings, bookmarks, history and the session live in profile.cpp.
//
// Remote (any LG remote, colour buttons not needed): in a page the D-pad moves between links (WebKit spatial
// navigation) and OK follows them; Up at the page's top edge shows the panel, where Left/Right + OK switch tabs.
// Back goes back in history, or to the panel when there is none. Hold OK = quick actions. CH+/CH- = next/previous
// tab. Colour keys, when the remote has them: Red = all tabs, Green = bookmarks, Yellow = quick actions (zoom),
// Blue = new tab. The Magic Remote pointer is a mouse in pages and its wheel scrolls.
#include "chrome.hpp"
#include "wren_ui.hpp"
#include "direct_owner.hpp"
#include "engine_client.hpp"
#include "media_server.hpp"
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
#include <utility>
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
constexpr const char *kVersion = "0.3.0";
// Pages are rendered at this fraction of the view's size in 1920x1080 logical units and scaled up by the
// GPU: CPU rendering at about 1280 px wide keeps scrolling usable, and CSS px at that size read well from a sofa.
constexpr float kRenderScale = 2.0f / 3.0f;
constexpr double kChromeSlideMs = 160;     // toolbar slide in/out
constexpr double kChromePeekMs = 2500;   // after Back the toolbar stays over the page this long
constexpr float kChromeRevealEdge = 6.0f;  // logical px at the top of the screen that bring the toolbar back
constexpr int kMaxRestarts = 3;
constexpr double kHoldOkMs = 550;  // OK held this long opens quick actions
// Background tabs beyond this many keep only their URL and title (Chromium's tab discarding); they reload
// when shown again. Each live tab costs WebKit memory, and a TV has little to spare.
constexpr size_t kMaxLiveTabs = 4;
constexpr size_t kMaxTabs = 12;

const std::vector<browser::wren::HomeData::Item> kShortcuts = {
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
    bool fullscreen = false;  // the page is in fullscreen (a video's fullscreen button): no chrome at all
    TabId opener = 0;    // the tab whose page opened this one (window.open), shown again when it closes itself
    double last_shown_ms = 0;
    bool crashed = false;      // its web process died; reloaded when next shown
    double crashed_ms = -1e9;  // when (a second crash soon after an automatic reload is left to the user)
};

class App;

// The whole window: the content (Home page or PageView), the glass panel over a page (tab row, navigation row),
// a hint pill, and the overlays.
class BrowserScreen : public wnk::ui::Screen {
 public:
    BrowserScreen(App &app, browser::EngineClient &engine);
    // Back closes an overlay, or in the chrome returns to the content, instead of the shell's double-Back exit.
    bool on_back(wnk::ui::Ui &ui) override;

    App &app_;
    browser::wren::TabRow *tabrow_ = nullptr;
    browser::IconButton *back_ = nullptr, *forward_ = nullptr, *reload_ = nullptr, *star_ = nullptr, *menu_ = nullptr;
    browser::Omnibox *omnibox_ = nullptr;
    browser::wren::ChromePanel *panel_ = nullptr;
    wnk::ui::Column *chrome_ = nullptr;             // the panel (and messages): drawn over the page
    wnk::ui::Row *toolbar_ = nullptr;               // the navigation row
    browser::ProgressLine *thin_progress_ = nullptr;  // load progress while the chrome is hidden
    browser::PageView *page_ = nullptr;
    browser::wren::HomePage *home_ = nullptr;
    browser::wren::HintPill *hints_ = nullptr;      // over a page while the panel has focus
    wnk::ui::View *content_ = nullptr;              // the page or the Home page
    browser::wren::TabOverview *overview_ = nullptr;
    browser::wren::SidePanel *side_ = nullptr;
    browser::wren::QuickActions *quick_ = nullptr;
    wnk::ui::Label *message_ = nullptr;

    bool overlay_open() const { return open_overlay() != nullptr; }
    browser::wren::Overlay *open_overlay() const {
        if (quick_->visible()) return quick_;
        if (side_->visible()) return side_;
        if (overview_->visible()) return overview_;
        return nullptr;
    }
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

    // A page opened a window (window.open, target=_blank, sign-in popups): the engine already has its view, which
    // WebKit is loading. It goes right after its opener and is shown, beyond the tab limit if need be (a sign-in
    // must not fail for it).
    void add_page_tab(TabId id, TabId opener) {
        if (tab(id) != nullptr) return;
        Tab t;
        t.id = id;
        t.live = true;
        t.loading = true;
        t.opener = opener;
        auto at = find(opener);
        tabs_.insert(at == tabs_.end() ? tabs_.end() : at + 1, t);
        activate(id);
    }

    // window.close(): close it and show the page that opened it.
    // window.close(): show the opener now and destroy the page's view a little later. Destroying it straight after
    // the switch made webOS minimise the app (TV, 2026-11-11).
    void close_page_tab(TabId id) {
        Tab *t = tab(id);
        if (t == nullptr) return;
        WNK_LOGI(kCat, "tab %u closed itself (window.close)", id);
        if (id == active_ && t->opener != 0 && tab(t->opener) != nullptr) activate(t->opener);
        pending_closes_.push_back({id, ctx_->ui().now_ms() + 1500});
    }

    // `then`: the tab to show if this one was shown (its neighbour otherwise). The next tab is shown before the closed
    // one's view is destroyed: webOS takes a page window vanishing with none to replace it as the app's window
    // closing and minimises the app (TV, 2026-11-11, closing a window.open popup).
    void close_tab(TabId id, TabId then = 0) {
        auto it = find(id);
        if (it == tabs_.end()) return;
        const size_t index = static_cast<size_t>(it - tabs_.begin());
        const bool live = it->live;
        if (id == active_ && tabs_.size() > 1) {
            TabId next = then != 0 && then != id && tab(then) != nullptr ? then : 0;
            if (next == 0) next = tabs_[index + 1 < tabs_.size() ? index + 1 : index - 1].id;
            activate(next);
        }
        if (live) engine_.tab_close(id);
        tabs_.erase(find(id));
        if (tabs_.empty()) {
            active_ = 0;
            new_tab();  // a TV browser never closes its window: start over with a New Tab page
            return;
        }
        refresh_chrome();
    }

    void activate(TabId id) {
        Tab *t = tab(id);
        if (t == nullptr) return;
        const bool changed = id != active_;
        active_ = id;
        t->last_shown_ms = ctx_->ui().now_ms();
        if (!t->url.empty() && !t->live) open_in_engine(*t, t->url);
        if (t->live) engine_.tab_activate(id);
        if (t->live && t->crashed) reload_crashed(*t);
        screen_->page_->show_tab(t->live ? id : 0);
        screen_->page_->ime(ctx_->ui(), false);
        if (changed && t->url.empty()) screen_->home_->search().field().set_text("");  // a fresh Home page
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
    // A tab whose web process died: load its page again in a new one, unless it died again within
    // kCrashLoopMs of the last automatic reload (then the user decides, with Green).
    void reload_crashed(Tab &t) {
        t.crashed = false;
        WNK_LOGI(kCat, "tab %u: reloading after its web process crashed", t.id);
        engine_.reload(t.id);
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

    // The menu button and Green: the side panel (bookmarks, history, zoom, settings, exit).
    void show_menu(browser::wren::SidePanel::Section section = browser::wren::SidePanel::Section::Saved) {
        close_overlays();
        cover(true);
        screen_->side_->show(ctx_->ui(), section, profile_.bookmarks(), profile_.history());
    }

    // Red and "All tabs": every tab as a card.
    void show_tab_overview() {
        close_overlays();
        cover(true);
        screen_->overview_->set_visible(true);
        rebuild_overview();
    }

    // Hold OK and Yellow: the common actions and page zoom.
    void show_quick_actions(bool focus_zoom = false) {
        close_overlays();
        const Tab *t = tab(active_);
        const std::string url = t != nullptr ? t->url : std::string();
        cover(true);
        screen_->quick_->show(ctx_->ui(), t != nullptr ? t->title : std::string(), url,
                              !url.empty() && profile_.is_bookmarked(url), focus_zoom);
    }

    // What lies under an overlay takes no focus while it is open: the Magic Remote pointer's hover focus would
    // otherwise land on the page (or Home's tiles) under the overlay's scrim and take the remote with it.
    void cover(bool on) {
        screen_->content_->set_enabled(!on);
        screen_->chrome_->set_enabled(!on);
    }

    void close_overlays() {
        auto &ui = ctx_->ui();
        for (browser::wren::Overlay *o : {static_cast<browser::wren::Overlay *>(screen_->overview_),
                                          static_cast<browser::wren::Overlay *>(screen_->side_),
                                          static_cast<browser::wren::Overlay *>(screen_->quick_)})
            if (o->visible()) o->set_visible(false);
        cover(false);
        (void)ui;
    }

    void rebuild_overview() {
        if (screen_->overview_->visible()) screen_->overview_->rebuild(ctx_->ui(), tab_infos(), active_);
    }

    void zoom_step(int delta) {
        auto &z = profile_.settings.zoom;
        z = std::clamp(z + delta, 0, static_cast<int>(browser::kZoomLevels.size()) - 1);
        profile_.settings_changed();
        apply_settings();
    }
    std::string zoom_text() const {
        return std::to_string(browser::kZoomLevels[static_cast<size_t>(profile_.settings.zoom)] / 10) + "%";
    }

    // Up at the page's top edge (and Back with no history): the panel, on the shown tab's chip.
    void focus_tabs(wnk::ui::Ui &ui) {
        screen_->chrome_->set_visible(true);
        if (auto *chip = screen_->tabrow_->chip(active_))
            ui.focus(chip);
        else
            ui.focus(&omnibox_field());
    }

    void quit() { ctx_->quit("menu exit"); }

    void show_bookmarks_page(wnk::ui::Ui &ui) { push_page(ui, browser::make_bookmarks_screen(profile_, hooks())); }
    void show_history_page(wnk::ui::Ui &ui) { push_page(ui, browser::make_history_screen(profile_, hooks())); }
    void show_settings_page(wnk::ui::Ui &ui) { push_page(ui, browser::make_settings_screen(profile_, hooks())); }
    void close_active_tab() { close_tab(active_); }
    // "Close all": every tab goes, and a New Tab page is left (a TV browser never closes its window).
    void close_all_tabs() {
        close_overlays();
        std::vector<TabId> ids;
        for (const Tab &t : tabs_) ids.push_back(t.id);
        for (TabId id : ids) close_tab(id);
    }

    // The chrome is shown unless the page itself has focus (Red, or Back with no history, moves focus to it; so
    // does the pointer at the top edge). While the page has focus the chrome still peeks over it when the page is
    // scrolled to its top and for kChromePeekMs after Back (focus stays on the page). It slides up out of view in
    // kChromeSlideMs and is then hidden, so its buttons can't take focus.
    bool chrome_wanted(wnk::ui::Ui &ui) {
        const Tab *t = tab(active_);
        if (screen_->overlay_open()) return false;  // overlays cover the page; the panel waits behind
        if (t != nullptr && t->url.empty()) return false;  // Home has its own header
        const bool page_mode = ui.focused() == screen_->page_ && t != nullptr && t->live && !t->url.empty() &&
                               !screen_->message_->visible() && ui.top_dialog() == nullptr && nav_->depth() == 1;
        if (!page_mode) return true;
        if (t->fullscreen) return false;
        // With a hardware video the page goes above the UI while the chrome is hidden (DirectOwner::set_page_z), so
        // being at the top no longer keeps the chrome: it would hide the page's video controls.
        return (t->at_top && !(raise_page_ && media_.video_active())) || ui.now_ms() < peek_until_ms_;
    }

    void peek_chrome() { peek_until_ms_ = ctx_->ui().now_ms() + kChromePeekMs; }

    void update_chrome(wnk::ui::Ui &ui) {
        auto *chrome = screen_->chrome_;
        // The column itself spans the screen (a plain View places it with the full height); the chrome ends at the
        // bottom of its last visible row.
        const float bottom = screen_->panel_->frame().bottom();
        const float h = std::max(0.0f, bottom - chrome->frame().y);
        chrome_h_ = h;
        const bool want = chrome_wanted(ui);
        const double now = ui.now_ms();
        const double dt = last_chrome_ms_ > 0 ? now - last_chrome_ms_ : 0;
        last_chrome_ms_ = now;
        const float step = static_cast<float>(dt / kChromeSlideMs);
        chrome_shown_ = want ? std::min(1.0f, chrome_shown_ + step) : std::max(0.0f, chrome_shown_ - step);
        if (want && !chrome->visible()) chrome->set_visible(true);
        chrome->set_translation({0, -(1.0f - chrome_shown_) * h});
        if (!want && chrome_shown_ <= 0.0f && chrome->visible()) chrome->set_visible(false);
        const Tab *t = tab(active_);
        screen_->thin_progress_->set_visible(!chrome->visible() && t != nullptr && !t->url.empty());
        // The hint pill while the panel has focus (not while it only peeks over the focused page).
        const bool hints = chrome->visible() && chrome_shown_ >= 1.0f && ui.focused() != nullptr &&
                           screen_->chrome_->is_ancestor_of(ui.focused());
        if (hints != screen_->hints_->visible()) screen_->hints_->set_visible(hints);
        screen_->home_->show_hints(!screen_->overlay_open());
    }

    // Hidden views can't take focus: show the chrome first (it slides in from its current position). On the Home
    // page the address is its search field.
    void focus_omnibox(wnk::ui::Ui &ui) {
        const Tab *t = tab(active_);
        if (t != nullptr && t->url.empty()) {
            ui.focus(&screen_->home_->search().field());
            return;
        }
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
        return f == screen_->page_ || (f != nullptr && screen_->home_->is_ancestor_of(f));
    }

    void focus_content() {
        const Tab *t = tab(active_);
        if (t != nullptr && t->url.empty())
            ctx_->ui().focus(&screen_->home_->search().field());
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
        browser::wren::fonts().regular = ui.theme().regular;
        browser::wren::fonts().semibold = ui.theme().bold;
        browser::wren::load_extra_fonts(ctx.renderer(), ctx.launch().app_dir);
        browser::wren::icon_lookup() = [this](const std::string &url) { return profile_.icon_for(url); };
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
        // {"wpebShow": "quick" | "side" | "zoom" | "tabs" | "panel" | "home"}: opens that overlay (or the tab panel over the page) a
        // moment after start, for screenshots and tests without a remote.
        if (const std::string show = launch_param(params_json, "wpebShow"); !show.empty()) {
            pending_show_ = show;
            pending_show_at_ms_ = ctx_->ui().now_ms() + 4000;
        }
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
        for (size_t i = 0; i < pending_closes_.size();) {
            if (ctx.ui().now_ms() < pending_closes_[i].second) { ++i; continue; }
            const TabId id = pending_closes_[i].first;
            pending_closes_.erase(pending_closes_.begin() + static_cast<long>(i));
            WNK_LOGI(kCat, "tab %u: closing its view", id);
            close_tab(id);
        }
        direct_.dispatch();
        // Experimental ({"wpebRaisePage":"1"}): the page above the UI while hardware video plays shows the page's
        // video controls, but the compositor then gives the page surface keyboard focus (remote keys lost).
        if (direct_.active() && raise_page_)
            direct_.set_page_z(media_.video_active() && !screen_->chrome_->visible() ? 1 : -1);
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
            screen_->panel_->set_progress(t->loading, t->progress);
            screen_->thin_progress_->set(t->loading, t->progress);
        }
        if (!pending_show_.empty() && ctx.ui().now_ms() >= pending_show_at_ms_) {
            const std::string show = std::exchange(pending_show_, std::string());
            if (show == "quick") show_quick_actions();
            if (show == "side") show_menu();
            if (show == "tabs") show_tab_overview();
            if (show == "panel") focus_tabs(ctx.ui());
            if (show == "home") {
                close_overlays();
                new_tab();
            }
            if (show == "zoom") show_menu(browser::wren::SidePanel::Section::Zoom);
        }
        keep_overlay_focus(ctx.ui());
        if (ok_down_ && !ok_long_ && ctx.ui().now_ms() - ok_down_ms_ >= kHoldOkMs) {
            ok_long_ = true;
            show_quick_actions();
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
        const bool browsing = nav_->depth() == 1 && ui.text_input_view() == nullptr && ui.top_dialog() == nullptr;
        // An open overlay owns the remote: Back closes it, and every other key goes to its widgets (focus is put back
        // inside first if it slipped out, e.g. to the page under it).
        if (browsing && screen_->overlay_open()) {
            browser::wren::Overlay *o = screen_->open_overlay();
            if (action == A::Back) {
                if (e.down && !e.repeat) o->close(ui);
                return true;
            }
            keep_overlay_focus(ui);
            if (action == A::ChannelUp || action == A::ChannelDown) {
                if (e.down && !e.repeat && o == screen_->overview_) step_tab(action == A::ChannelUp ? -1 : +1);
                return true;
            }
            return false;  // the kit routes it to the focused overlay widget
        }
        // Hold OK: quick actions. OK is held back until it is released (a short press is then delivered as usual) or
        // has been held kHoldOkMs (on_frame opens the actions; the release is swallowed).
        if (action == A::Confirm) {
            const bool eligible = browsing && !screen_->overlay_open() &&
                                  !(ui.focused() == screen_->page_ && screen_->page_->editing());
            if (e.down && !e.repeat && eligible) {
                ok_down_ = true;
                ok_long_ = false;
                ok_down_ms_ = ui.now_ms();
                ok_key_ = e;
                return true;
            }
            if (ok_down_) {
                if (e.down) return true;  // auto-repeat while held
                ok_down_ = false;
                if (!ok_long_) deliver_ok(ui, e);
                return true;
            }
            return false;
        }
        if (browsing && e.down && !e.repeat) {
            switch (action) {
            case A::Red:
            case A::Green:
            case A::Yellow:
            case A::Blue:
                if (!browser::wren::colour_keys_seen()) {
                    browser::wren::colour_keys_seen() = true;
                    screen_->home_->refresh_hints();
                }
                if (action == A::Red) show_tab_overview();
                if (action == A::Green) show_menu();
                if (action == A::Yellow) show_quick_actions(true);
                if (action == A::Blue) new_tab();
                return true;
            case A::ChannelUp:
            case A::ChannelDown:
                if (screen_->overlay_open() && !screen_->overview_->visible()) return true;
                step_tab(action == A::ChannelUp ? -1 : +1);
                return true;
            default: break;
            }
        }
        if (browsing && (action == A::ChannelUp || action == A::ChannelDown || action == A::Red || action == A::Green ||
                         action == A::Yellow || action == A::Blue))
            return true;  // handled on press
        // The page lies under the chrome, so spatial navigation never finds it "below": Down from the navigation row
        // goes back to the page, which hides the chrome again.
        if (action == A::Down && e.down && browsing && ui.focused() != nullptr && screen_->toolbar_->is_ancestor_of(ui.focused())) {
            focus_content();
            return true;
        }
        const Tab *t = tab(active_);
        if (ui.focused() != screen_->page_ || screen_->page_->editing() || t == nullptr || !t->live || !engine_.running())
            return false;
        switch (action) {
        case A::Back:
            if (e.down && !e.repeat) {
                if (t->fullscreen) {
                    engine_.exit_fullscreen(t->id);  // as Escape on a desktop browser
                } else if (t->can_back) {
                    engine_.go_back(t->id);
                    peek_chrome();  // show where we are (the address) while the page stays focused
                } else
                    focus_tabs(ui);
            }
            return true;
        case A::Exit:
        case A::Home:
            return false;  // the shell and the TV handle these
        default: break;
        }
        const browser::WebKey k = browser::web_key_from_sdl(e.keycode);
        if (k.keyval == 0) return false;
        engine_.key(e.down, k.keyval, k.keycode, 0);
        return true;
    }

    // A short OK press, held back by on_key: to the page as Return, else to the focused widget.
    void deliver_ok(wnk::ui::Ui &ui, const wnk::input::KeyEvent &release) {
        const Tab *t = tab(active_);
        if (ui.focused() == screen_->page_ && t != nullptr && t->live && engine_.running()) {
            const browser::WebKey k = browser::web_key_from_sdl(ok_key_.keycode);
            if (k.keyval != 0) {
                engine_.key(true, k.keyval, k.keycode, 0);
                engine_.key(false, k.keyval, k.keycode, 0);
            }
            return;
        }
        wnk::input::ActionEvent a;
        a.action = wnk::input::Action::Confirm;
        a.phase = wnk::input::Phase::Press;
        a.source = ok_key_;
        ui.handle(a);
        a.phase = wnk::input::Phase::Release;
        a.held_ms = release.time_ms - ok_key_.time_ms;
        a.source = release;
        ui.handle(a);
    }

    // Focus belongs inside an open overlay. Its widgets were hidden until the overlay opened, and the kit may not take
    // a focus request for them in that frame; the page under it must never keep the remote.
    void keep_overlay_focus(wnk::ui::Ui &ui) {
        browser::wren::Overlay *o = screen_->open_overlay();
        if (o == nullptr) return;
        wnk::ui::View *f = ui.focused();
        if (f != nullptr && o->is_ancestor_of(f)) return;
        if (o == screen_->quick_ || o == screen_->side_ || o == screen_->overview_) ui.focus_within(o);
    }

    // CH+ / CH-: the previous / next tab.
    void step_tab(int delta) {
        if (tabs_.size() < 2) return;
        auto it = find(active_);
        const long i = it == tabs_.end() ? 0 : static_cast<long>(it - tabs_.begin());
        const long n = static_cast<long>(tabs_.size());
        activate(tabs_[static_cast<size_t>(((i + delta) % n + n) % n)].id);
        if (!tabs_.empty()) ctx_->ui().toast(tab(active_)->title.empty() ? "New tab" : tab(active_)->title, 1200);
    }

    bool on_pointer(wnk::app::Context &ctx, const wnk::input::PointerEvent &p) override {
        using T = wnk::input::PointerEvent::Type;
        const Tab *t = tab(active_);
        if (!engine_.running() || t == nullptr || !t->live || !screen_->page_->visible() || nav_->depth() != 1) return false;
        if (screen_->overlay_open() || ok_down_) {
            leave_page();
            return ok_down_ && (p.type == T::Press || p.type == T::Release);  // OK's own click, held back with the key
        }
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
            browser::MediaServer::set_foreground(false);
            engine_.set_visible(false);
            save_session();
            profile_.flush_now();  // the TV may close a backgrounded app without warning
        }
        if (t.to == S::Running) {
            browser::MediaServer::set_foreground(true);
            engine_.set_visible(true);
        }
    }

    void on_stop(wnk::app::Context &) override {
        save_session();
        profile_.flush_now();
        engine_.stop();
        media_.stop();  // after the engine: its web processes held the streams
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
        engine_.set_zoom(static_cast<uint32_t>(browser::kZoomLevels[static_cast<size_t>(profile_.settings.zoom)]));
        // Pages lay out at 2/3 of the screen (kRenderScale); 1/kRenderScale renders them at full resolution.
        // Direct presentation always does: the page surface is shown 1:1, not scaled to the page area.
        const bool full = profile_.settings.sharp_text || screen_->page_->direct();
        engine_.set_scale(full ? static_cast<uint32_t>(1000.0f / kRenderScale + 0.5f) : 1000u);
    }

    void refresh_bookmarks() {}  // bookmarks show in the side panel, filled when it opens

    std::vector<browser::TabInfo> tab_infos() const {
        std::vector<browser::TabInfo> infos;
        for (const Tab &t : tabs_) infos.push_back({t.id, t.title, t.url, t.loading});
        return infos;
    }

    // The Home page's "Continue browsing": the latest distinct pages from history.
    std::vector<browser::wren::HomeData::Item> recents() const {
        std::vector<browser::wren::HomeData::Item> out;
        for (const auto &h : profile_.history()) {
            if (h.url.empty()) continue;
            if (std::any_of(out.begin(), out.end(), [&](const auto &o) { return o.url == h.url; })) continue;
            out.push_back({h.title, h.url});
            if (out.size() == 4) break;
        }
        return out;
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
        const std::vector<browser::TabInfo> infos = tab_infos();
        auto &ui = ctx_->ui();
        const bool row_focused = ui.focused() != nullptr && screen_->tabrow_->is_ancestor_of(ui.focused());
        screen_->tabrow_->rebuild(ui, infos, active_);
        if (row_focused) {
            if (auto *chip = screen_->tabrow_->chip(active_)) ui.focus(chip);
        }
        const bool home_focused = ui.focused() != nullptr && screen_->home_->is_ancestor_of(ui.focused()) &&
                                  ui.focused() != &screen_->home_->search().field();
        screen_->home_->set_tabs(infos, active_);
        const Tab *t = tab(active_);
        const bool page = t != nullptr && !t->url.empty();
        if (!page) screen_->home_->set_recents(recents());
        if (home_focused && !page) ui.focus(&screen_->home_->search().field());
        screen_->home_->set_visible(!page);
        screen_->page_->set_visible(page);
        screen_->back_->set_active(t != nullptr && t->can_back);
        screen_->forward_->set_active(t != nullptr && t->can_forward);
        screen_->reload_->set_icon(t != nullptr && t->loading ? "close" : "reload");
        screen_->reload_->set_active(page);
        const bool marked = page && profile_.is_bookmarked(t->url);
        screen_->star_->set_icon(marked ? "star-on" : "star");
        screen_->star_->accessible_label = marked ? "Remove bookmark" : "Bookmark this page";
        screen_->star_->set_active(page);
        if (ui.focused() != &omnibox_field()) screen_->omnibox_->show_url(t != nullptr ? t->url : std::string());
        if (browser::wren::colour_keys_seen())
            screen_->hints_->set_hints({{"◀ ▶", {}, "Switch tab"}, {"OK", {}, "Open"}, {"▼", {}, "Page"},
                                        {"", browser::wren::kYellow, "Zoom"}});
        else
            screen_->hints_->set_hints({{"◀ ▶", {}, "Switch tab"}, {"OK", {}, "Open"}, {"▼", {}, "Page"},
                                        {"Hold OK", {}, "More"}});
        rebuild_overview();
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
        cb.tab_created = [this](TabId id, TabId opener) { add_page_tab(id, opener); };
        cb.tab_close = [this](TabId id) { close_page_tab(id); };
        cb.fullscreen = [this](TabId id, bool on) {
            Tab *t = tab(id);
            if (t == nullptr) return;
            t->fullscreen = on;
            WNK_LOGI(kCat, "tab %u %s fullscreen", id, on ? "entered" : "left");
            // The page takes the remote while it is fullscreen (its video controls), and gets it back after.
            if (on && id == active_) focus_content();
        };
        cb.scroll = [this](TabId id, bool at_top) {
            if (Tab *t = tab(id)) t->at_top = at_top;
        };
        cb.icon = [this](TabId id, const std::string &url) {
            Tab *t = tab(id);
            if (t == nullptr || t->url.empty()) return;
            WNK_LOGI(kCat, "tab %u icon %s", id, url.empty() ? "(none declared)" : url.c_str());
            profile_.set_icon(t->url, url);
            refresh_chrome();
        };
        cb.edge_up = [this](TabId id) {
            auto &ui = ctx_->ui();
            if (id == active_ && ui.focused() == screen_->page_ && !screen_->overlay_open() && nav_->depth() == 1)
                focus_tabs(ui);
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
            if (kind != WPE_ERROR_WEB_PROCESS_CRASHED) return;
            const double now = ctx_->ui().now_ms();
            if (direct_.active() && now - crash_at_ms_ > 1000) {
                // The compositor can keep the dead process's surface on the page layer, and the next page's
                // attach is then refused ("Layer already attached", a fatal Wayland error; TV, 2026-11-11).
                if (direct_.renew_layer()) engine_.set_layer(direct_.layer());
            }
            crash_at_ms_ = now;
            Tab *t = tab(id);
            if (t == nullptr) return;
            t->fullscreen = false;
            const bool looping = now - t->crashed_ms < kCrashLoopMs;
            t->crashed_ms = now;
            t->crashed = !looping;
            if (looping) {
                if (id == active_) ctx_->ui().toast("This page stopped again (" + msg + "). Press Green to reload.", 6000);
            } else if (id == active_) {
                reload_crashed(*t);
            }
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
        // On the TV pages are drawn by the GPU straight to the screen by default (direct presentation, tested
        // 2026-11-11; a failure falls back to SHM frames by itself). Launch parameters {"wpebRenderer":"software"|"gpu",
        // "wpebPresent":"shm"|"direct"} or WPE_BROWSER_RENDERER / WPE_BROWSER_PRESENT choose otherwise (a parameter
        // wins; an installed app gets no environment from the launcher). Host builds keep software + SHM.
#if defined(WNK_SDL_WEBOS)
        constexpr const char *kDefaultRenderer = "gpu", *kDefaultPresent = "direct";
#else
        constexpr const char *kDefaultRenderer = "software", *kDefaultPresent = "shm";
#endif
        const std::string &params = ctx.launch().params_json;
        std::string renderer = launch_param(params, "wpebRenderer");
        if (renderer.empty() && std::getenv("WPE_BROWSER_RENDERER")) renderer = std::getenv("WPE_BROWSER_RENDERER");
        if (renderer.empty()) renderer = kDefaultRenderer;
        l.gpu = renderer == "gpu";
        // Direct presentation (with gpu): pages draw into a surface below this window (docs/webkit-like-chromium.md).
        std::string present = launch_param(params, "wpebPresent");
        if (present.empty() && std::getenv("WPE_BROWSER_PRESENT")) present = std::getenv("WPE_BROWSER_PRESENT");
        if (present.empty()) present = kDefaultPresent;
        WNK_LOGI(kCat, "renderer %s, presentation %s", l.gpu ? "gpu" : "software", l.gpu ? present.c_str() : "shm");
        if (l.gpu && !direct_failed_ && present == "direct") {
            std::string why;
            // Page layer below the UI window (z -1): the chrome is drawn over the page, which shows through where
            // the UI is transparent. {"wpebPageZ": "1"} puts it above (experiments).
            raise_page_ = launch_param(params, "wpebRaisePage") == "1";
            const std::string z = launch_param(params, "wpebPageZ");
            if (!direct_.active() && !direct_.open(l.engine_dir, why, z.empty() ? -1 : std::atoi(z.c_str())))
                WNK_LOGW(kCat, "direct presentation unavailable: %s", why.c_str());
            if (direct_.active()) {
                l.surface_group = direct_.group();
                l.surface_layer = direct_.layer();
                l.wayland_socket = direct_.socket();
                // {"wpebVideo":"software"} keeps video on the CPU (GStreamer's own decoders, drawn into the page).
                std::string why;
                if (launch_param(params, "wpebVideo") == "software")
                    WNK_LOGI(kCat, "hardware video off (launch parameter)");
                else if (media_.running() || media_.start(l.runtime_dir + "/media.sock", &why))
                    l.media_socket = media_.path();
                else
                    WNK_LOGW(kCat, "hardware video unavailable: %s", why.c_str());
            }
        }
        if (!params.empty()) {
            const auto j = nlohmann::json::parse(params, nullptr, false);
            if (j.is_object() && j.contains("wpebEnv") && j["wpebEnv"].is_object())
                for (const auto &[k, v] : j["wpebEnv"].items())
                    if (v.is_string()) l.extra_env.emplace_back(k, v.get<std::string>());
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
    static constexpr double kCrashLoopMs = 20000;
    double crash_at_ms_ = -1e9;  // the last web-process crash
    bool restart_pending_ = false;
    double restart_at_ms_ = 0;
    float chrome_shown_ = 1.0f;  // 0 hidden .. 1 shown (slide position)
    float chrome_h_ = 0;         // height of the chrome's rows
    double last_chrome_ms_ = 0;
    double peek_until_ms_ = 0;
    browser::DirectOwner direct_;
    browser::MediaServer media_;
    bool raise_page_ = false;
    std::vector<std::pair<TabId, double>> pending_closes_;  // window.close(): destroyed a little later  // hardware video for pages (direct presentation only)
    bool direct_failed_ = false;           // stays in SHM mode for the rest of this run
    bool ok_down_ = false, ok_long_ = false;  // OK held back by on_key (Hold OK)
    std::string pending_show_;  // launch parameter wpebShow
    double pending_show_at_ms_ = 0;
    double ok_down_ms_ = 0;
    wnk::input::KeyEvent ok_key_{};
    bool direct_fallback_pending_ = false;
};

BrowserScreen::BrowserScreen(App &app, browser::EngineClient &engine) : Screen("browser"), app_(app) {
    name = "browser";
    // The page fills the screen (with direct presentation the TV shows the page surface at full window size
    // anyway); the glass panel lies over its top and slides away while the page has focus (App::update_chrome).
    auto &stack = add<wnk::ui::View>();
    stack.set_width(wnk::ui::Dim::fill()).set_height(wnk::ui::Dim::fill());
    auto &content = stack.add<wnk::ui::View>();
    content.set_width(wnk::ui::Dim::fill()).set_height(wnk::ui::Dim::fill());
    content_ = &content;
    page_ = &content.add<browser::PageView>(engine);
    page_->set_width(wnk::ui::Dim::fill()).set_height(wnk::ui::Dim::fill());
    page_->set_focus_ring(false);

    browser::wren::TabHandlers tabs{
        [&app](uint32_t id) { app.close_overlays(); app.activate(id); },
        [&app](uint32_t id) { app.close_tab(id); },
        [&app]() { app.close_overlays(); app.new_tab(); },
        [&app]() { app.show_tab_overview(); },
        [&app]() { app.close_all_tabs(); },
    };
    browser::wren::HomeData home;
    home.shortcuts = kShortcuts;
    home_ = &content.add<browser::wren::HomePage>(
        home, tabs, [&app](wnk::ui::Ui &, const std::string &url) { app.navigate(url); },
        [&app](wnk::ui::Ui &) { app.show_menu(); });
    home_->set_width(wnk::ui::Dim::fill()).set_height(wnk::ui::Dim::fill());

    thin_progress_ = &stack.add<browser::ProgressLine>();
    thin_progress_->set_width(wnk::ui::Dim::fill());
    thin_progress_->set_visible(false);

    auto &col = stack.add<wnk::ui::Column>(12);
    col.set_width(wnk::ui::Dim::fill());
    col.set_padding({60, 27, 60, 0});
    chrome_ = &col;
    panel_ = &col.add<browser::wren::ChromePanel>();
    tabrow_ = &panel_->add<browser::wren::TabRow>(tabs);
    auto &line = panel_->add<wnk::ui::View>();
    line.set_width(wnk::ui::Dim::fill()).set_height(wnk::ui::Dim::fixed(1.5f));
    line.set_padding({12, 0, 12, 0});
    line.set_background(wnk::render::kWhite.with_alpha(0.08f));
    auto &bar = panel_->add<wnk::ui::Row>(6);
    toolbar_ = &bar;
    bar.set_width(wnk::ui::Dim::fill());
    back_ = &bar.add<browser::IconButton>("back", "Back", [&app](wnk::ui::Ui &) { app.go_back(); }, 72);
    forward_ = &bar.add<browser::IconButton>("forward", "Forward", [&app](wnk::ui::Ui &) { app.go_forward(); }, 72);
    reload_ = &bar.add<browser::IconButton>("reload", "Reload", [&app](wnk::ui::Ui &) { app.reload_or_stop(); }, 72);
    omnibox_ = &bar.add<browser::Omnibox>([&app](wnk::ui::Ui &, const std::string &text) { app.navigate(text); },
                                          "Search or type a URL", 72);
    omnibox_->set_width(wnk::ui::Dim::fill());
    omnibox_->set_align(wnk::ui::Align::Start, wnk::ui::Align::Center);
    star_ = &bar.add<browser::IconButton>("star", "Bookmark this page", [&app](wnk::ui::Ui &) { app.toggle_bookmark(); }, 72);
    menu_ = &bar.add<browser::IconButton>("menu", "Menu", [&app](wnk::ui::Ui &) { app.show_menu(); }, 72);


    // Engine trouble: a glass-dark box above the hint pill (also on the Home page, which has no panel).
    auto &message_box = stack.add<wnk::ui::View>();
    message_box.set_width(wnk::ui::Dim::fill()).set_height(wnk::ui::Dim::fill());
    message_box.set_padding({240, 0, 240, 135});
    message_ = &message_box.add<wnk::ui::Label>("");
    message_->set_padding(wnk::ui::Insets::xy(39, 27));
    message_->set_max_lines(3);
    message_->set_background(wnk::render::Color::hex(0x0C1018E6), 30);
    message_->set_align(wnk::ui::Align::Center, wnk::ui::Align::End);
    message_->set_visible(false);

    auto &hint_box = stack.add<wnk::ui::View>();
    hint_box.set_width(wnk::ui::Dim::fill()).set_height(wnk::ui::Dim::fill());
    hint_box.set_padding({0, 0, 0, 39});
    hints_ = &hint_box.add<browser::wren::HintPill>(browser::wren::Glass::Dark);
    hints_->set_align(wnk::ui::Align::Center, wnk::ui::Align::End);
    hints_->set_visible(false);

    overview_ = &stack.add<browser::wren::TabOverview>(tabs);
    browser::wren::PanelHandlers ph;
    ph.open = [&app](wnk::ui::Ui &, const std::string &url) { app.close_overlays(); app.navigate(url); };
    ph.history_page = [&app](wnk::ui::Ui &ui) { app.close_overlays(); app.show_history_page(ui); };
    ph.bookmarks_page = [&app](wnk::ui::Ui &ui) { app.close_overlays(); app.show_bookmarks_page(ui); };
    ph.settings = [&app](wnk::ui::Ui &ui) { app.close_overlays(); app.show_settings_page(ui); };
    ph.exit = [&app](wnk::ui::Ui &) { app.quit(); };
    ph.zoom = [&app](int d) { app.zoom_step(d); };
    ph.zoom_text = [&app]() { return app.zoom_text(); };
    side_ = &stack.add<browser::wren::SidePanel>(ph);
    browser::wren::QuickHandlers qh;
    qh.new_tab = [&app](wnk::ui::Ui &) { app.new_tab(); };
    qh.bookmark = [&app](wnk::ui::Ui &) { app.toggle_bookmark(); };
    qh.tabs = [&app](wnk::ui::Ui &) { app.show_tab_overview(); };
    qh.reload = [&app](wnk::ui::Ui &) { app.reload_or_stop(); };
    qh.close_tab = [&app](wnk::ui::Ui &) { app.close_active_tab(); };
    qh.history = [&app](wnk::ui::Ui &ui) { app.show_history_page(ui); };
    qh.home = [&app](wnk::ui::Ui &) { app.go_home(); };
    qh.settings = [&app](wnk::ui::Ui &ui) { app.show_settings_page(ui); };
    qh.zoom = [&app](int d) { app.zoom_step(d); };
    qh.zoom_text = [&app]() { return app.zoom_text(); };
    quick_ = &stack.add<browser::wren::QuickActions>(qh);
    for (browser::wren::Overlay *o : {static_cast<browser::wren::Overlay *>(overview_), static_cast<browser::wren::Overlay *>(side_),
                                      static_cast<browser::wren::Overlay *>(quick_)})
        o->on_close = [&app](wnk::ui::Ui &) {
            app.cover(false);
            app.focus_content();
        };
}

bool BrowserScreen::on_back(wnk::ui::Ui &ui) {
    for (browser::wren::Overlay *o : {static_cast<browser::wren::Overlay *>(quick_), static_cast<browser::wren::Overlay *>(side_),
                                      static_cast<browser::wren::Overlay *>(overview_)}) {
        if (o->visible()) {
            o->close(ui);
            return true;
        }
    }
    if (app_.content_focused()) return false;  // the shell's double-Back exit
    app_.focus_content();
    return true;
}

}  // namespace

int main(int argc, char **argv) {
    wnk::app::AppConfig config;
    config.fallback_app_id = "org.sostk.wpebrowser";
    config.title = "Wren";
    config.font_regular = "fonts/Manrope-Regular.ttf";
    config.font_bold = "fonts/Manrope-SemiBold.ttf";
    config.script_env_prefix = "APP_WPEBROWSER";
    config.double_back_exit = true;
    // HTTP(S) for site icons (the kit's image service: libcurl, a disk cache, PNG/JPEG/GIF/WebP/SVG decoding). Pages
    // themselves load in the engine.
    config.http = true;
    config.http_cache_bytes = 8u << 20;
    config.image_memory_bytes = 16u << 20;
    // Wren's ink and glass: kit widgets (TextField, forms, dialogs, toasts) pick these up.
    config.theme.background = pal::kFrame;
    config.theme.surface = pal::kToolbar;
    // Form rows (Settings) sit on cards: same colour at rest, a lighter hover when focused.
    config.theme.raised = pal::kCard;
    config.theme.raised_focused = pal::kHover;
    config.theme.text = pal::kText;
    config.theme.text_muted = pal::kMuted;
    config.theme.accent = pal::kAccent;
    config.theme.focus_ring = wnk::render::kWhite.with_alpha(0.92f);
    config.theme.placeholder = pal::kHover;
    config.theme.ring_width = 3;
    config.theme.ring_gap = 3;
    config.theme.radius = 24;
    config.theme.text_size = 28;
    return wnk::app::run(argc, argv, config, [] { return std::make_unique<App>(); });
}
