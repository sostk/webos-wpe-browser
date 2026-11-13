// SPDX-License-Identifier: GPL-3.0-or-later
// Wren's TV interface (the design approved on 2026-11-14): a dark ambient background with soft colour glows, glass
// panels (translucent fills, hairline borders, a top highlight), Manrope, one ice-blue accent, and a focus style of
// a white ring with an accent halo. Everything works with the arrows, OK and Back alone (remotes without colour
// buttons); colour keys are shortcuts whose hints appear once such a key has been pressed.
//
//   HomePage      the New Tab page: header (logo, open tabs, menu, clock), search, shortcuts, recent pages
//   ChromePanel   over a web page: one glass panel with the tab row above the navigation row (▲ shows it)
//   TabOverview   all tabs as cards (Red, "All tabs")
//   SidePanel     bookmarks, history, zoom, settings, exit (the menu button, Green)
//   QuickActions  Hold OK anywhere: the common actions and page zoom (Yellow)
//
// The kit's renderer draws rounded rectangles, images and text, so glass is built from translucent fills over the
// ambient background (a texture computed once) or over the page; there is no live blur (the page is another
// surface, below this window).
#pragma once

#include "chrome.hpp"
#include "profile.hpp"

#include "wnk/ui/widgets.hpp"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace browser::wren {

using wnk::render::Color;

inline constexpr Color kInk = Color::hex(0x06080DFF);       // the background under the glows
inline constexpr Color kText = Color::hex(0xF2F4F8FF);
inline constexpr Color kSoft = Color::hex(0xC3CBDAFF);      // secondary text on glass
inline constexpr Color kMuted = Color::hex(0xA9B2C3FF);     // captions, hosts
inline constexpr Color kAccent = Color::hex(0xB9D3FFFF);    // ice blue
inline constexpr Color kAccentDeep = Color::hex(0x8FA2FFFF);
inline constexpr Color kOnAccent = Color::hex(0x0A0D14FF);  // text on accent and on monograms
inline constexpr Color kSecure = Color::hex(0x9FF0D2FF);
inline constexpr Color kRed = Color::hex(0xF06A6FFF), kGreen = Color::hex(0x4FD99AFF), kYellow = Color::hex(0xF3C94FFF),
                       kBlue = Color::hex(0x6C9BFFFF);

// Manrope, loaded at start (regular and semibold are also the kit theme's fonts).
struct Fonts {
    wnk::render::FontId light = wnk::render::kNoFont, regular = wnk::render::kNoFont, semibold = wnk::render::kNoFont,
                        bold = wnk::render::kNoFont;
};
Fonts &fonts();
void load_extra_fonts(wnk::render::Renderer &r, const std::string &app_dir);

// The icon URL for a page (the app answers from the profile's site icons); empty: the letter mark is drawn.
std::function<std::string(const std::string &page_url)> &icon_lookup();

// True once a colour key has been pressed: hints then name the colour keys too.
bool &colour_keys_seen();

// Drawing.
enum class Glass : uint8_t { Light, Dark };
void draw_ambient(wnk::ui::Ui &ui, const wnk::Rect &r);  // the background with its glows
void draw_glass(wnk::ui::Ui &ui, const wnk::Rect &r, float radius, Glass kind = Glass::Light, bool shadow = true);
void draw_focus(wnk::ui::Ui &ui, const wnk::Rect &r, float radius);  // white ring + accent halo
Color tile_color(const std::string &url, const std::string &title);      // pastel per site
void draw_tile_mark(wnk::ui::Ui &ui, const wnk::Rect &r, const std::string &url, const std::string &title, float text_size);
void draw_text_centered(wnk::ui::Ui &ui, const wnk::render::TextStyle &s, const wnk::Rect &box, const std::string &text);

// One hint in a hint pill: a key cap ("OK", "◀ ▶") or a colour dot, and what it does.
struct Hint {
    std::string key;
    Color dot = Color::hex(0x00000000);  // used when key is empty
    std::string text;
};

class HintPill : public wnk::ui::View {
 public:
    explicit HintPill(Glass kind = Glass::Light) : kind_(kind) {}
    void set_hints(std::vector<Hint> hints);

 protected:
    wnk::Size measure_content(wnk::ui::Ui &ui, wnk::Size available) override;
    void draw(wnk::ui::Ui &ui) override;

 private:
    Glass kind_;
    std::vector<Hint> hints_;
};

// A round glass button with an icon and/or a label (New tab, Close all, All tabs, +). `primary`: accent filled.
class GlassButton : public wnk::ui::View {
 public:
    GlassButton(std::string icon, std::string label, wnk::ui::ClickHandler on_click, float height = 66, bool primary = false);
    void set_label(std::string label);

 protected:
    wnk::Size measure_content(wnk::ui::Ui &ui, wnk::Size available) override;
    void layout_content(wnk::ui::Ui &ui, const wnk::Rect &content) override;
    bool on_action(wnk::ui::Ui &ui, const wnk::input::ActionEvent &e) override;
    void draw(wnk::ui::Ui &ui) override;

 private:
    wnk::ui::ImageView *image_ = nullptr;
    std::string label_;
    wnk::ui::ClickHandler on_click_;
    float height_;
    bool primary_;
};

// A tab as a chip: site mark and title; the shown tab is brighter. `compact`: the mark only (Home header), except
// for the shown tab.
class TabChip : public wnk::ui::View {
 public:
    TabChip(TabInfo info, bool active, bool compact, std::function<void(uint32_t)> on_activate);
    uint32_t tab_id() const { return info_.id; }

 protected:
    wnk::Size measure_content(wnk::ui::Ui &ui, wnk::Size available) override;
    bool on_action(wnk::ui::Ui &ui, const wnk::input::ActionEvent &e) override;
    void draw(wnk::ui::Ui &ui) override;

 private:
    TabInfo info_;
    bool active_, compact_;
    std::function<void(uint32_t)> on_activate_;
};

struct TabHandlers {
    std::function<void(uint32_t)> activate, close;
    std::function<void()> new_tab, show_all, close_all;
};

// The tab row of the chrome panel: chips, +, and "All tabs" at the right end.
class TabRow : public wnk::ui::Row {
 public:
    explicit TabRow(TabHandlers h);
    void rebuild(wnk::ui::Ui &ui, const std::vector<TabInfo> &tabs, uint32_t active);
    TabChip *chip(uint32_t id);

 private:
    TabHandlers h_;
};

// The glass panel over a web page: the tab row, a hairline, the navigation row; load progress along its bottom edge.
class ChromePanel : public wnk::ui::Column {
 public:
    ChromePanel();
    void set_progress(bool loading, float value);

 protected:
    void draw(wnk::ui::Ui &ui) override;
    void draw_over(wnk::ui::Ui &ui) override;

 private:
    bool loading_ = false;
    float target_ = 0, shown_ = 0;
};

// The New Tab page.
struct HomeData {
    struct Item {
        std::string title, url;
    };
    std::vector<Item> shortcuts, recents;
};
class HomePage : public wnk::ui::View {
 public:
    HomePage(HomeData data, TabHandlers tabs, std::function<void(wnk::ui::Ui &, const std::string &)> open,
             wnk::ui::ClickHandler menu);
    Omnibox &search() { return *search_; }
    void set_tabs(const std::vector<TabInfo> &tabs, uint32_t active);
    void set_recents(std::vector<HomeData::Item> recents);
    void refresh_hints();
    void show_hints(bool on);

 protected:
    void layout_content(wnk::ui::Ui &ui, const wnk::Rect &content) override;
    void update(wnk::ui::Ui &ui, float dt) override;
    void draw(wnk::ui::Ui &ui) override;

 private:
    std::function<void(wnk::ui::Ui &, const std::string &)> open_;
    TabHandlers tabs_h_;
    wnk::ui::ImageView *mark_;
    wnk::ui::Label *name_, *clock_, *search_label_, *recent_label_;
    wnk::ui::Row *tabs_;
    IconButton *menu_;
    Omnibox *search_;
    wnk::ui::Grid *tiles_, *recents_;
    HintPill *hints_;
    std::string clock_text_;
};

// Overlays: drawn over the browser screen, focus kept inside, Back closes them.
class Overlay : public wnk::ui::View {
 public:
    Overlay();
    std::function<void(wnk::ui::Ui &)> on_close;
    void close(wnk::ui::Ui &ui);
    bool on_action(wnk::ui::Ui &ui, const wnk::input::ActionEvent &e) override;
};

class TabOverview : public Overlay {
 public:
    explicit TabOverview(TabHandlers h);
    void rebuild(wnk::ui::Ui &ui, const std::vector<TabInfo> &tabs, uint32_t active);

 protected:
    void layout_content(wnk::ui::Ui &ui, const wnk::Rect &content) override;
    void draw(wnk::ui::Ui &ui) override;

 private:
    TabHandlers h_;
    wnk::ui::Label *title_, *count_;
    wnk::ui::Row *buttons_;
    wnk::ui::Grid *grid_;
    HintPill *hints_;
};

struct PanelHandlers {
    std::function<void(wnk::ui::Ui &, const std::string &)> open;
    std::function<void(wnk::ui::Ui &)> history_page, bookmarks_page, settings, exit;
    std::function<void(int delta)> zoom;        // -1 / +1 step
    std::function<std::string()> zoom_text;     // "110%"
};

class SidePanel : public Overlay {
 public:
    enum class Section : uint8_t { Saved, History, Zoom };
    explicit SidePanel(PanelHandlers h);
    void show(wnk::ui::Ui &ui, Section s, const std::vector<Bookmark> &bookmarks, const std::vector<HistoryEntry> &history);
    void refresh_zoom();

 protected:
    void layout_content(wnk::ui::Ui &ui, const wnk::Rect &content) override;
    void draw(wnk::ui::Ui &ui) override;

 private:
    void fill(wnk::ui::Ui &ui);
    PanelHandlers h_;
    Section section_ = Section::Saved;
    std::vector<Bookmark> bookmarks_;
    std::vector<HistoryEntry> history_;
    wnk::ui::Column *rail_;
    wnk::ui::Column *body_;
    wnk::Rect panel_{};
    wnk::ui::Label *zoom_label_ = nullptr;
};

struct QuickHandlers {
    std::function<void(wnk::ui::Ui &)> new_tab, bookmark, tabs, reload, close_tab, history, home, settings;
    std::function<void(int delta)> zoom;
    std::function<std::string()> zoom_text;
};

class QuickActions : public Overlay {
 public:
    explicit QuickActions(QuickHandlers h);
    // `title`/`url`: the shown page (empty on the New Tab page); `bookmarked`: the star's state.
    void show(wnk::ui::Ui &ui, const std::string &title, const std::string &url, bool bookmarked, bool focus_zoom);

 protected:
    void layout_content(wnk::ui::Ui &ui, const wnk::Rect &content) override;
    void draw(wnk::ui::Ui &ui) override;

 private:
    QuickHandlers h_;
    std::string title_, url_;
    wnk::ui::Grid *grid_;
    wnk::ui::Row *zoom_row_;
    wnk::ui::Label *zoom_label_;
    wnk::ui::View *zoom_in_ = nullptr;
    wnk::Rect panel_{};
    HintPill *hints_;
};

}  // namespace browser::wren
