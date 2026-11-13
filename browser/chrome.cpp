// SPDX-License-Identifier: GPL-3.0-or-later
#include "chrome.hpp"
#include "wren_ui.hpp"

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
bool is_confirm(const wnk::input::ActionEvent &e) {
    return e.action == wnk::input::Action::Confirm && e.phase == wnk::input::Phase::Press;
}

}  // namespace

void set_icon_dir(std::string dir) { g_icon_dir = std::move(dir); }

const std::string &icon_dir_url() { return g_icon_dir; }

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
    set_focus_ring(false);  // wren::draw_focus instead
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
    const float s = std::round(size_ * 0.46f);
    image_->layout(ui, {content.cx() - s * 0.5f, content.cy() - s * 0.5f, s, s});
}

bool IconButton::on_action(Ui &ui, const wnk::input::ActionEvent &e) {
    if (!is_confirm(e)) return false;
    if (active_ && on_click_) on_click_(ui);
    return true;
}

void IconButton::draw(Ui &ui) {
    if (!ui.is_focused(this) || !active_) return;
    ui.renderer().fill_rect(frame(), ui.fade(palette::kGlassFocused), size_ * 0.5f);
    wren::draw_focus(ui, frame(), size_ * 0.5f);
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
    r.stroke_rect(f, ui.fade(wnk::render::kWhite.with_alpha(focused ? 0.22f : 0.14f)), 1.5f, f.h * 0.5f);
    if (focused) wren::draw_focus(ui, f, f.h * 0.5f);
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
    if (!loading_) return;
    shown_ += (target_ - shown_) * 0.25f;
    r.fill_rect({f.x, f.y, f.w * std::clamp(shown_, 0.0f, 1.0f), f.h}, ui.fade(palette::kAccent));
}

}  // namespace browser
