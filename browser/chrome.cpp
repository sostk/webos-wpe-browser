// SPDX-License-Identifier: GPL-3.0-or-later
#include "chrome.hpp"

#include "wnk/ui/ui.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>

namespace browser {

using wnk::Rect;
using wnk::Size;
using wnk::render::Color;
using wnk::render::TextStyle;
using wnk::ui::Ui;

namespace {

std::string g_icon_dir;

constexpr Color kMonogramColors[] = {
    Color::hex(0x8AB4F8FF), Color::hex(0xF28B82FF), Color::hex(0xFDD663FF), Color::hex(0x81C995FF),
    Color::hex(0xFF8BCBFF), Color::hex(0xC58AF9FF), Color::hex(0x78D9ECFF), Color::hex(0xFCAD70FF),
};
constexpr Color kMonogramText = Color::hex(0x202124FF);

bool is_confirm(const wnk::input::ActionEvent &e) {
    return e.action == wnk::input::Action::Confirm && e.phase == wnk::input::Phase::Press;
}

void draw_centered(wnk::render::Renderer &r, const TextStyle &s, const Rect &box, const std::string &text) {
    const float w = r.measure_text(s, text);
    r.draw_text(s, {box.cx() - w * 0.5f, box.y + (box.h - r.line_height(s)) * 0.5f + r.ascent(s)}, text);
}

void draw_monogram(Ui &ui, const Rect &circle, const Monogram &m, float text_size) {
    auto &r = ui.renderer();
    r.fill_rect(circle, ui.fade(m.color), circle.w * 0.5f);
    draw_centered(r, TextStyle{ui.theme().bold, text_size, ui.fade(kMonogramText)}, circle, m.letter);
}

// One shortcut on the New Tab page.
class ShortcutTile : public wnk::ui::View {
 public:
    ShortcutTile(NewTabPage::Shortcut s, std::function<void(Ui &, const std::string &)> open)
        : s_(std::move(s)), open_(std::move(open)) {
        set_focusable(true);
        set_size(200, 176);
        set_focus_ring(true, 16);
        accessible_label = s_.title;
    }

 protected:
    bool on_action(Ui &ui, const wnk::input::ActionEvent &e) override {
        if (!is_confirm(e)) return false;
        open_(ui, s_.url);
        return true;
    }
    void draw(Ui &ui) override {
        auto &r = ui.renderer();
        const Rect f = frame();
        if (ui.is_focused(this)) r.fill_rect(f, ui.fade(palette::kTabHover), 16);
        const float d = 88;
        draw_monogram(ui, {f.cx() - d * 0.5f, f.y + 20, d, d}, monogram_for(s_.url, s_.title), 40);
        const TextStyle s{ui.theme().regular, 22, ui.fade(palette::kText)};
        draw_centered(r, s, {f.x + 8, f.y + 20 + d + 12, f.w - 16, 36}, ellipsize(r, s, s_.title, f.w - 16));
    }

 private:
    NewTabPage::Shortcut s_;
    std::function<void(Ui &, const std::string &)> open_;
};

}  // namespace

void set_icon_dir(std::string dir) { g_icon_dir = std::move(dir); }

std::string icon_url(const std::string &name, bool dim) { return g_icon_dir + name + (dim ? "-dim" : "") + ".svg"; }

std::string host_of(const std::string &url) {
    size_t start = url.find("://");
    start = start == std::string::npos ? 0 : start + 3;
    size_t end = url.find_first_of("/?#:", start);
    std::string host = url.substr(start, end == std::string::npos ? std::string::npos : end - start);
    if (host.rfind("www.", 0) == 0) host = host.substr(4);
    return host;
}

Monogram monogram_for(const std::string &url, const std::string &title) {
    const std::string host = host_of(url);
    const std::string &src = !host.empty() && url.rfind("about:", 0) != 0 ? host : title;
    Monogram m{"", kMonogramColors[0]};
    for (unsigned char c : src) {
        if (std::isalnum(c)) {
            m.letter = std::string(1, static_cast<char>(std::toupper(c)));
            break;
        }
    }
    if (m.letter.empty()) m.letter = "•";
    uint32_t h = 2166136261u;  // FNV-1a: stable colour per site
    for (unsigned char c : host) h = (h ^ c) * 16777619u;
    m.color = kMonogramColors[h % (sizeof(kMonogramColors) / sizeof(kMonogramColors[0]))];
    return m;
}

std::string ellipsize(wnk::render::Renderer &r, const TextStyle &s, const std::string &text, float width) {
    if (width <= 0) return {};
    if (r.measure_text(s, text) <= width) return text;
    const std::string dots = "...";
    size_t end = text.size();
    while (end > 0) {
        // step back one UTF-8 code point
        do {
            --end;
        } while (end > 0 && (static_cast<unsigned char>(text[end]) & 0xC0) == 0x80);
        const std::string candidate = text.substr(0, end) + dots;
        if (r.measure_text(s, candidate) <= width) return candidate;
    }
    return dots;
}

// ---- IconButton ------------------------------------------------------------------------------------------

IconButton::IconButton(std::string icon, std::string label, wnk::ui::ClickHandler on_click, float size)
    : icon_(std::move(icon)), on_click_(std::move(on_click)), size_(size) {
    set_focusable(true);
    set_size(size, size);
    set_focus_ring(true, size * 0.5f);
    accessible_label = std::move(label);
    image_ = &add<wnk::ui::ImageView>(icon_url(icon_));
    image_->set_fit(wnk::ui::ImageFit::Contain);
}

void IconButton::set_icon(const std::string &icon) {
    if (icon == icon_) return;
    icon_ = icon;
    image_->set_url(icon_url(icon_, !active_));
}

void IconButton::set_active(bool active) {
    if (active == active_) return;
    active_ = active;
    image_->set_url(icon_url(icon_, !active_));
}

Size IconButton::measure_content(Ui &, Size) { return {size_, size_}; }

void IconButton::layout_content(Ui &ui, const Rect &content) {
    const float s = std::round(size_ * 0.56f);
    image_->layout(ui, {content.cx() - s * 0.5f, content.cy() - s * 0.5f, s, s});
}

bool IconButton::on_action(Ui &ui, const wnk::input::ActionEvent &e) {
    if (!is_confirm(e)) return false;
    if (active_ && on_click_) on_click_(ui);
    return true;
}

void IconButton::draw(Ui &ui) {
    if (ui.is_focused(this) && active_) ui.renderer().fill_rect(frame(), ui.fade(palette::kHover), size_ * 0.5f);
}

// ---- TabView ---------------------------------------------------------------------------------------------

TabView::TabView(TabInfo info, bool active, std::function<void(uint32_t)> on_activate, std::function<void(uint32_t)> on_close)
    : info_(std::move(info)), active_(active), on_activate_(std::move(on_activate)) {
    set_focusable(true);
    set_focus_ring(true, 12);
    accessible_label = info_.title;
    const uint32_t id = info_.id;
    close_ = &add<IconButton>("close", "Close tab", [on_close, id](Ui &) { on_close(id); }, 36);
}

void TabView::layout_content(Ui &ui, const Rect &content) {
    close_->layout(ui, {content.right() - 36 - 10, content.cy() - 18, 36, 36});
}

bool TabView::on_action(Ui &, const wnk::input::ActionEvent &e) {
    if (!is_confirm(e)) return false;
    on_activate_(info_.id);
    return true;
}

void TabView::draw(Ui &ui) {
    auto &r = ui.renderer();
    const Rect f = frame();
    const bool hot = ui.is_focused(this) || ui.is_focused(close_);
    if (active_) {
        // Joined to the toolbar below: rounded top, the bottom runs under the toolbar's background.
        r.fill_rect({f.x, f.y, f.w, f.h + 16}, ui.fade(palette::kToolbar), 12);
    } else if (hot) {
        r.fill_rect({f.x + 4, f.y + 4, f.w - 8, f.h - 8}, ui.fade(palette::kTabHover), 12);
    } else {
        r.fill_rect({f.right() - 1, f.y + 14, 1, f.h - 28}, ui.fade(palette::kSeparator));
    }
    const float d = 32;
    const Rect circle{f.x + 16, f.cy() - d * 0.5f, d, d};
    if (info_.url.empty())
        r.fill_rect(circle, ui.fade(palette::kHover), d * 0.5f);  // New Tab: a plain disc, no site letter
    else
        draw_monogram(ui, circle, monogram_for(info_.url, info_.title), 18);
    if (info_.loading) {
        const float a = 0.55f + 0.45f * std::sin(static_cast<float>(ui.now_ms()) / 180.0f);
        r.stroke_rect({circle.x - 4, circle.y - 4, circle.w + 8, circle.h + 8}, ui.fade(palette::kAccent.with_alpha(a)), 3,
                      (d + 8) * 0.5f);
    }
    const TextStyle s{ui.theme().regular, 22, ui.fade(active_ ? palette::kText : palette::kMuted)};
    const float x = circle.right() + 12;
    const float w = f.right() - 36 - 18 - x;
    const std::string title = info_.title.empty() ? (info_.url.empty() ? "New Tab" : host_of(info_.url)) : info_.title;
    r.draw_text(s, {x, f.y + (f.h - r.line_height(s)) * 0.5f + r.ascent(s)}, ellipsize(r, s, title, w));
}

// ---- TabStrip --------------------------------------------------------------------------------------------

TabStrip::TabStrip(Handlers h) : Row(0), h_(std::move(h)) {
    name = "tabstrip";
    set_padding({16, 10, 16, 0});
    set_width(wnk::ui::Dim::fill());
    set_height(wnk::ui::Dim::fixed(66));
    set_background(palette::kFrame);
}

void TabStrip::rebuild(Ui &, const std::vector<TabInfo> &tabs, uint32_t active) {
    clear_children();
    // Chromium: tabs share the width up to a comfortable maximum, then shrink.
    const float available = 1920 - 32 - 72;
    const float w = std::clamp(available / std::max<size_t>(tabs.size(), 1), 160.0f, 360.0f);
    for (const TabInfo &t : tabs) {
        auto &tv = add<TabView>(t, t.id == active, h_.activate, h_.close);
        tv.set_width(wnk::ui::Dim::fixed(w)).set_height(wnk::ui::Dim::fixed(56));
        tv.set_align(wnk::ui::Align::Start, wnk::ui::Align::End);
    }
    auto &plus = add<IconButton>("add", "New tab", [this](Ui &) { h_.new_tab(); }, 48);
    plus.set_align(wnk::ui::Align::Start, wnk::ui::Align::Center);
}

TabView *TabStrip::tab_view(uint32_t id) {
    for (auto &c : children()) {
        if (auto *tv = dynamic_cast<TabView *>(c.get()); tv != nullptr && tv->tab_id() == id) return tv;
    }
    return nullptr;
}

// ---- Omnibox ---------------------------------------------------------------------------------------------

Omnibox::Omnibox(std::function<void(Ui &, const std::string &)> on_submit, std::string placeholder, float height,
                 Color rest, Color focused, wnk::ui::ClickHandler on_star)
    : height_(height), rest_(rest), focused_(focused) {
    name = "omnibox";
    icon_ = &add<wnk::ui::ImageView>(icon_url("search", true));
    icon_->set_fit(wnk::ui::ImageFit::Contain);
    field_ = &add<wnk::ui::TextField>(std::move(placeholder));
    field_->name = "address";
    field_->set_focus_ring(false);  // the pill shows focus instead
    field_->set_chromeless(true);
    field_->set_on_submit(std::move(on_submit));
    if (on_star) {
        star_ = &add<IconButton>("star", "Bookmark this page", std::move(on_star), height * 0.75f);
        star_->set_visible(false);
    }
}

void Omnibox::set_star(bool shown, bool bookmarked) {
    if (star_ == nullptr) return;
    star_->set_visible(shown);
    star_->set_icon(bookmarked ? "star-on" : "star");
    star_->accessible_label = bookmarked ? "Edit bookmark" : "Bookmark this page";
}

void Omnibox::show_url(const std::string &url) {
    const bool https = url.rfind("https://", 0) == 0;
    const bool page = !url.empty() && url.rfind("about:", 0) != 0;
    icon_->set_url(icon_url(!page ? "search" : https ? "lock" : "info", !page));
    std::string shown = !page ? std::string() : https ? url.substr(8) : url;
    if (shown.size() > 1 && shown.back() == '/' && shown.find('/') == shown.size() - 1) shown.pop_back();
    field_->set_text(shown);
}

Size Omnibox::measure_content(Ui &, Size available) { return {available.w, height_}; }

void Omnibox::layout_content(Ui &ui, const Rect &c) {
    const float is = std::round(height_ * 0.46f);
    icon_->layout(ui, {c.x + height_ * 0.4f, c.cy() - is * 0.5f, is, is});
    const float fx = c.x + height_ * 0.4f + is + 12;
    float right = c.right() - height_ * 0.4f;
    if (star_ != nullptr && star_->visible()) {
        const float ss = height_ * 0.75f;
        star_->layout(ui, {c.right() - ss - 6, c.cy() - ss * 0.5f, ss, ss});
        right = c.right() - ss - 12;
    }
    field_->layout(ui, {fx, c.y, right - fx, c.h});
}

void Omnibox::draw(Ui &ui) {
    auto &r = ui.renderer();
    const Rect f = frame();
    const bool focused = ui.is_focused(field_) || field_->editing();
    r.fill_rect(f, ui.fade(focused ? focused_ : rest_), f.h * 0.5f);
    if (focused) r.stroke_rect(f, ui.fade(palette::kAccent), field_->editing() ? 3 : 2, f.h * 0.5f);
}

// ---- Chip / BookmarksBar ---------------------------------------------------------------------------------

Chip::Chip(std::string label, std::string url, std::string icon, wnk::ui::ClickHandler on_click)
    : label_(std::move(label)), url_(std::move(url)), on_click_(std::move(on_click)) {
    set_focusable(true);
    set_focus_ring(true, 18);
    set_height(wnk::ui::Dim::fixed(40));
    accessible_label = label_;
    if (!icon.empty()) {
        icon_ = &add<wnk::ui::ImageView>(icon_url(icon));
        icon_->set_fit(wnk::ui::ImageFit::Contain);
    }
}

Size Chip::measure_content(Ui &ui, Size) {
    auto &r = ui.renderer();
    const TextStyle s{ui.theme().regular, 21, palette::kText};
    const float text = std::min(r.measure_text(s, label_), 220.0f);
    return {12 + 24 + 10 + text + 14, 40};
}

void Chip::layout_content(Ui &ui, const Rect &c) {
    if (icon_ != nullptr) icon_->layout(ui, {c.x + 12, c.cy() - 12, 24, 24});
}

bool Chip::on_action(Ui &ui, const wnk::input::ActionEvent &e) {
    if (!is_confirm(e)) return false;
    if (on_click_) on_click_(ui);
    return true;
}

void Chip::draw(Ui &ui) {
    auto &r = ui.renderer();
    const Rect f = frame();
    if (ui.is_focused(this)) r.fill_rect(f, ui.fade(palette::kHover), f.h * 0.5f);
    if (icon_ == nullptr) {
        const Monogram m = monogram_for(url_, label_);
        draw_monogram(ui, {f.x + 12, f.cy() - 12, 24, 24}, m, 14);
    }
    const TextStyle s{ui.theme().regular, 21, ui.fade(palette::kText)};
    const float x = f.x + 12 + 24 + 10;
    r.draw_text(s, {x, f.y + (f.h - r.line_height(s)) * 0.5f + r.ascent(s)}, ellipsize(r, s, label_, f.right() - 14 - x));
}

BookmarksBar::BookmarksBar(std::function<void(Ui &, const std::string &)> open, wnk::ui::ClickHandler manage)
    : Row(6), open_(std::move(open)), manage_(std::move(manage)) {
    name = "bookmarksbar";
    set_padding({16, 4, 16, 8});
    set_width(wnk::ui::Dim::fill());
    set_background(palette::kToolbar);
}

void BookmarksBar::rebuild(Ui &ui, const std::vector<Item> &items) {
    clear_children();
    if (items.empty()) {
        auto &hint = add<wnk::ui::Label>("For quick access, bookmark pages with the star in the address bar.", wnk::ui::TextRole::Small);
        hint.set_muted(true);
        hint.set_padding({12, 8, 0, 8});
        return;
    }
    // As many chips as fit in one row (Chromium moves the rest behind ">>"); "All bookmarks" always shows.
    auto &r = ui.renderer();
    const TextStyle s{ui.theme().regular, 21, palette::kText};
    const float limit = 1920 - 32 - 260;
    float used = 0;
    for (const Item &it : items) {
        const float w = 12 + 24 + 10 + std::min(r.measure_text(s, it.title), 220.0f) + 14 + 6;
        if (used + w > limit) break;
        used += w;
        const std::string url = it.url;
        add<Chip>(it.title, it.url, "", [this, url](Ui &u) { open_(u, url); });
    }
    auto &spacer = add<wnk::ui::View>();
    spacer.set_width(wnk::ui::Dim::fill());
    add<Chip>("All bookmarks", "", "bookmarks", manage_);
}

// ---- ProgressLine ----------------------------------------------------------------------------------------

ProgressLine::ProgressLine() {
    set_width(wnk::ui::Dim::fill());
    set_height(wnk::ui::Dim::fixed(3));
}

void ProgressLine::set(bool loading, float value) {
    if (!loading_ && loading) shown_ = 0.05f;
    loading_ = loading;
    target_ = std::max(value, 0.05f);
}

void ProgressLine::draw(Ui &ui) {
    auto &r = ui.renderer();
    const Rect f = frame();
    r.fill_rect(f, ui.fade(palette::kToolbar));
    if (!loading_) return;
    shown_ += (target_ - shown_) * 0.25f;
    r.fill_rect({f.x, f.y, f.w * std::clamp(shown_, 0.0f, 1.0f), f.h}, ui.fade(palette::kAccent));
}

// ---- NewTabPage ------------------------------------------------------------------------------------------

NewTabPage::NewTabPage(std::vector<Shortcut> shortcuts, std::function<void(Ui &, const std::string &)> open) {
    name = "ntp";
    heading_ = &add<wnk::ui::Label>("Search the web", wnk::ui::TextRole::Title);
    heading_->set_text_align(wnk::ui::Align::Center);
    search_ = &add<Omnibox>(open, "Search DuckDuckGo or type a URL", 72, palette::kNtpSearch, palette::kNtpSearchFocused);
    search_->field().name = "ntp-search";
    tiles_ = &add<wnk::ui::Grid>(4, 24);
    for (auto &s : shortcuts) tiles_->add<ShortcutTile>(s, open);
}

void NewTabPage::layout_content(Ui &ui, const Rect &c) {
    const float y0 = c.y + c.h * 0.16f;
    const Size hs = heading_->measure(ui, {c.w, 120});
    heading_->layout(ui, {c.cx() - hs.w * 0.5f, y0, hs.w, hs.h});
    const float sw = std::min(960.0f, c.w - 160);
    search_->layout(ui, {c.cx() - sw * 0.5f, y0 + hs.h + 40, sw, 72});
    const Size ts = tiles_->measure(ui, {sw, c.h});
    tiles_->layout(ui, {c.cx() - ts.w * 0.5f, y0 + hs.h + 40 + 72 + 64, ts.w, ts.h});
}

void NewTabPage::draw(Ui &ui) { ui.renderer().fill_rect(frame(), ui.fade(palette::kNtpBackground)); }

}  // namespace browser
