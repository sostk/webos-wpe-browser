// SPDX-License-Identifier: GPL-3.0-or-later
// Shared chrome pieces, styled for Wren's glass look (wren_ui.hpp has the rest): round icon buttons, the address
// pill (omnibox), a thin progress line, site monograms and URL helpers. Everything works with the remote's D-pad
// (focus) and the Magic Remote pointer (hover = focus, click = OK).
#pragma once

#include "wnk/ui/widgets.hpp"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace browser {

// Colours the kit theme and the pushed pages (Settings, History, Bookmarks) use: Wren's ink and glass.
namespace palette {
inline constexpr wnk::render::Color kFrame = wnk::render::Color::hex(0x0B0E15FF);
inline constexpr wnk::render::Color kToolbar = wnk::render::Color::hex(0x141924FF);
inline constexpr wnk::render::Color kHover = wnk::render::Color::hex(0x2A3142FF);
inline constexpr wnk::render::Color kText = wnk::render::Color::hex(0xF2F4F8FF);
inline constexpr wnk::render::Color kMuted = wnk::render::Color::hex(0xA9B2C3FF);
inline constexpr wnk::render::Color kAccent = wnk::render::Color::hex(0xB9D3FFFF);
inline constexpr wnk::render::Color kCard = wnk::render::Color::hex(0x161B26FF);
inline constexpr wnk::render::Color kGlass = wnk::render::Color::hex(0xFFFFFF14);
inline constexpr wnk::render::Color kGlassFocused = wnk::render::Color::hex(0xFFFFFF26);
}  // namespace palette

// "file://<app>/icons/" for the SVG icons; set once at start.
void set_icon_dir(std::string file_url_dir);
const std::string &icon_dir_url();
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

// A round icon button (back, forward, reload, menu ...): a glass disc with the focus ring while focused/hovered.
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

// The omnibox: security/search icon and the address TextField in a pill that lightens while focused.
class Omnibox : public wnk::ui::View {
 public:
    explicit Omnibox(std::function<void(wnk::ui::Ui &, const std::string &)> on_submit, std::string placeholder,
                     float height = 64, wnk::render::Color rest = palette::kGlass,
                     wnk::render::Color focused = palette::kGlassFocused,
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

// A 3 px line at the top of the screen that fills while a page loads with the chrome hidden.
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

}  // namespace browser
