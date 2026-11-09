// SPDX-License-Identifier: GPL-3.0-or-later
// Browser chrome in the style of desktop Chromium's dark theme, built on the kit's views: a tab strip with
// site monograms, a toolbar of round icon buttons, a pill-shaped omnibox, a thin progress line and a native
// New Tab page. Everything works with the remote's D-pad (focus) and the Magic Remote pointer (hover = focus,
// click = OK).
#pragma once

#include "wnk/ui/widgets.hpp"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace browser {

// Chromium dark palette.
namespace palette {
inline constexpr wnk::render::Color kFrame = wnk::render::Color::hex(0x202124FF);
inline constexpr wnk::render::Color kToolbar = wnk::render::Color::hex(0x35363AFF);
inline constexpr wnk::render::Color kOmnibox = wnk::render::Color::hex(0x202124FF);
inline constexpr wnk::render::Color kOmniboxFocused = wnk::render::Color::hex(0x303134FF);
inline constexpr wnk::render::Color kHover = wnk::render::Color::hex(0x4A4C50FF);
inline constexpr wnk::render::Color kTabHover = wnk::render::Color::hex(0x2D2E31FF);
inline constexpr wnk::render::Color kSeparator = wnk::render::Color::hex(0x5F6368FF);
inline constexpr wnk::render::Color kText = wnk::render::Color::hex(0xE8EAEDFF);
inline constexpr wnk::render::Color kMuted = wnk::render::Color::hex(0x9AA0A6FF);
inline constexpr wnk::render::Color kAccent = wnk::render::Color::hex(0x8AB4F8FF);
inline constexpr wnk::render::Color kCard = wnk::render::Color::hex(0x292A2DFF);
inline constexpr wnk::render::Color kNtpBackground = wnk::render::Color::hex(0x202124FF);
inline constexpr wnk::render::Color kNtpSearch = wnk::render::Color::hex(0x303134FF);
inline constexpr wnk::render::Color kNtpSearchFocused = wnk::render::Color::hex(0x3C4043FF);
}  // namespace palette

// "file://<app>/icons/" for the SVG icons; set once at start.
void set_icon_dir(std::string file_url_dir);
std::string icon_url(const std::string &name, bool dim = false);

// The letter and colour that stand in for a site's favicon (Chromium's monogram fallback).
struct Monogram {
    std::string letter;
    wnk::render::Color color;
};
Monogram monogram_for(const std::string &url, const std::string &title);
std::string host_of(const std::string &url);
// Cuts `text` to fit `width` with an ellipsis, on code point boundaries.
std::string ellipsize(wnk::render::Renderer &r, const wnk::render::TextStyle &s, const std::string &text, float width);

// A round icon button (back, forward, reload, menu ...): grey disc while focused/hovered.
class IconButton : public wnk::ui::View {
 public:
    IconButton(std::string icon, std::string label, wnk::ui::ClickHandler on_click, float size = 64);
    void set_icon(const std::string &icon);
    void set_active(bool active);  // dimmed and inert when false (e.g. Back with no history)

 protected:
    wnk::Size measure_content(wnk::ui::Ui &ui, wnk::Size available) override;
    void layout_content(wnk::ui::Ui &ui, const wnk::Rect &content) override;
    bool on_action(wnk::ui::Ui &ui, const wnk::input::ActionEvent &e) override;
    void draw(wnk::ui::Ui &ui) override;

 private:
    wnk::ui::ImageView *image_;
    std::string icon_;
    wnk::ui::ClickHandler on_click_;
    float size_;
    bool active_ = true;
};

struct TabInfo {
    uint32_t id = 0;
    std::string title, url;
    bool loading = false;
};

// One tab in the strip: monogram, title, close button. OK activates it.
class TabView : public wnk::ui::View {
 public:
    TabView(TabInfo info, bool active, std::function<void(uint32_t)> on_activate, std::function<void(uint32_t)> on_close);
    uint32_t tab_id() const { return info_.id; }

 protected:
    void layout_content(wnk::ui::Ui &ui, const wnk::Rect &content) override;
    bool on_action(wnk::ui::Ui &ui, const wnk::input::ActionEvent &e) override;
    void draw(wnk::ui::Ui &ui) override;

 private:
    TabInfo info_;
    bool active_;
    std::function<void(uint32_t)> on_activate_;
    IconButton *close_;
};

// The strip of tabs plus the new-tab button. rebuild() whenever tabs change.
class TabStrip : public wnk::ui::Row {
 public:
    struct Handlers {
        std::function<void(uint32_t)> activate, close;
        std::function<void()> new_tab;
    };
    explicit TabStrip(Handlers h);
    void rebuild(wnk::ui::Ui &ui, const std::vector<TabInfo> &tabs, uint32_t active);
    TabView *tab_view(uint32_t id);

 private:
    Handlers h_;
};

// The omnibox: security/search icon and the address TextField in a pill that lightens while focused.
class Omnibox : public wnk::ui::View {
 public:
    explicit Omnibox(std::function<void(wnk::ui::Ui &, const std::string &)> on_submit, std::string placeholder,
                     float height = 64, wnk::render::Color rest = palette::kOmnibox,
                     wnk::render::Color focused = palette::kOmniboxFocused,
                     wnk::ui::ClickHandler on_star = {});
    // The bookmark star at the right end (only when constructed with on_star): hidden for New Tab pages.
    void set_star(bool shown, bool bookmarked);
    // Shows `url` the way Chromium does: no "https://", a lock for https, an info icon otherwise.
    void show_url(const std::string &url);
    wnk::ui::TextField &field() { return *field_; }

 protected:
    wnk::Size measure_content(wnk::ui::Ui &ui, wnk::Size available) override;
    void layout_content(wnk::ui::Ui &ui, const wnk::Rect &content) override;
    void draw(wnk::ui::Ui &ui) override;

 private:
    wnk::ui::ImageView *icon_;
    wnk::ui::TextField *field_;
    IconButton *star_ = nullptr;
    float height_;
    wnk::render::Color rest_, focused_;
};

// The bookmarks ("favourites") bar under the toolbar: one chip per bookmark that fits, then "All bookmarks".
class BookmarksBar : public wnk::ui::Row {
 public:
    struct Item {
        std::string title, url;
    };
    BookmarksBar(std::function<void(wnk::ui::Ui &, const std::string &)> open, wnk::ui::ClickHandler manage);
    void rebuild(wnk::ui::Ui &ui, const std::vector<Item> &items);

 private:
    std::function<void(wnk::ui::Ui &, const std::string &)> open_;
    wnk::ui::ClickHandler manage_;
};

// A focusable pill with a monogram (or icon) and a label: bookmark chips, "All bookmarks".
class Chip : public wnk::ui::View {
 public:
    Chip(std::string label, std::string url, std::string icon, wnk::ui::ClickHandler on_click);

 protected:
    wnk::Size measure_content(wnk::ui::Ui &ui, wnk::Size available) override;
    void layout_content(wnk::ui::Ui &ui, const wnk::Rect &content) override;
    bool on_action(wnk::ui::Ui &ui, const wnk::input::ActionEvent &e) override;
    void draw(wnk::ui::Ui &ui) override;

 private:
    std::string label_, url_;
    wnk::ui::ImageView *icon_ = nullptr;
    wnk::ui::ClickHandler on_click_;
};

// A 3 px line under the toolbar that fills while a page loads.
class ProgressLine : public wnk::ui::View {
 public:
    ProgressLine();
    void set(bool loading, float value);

 protected:
    void draw(wnk::ui::Ui &ui) override;

 private:
    bool loading_ = false;
    float target_ = 0, shown_ = 0;
};

// Chromium's New Tab page, natively: a search box and shortcut tiles. Shown for tabs without a page.
class NewTabPage : public wnk::ui::View {
 public:
    struct Shortcut {
        std::string title, url;
    };
    NewTabPage(std::vector<Shortcut> shortcuts, std::function<void(wnk::ui::Ui &, const std::string &)> open);
    Omnibox &search() { return *search_; }

 protected:
    void layout_content(wnk::ui::Ui &ui, const wnk::Rect &content) override;
    void draw(wnk::ui::Ui &ui) override;

 private:
    wnk::ui::Label *heading_;
    Omnibox *search_;
    wnk::ui::Grid *tiles_;
};

}  // namespace browser
