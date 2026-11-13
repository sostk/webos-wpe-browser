// SPDX-License-Identifier: GPL-3.0-or-later
#include "screens.hpp"

#include "wnk/ui/forms.hpp"
#include "wnk/ui/ui.hpp"
#include "wnk/ui/widgets.hpp"

#include <algorithm>

namespace browser {

using wnk::Rect;
using wnk::Size;
using wnk::render::Color;
using wnk::render::TextStyle;
using wnk::ui::Ui;

namespace {

constexpr Color kCard = palette::kCard;
constexpr float kColumnWidth = 1120;
constexpr size_t kMaxHistoryRows = 300;

bool is_confirm(const wnk::input::ActionEvent &e) {
    return e.action == wnk::input::Action::Confirm && e.phase == wnk::input::Phase::Press;
}

// A chrome://-style page: header with a back arrow and a title, then a scrolling centred column.
class PageScreen : public wnk::ui::Screen {
 public:
    PageScreen(const std::string &title, std::function<void(Ui &)> close) : Screen(title) {
        name = title;
        auto &col = add<wnk::ui::Column>(0);
        col.set_width(wnk::ui::Dim::fill()).set_height(wnk::ui::Dim::fill());
        col.set_background(palette::kFrame);

        auto &header = col.add<wnk::ui::Row>(20);
        header.set_padding({24, 16, 24, 16});
        header.set_width(wnk::ui::Dim::fill());
        header.set_background(palette::kFrame);
        header.add<IconButton>("back", "Back", std::move(close));
        auto &t = header.add<wnk::ui::Label>(title, wnk::ui::TextRole::Title);
        t.set_font_size(40);
        t.set_align(wnk::ui::Align::Start, wnk::ui::Align::Center);

        auto &line = col.add<wnk::ui::View>();
        line.set_width(wnk::ui::Dim::fill()).set_height(wnk::ui::Dim::fixed(1));
        line.set_background(palette::kHover);

        scroll_ = &col.add<wnk::ui::ScrollView>();
        scroll_->set_width(wnk::ui::Dim::fill()).set_height(wnk::ui::Dim::fill());
        body_ = &scroll_->add<wnk::ui::Column>(16);
        // The scroll view stretches its child, so centre the column with side padding.
        const float side = (1920 - kColumnWidth) * 0.5f;
        body_->set_width(wnk::ui::Dim::fill());
        body_->set_padding({side, 32, side, 64});
    }

 protected:
    wnk::ui::Column &section(const std::string &heading) {
        if (!heading.empty()) {
            auto &h = body_->add<wnk::ui::Label>(heading, wnk::ui::TextRole::Small);
            h.set_muted(true);
            h.set_padding({8, 16, 0, 0});
        }
        auto &card = body_->add<wnk::ui::Column>(0);
        card.set_width(wnk::ui::Dim::fill());
        card.set_padding(wnk::ui::Insets::all(8));
        card.set_background(kCard, 12);
        return card;
    }

    wnk::ui::ScrollView *scroll_ = nullptr;
    wnk::ui::Column *body_ = nullptr;
};

// One row of a list (history entry, bookmark): optional time, monogram, title over host, delete button.
class ListRow : public wnk::ui::View {
 public:
    ListRow(std::string time, std::string title, std::string url, std::function<void(Ui &)> open,
            std::function<void(Ui &)> remove)
        : time_(std::move(time)), title_(std::move(title)), url_(std::move(url)), open_(std::move(open)) {
        set_focusable(true);
        set_focus_ring(true, 10);
        set_width(wnk::ui::Dim::fill());
        set_height(wnk::ui::Dim::fixed(84));
        accessible_label = title_;
        remove_ = &add<IconButton>("delete", "Remove", std::move(remove), 52);
    }

 protected:
    void layout_content(Ui &ui, const Rect &c) override { remove_->layout(ui, {c.right() - 52 - 12, c.cy() - 26, 52, 52}); }
    bool on_action(Ui &ui, const wnk::input::ActionEvent &e) override {
        if (!is_confirm(e)) return false;
        open_(ui);
        return true;
    }
    void draw(Ui &ui) override {
        auto &r = ui.renderer();
        const Rect f = frame();
        if (ui.is_focused(this) || ui.is_focused(remove_)) r.fill_rect(f, ui.fade(palette::kHover), 10);
        float x = f.x + 20;
        if (!time_.empty()) {
            const TextStyle ts{ui.theme().regular, 22, ui.fade(palette::kMuted)};
            r.draw_text(ts, {x, f.y + (f.h - r.line_height(ts)) * 0.5f + r.ascent(ts)}, time_);
            x += 96;
        }
        const Monogram m = monogram_for(url_, title_);
        const float d = 40;
        r.fill_rect({x, f.cy() - d * 0.5f, d, d}, ui.fade(m.color), d * 0.5f);
        const TextStyle ls{ui.theme().bold, 20, ui.fade(Color::hex(0x202124FF))};
        const float lw = r.measure_text(ls, m.letter);
        r.draw_text(ls, {x + (d - lw) * 0.5f, f.cy() - r.line_height(ls) * 0.5f + r.ascent(ls)}, m.letter);
        x += d + 20;
        const float w = f.right() - 52 - 32 - x;
        const TextStyle t{ui.theme().regular, 25, ui.fade(palette::kText)};
        const TextStyle u{ui.theme().regular, 20, ui.fade(palette::kMuted)};
        r.draw_text(t, {x, f.y + 14 + r.ascent(t)}, ellipsize(r, t, title_.empty() ? url_ : title_, w));
        r.draw_text(u, {x, f.y + 48 + r.ascent(u)}, ellipsize(r, u, host_of(url_), w));
    }

 private:
    std::string time_, title_, url_;
    std::function<void(Ui &)> open_;
    IconButton *remove_;
};

// ---- Settings --------------------------------------------------------------------------------------------

class SettingsScreen : public PageScreen {
 public:
    SettingsScreen(Profile &p, PageHooks h) : PageScreen("Settings", h.close) {
        Settings &s = p.settings;
        auto changed = [&p, h](Ui &ui) {
            p.settings_changed();
            if (h.settings_changed) h.settings_changed(ui);
        };

        auto &look = section("Appearance");
        std::vector<std::string> zooms;
        for (int z : kZoomLevels) zooms.push_back(std::to_string(z / 10) + " %");
        auto &zoom = look.add<wnk::ui::Choice>("Page zoom", zooms, s.zoom, [&s, changed](Ui &ui, int i) {
            s.zoom = i;
            changed(ui);
        });
        zoom.set_description("Makes text and pictures on every page larger");
        auto &sharp = look.add<wnk::ui::Toggle>("Sharp text", s.sharp_text, [&s, changed](Ui &ui, bool on) {
            s.sharp_text = on;
            changed(ui);
        });
        sharp.set_description("Draws pages at the screen's full resolution. Turn off if pages scroll slowly");

        auto &search = section("Search engine");
        std::vector<std::string> engines;
        for (const auto &e : kSearchEngines) engines.emplace_back(e.name);
        search.add<wnk::ui::Choice>("Search engine used in the address bar", engines, s.search_engine,
                                    [&s, changed](Ui &ui, int i) {
                                        s.search_engine = i;
                                        changed(ui);
                                    });

        auto &start = section("On startup");
        start.add<wnk::ui::Choice>("When the browser opens",
                                   std::vector<std::string>{"Open the New Tab page", "Continue where you left off"},
                                   s.restore_session ? 1 : 0, [&s, changed](Ui &ui, int i) {
                                       s.restore_session = i == 1;
                                       changed(ui);
                                   });

        auto &privacy = section("Privacy and security");
        privacy.add<wnk::ui::ValueRow>("Clear browsing data")
            .set_value("History, cookies, cache")
            .set_on_click([h](Ui &ui) { h.show_clear_data(ui); });
        privacy.add<wnk::ui::ValueRow>("History").set_value(std::to_string(p.history().size()) + " pages")
            .set_on_click([h](Ui &ui) { h.show_history(ui); });
        privacy.add<wnk::ui::ValueRow>("Bookmarks").set_value(std::to_string(p.bookmarks().size()))
            .set_on_click([h](Ui &ui) { h.show_bookmarks(ui); });

        auto &about = section("About Wren");
        about.add<wnk::ui::ValueRow>("Version").set_value("0.3.0");
        const std::string engine = h.engine_version ? h.engine_version() : std::string();
        about.add<wnk::ui::ValueRow>("Engine").set_value(engine.empty() ? "WPE WebKit (not running)" : "WPE WebKit " + engine);
        about.add<wnk::ui::ValueRow>("Licence").set_value("GPL-3.0-or-later");
    }
};

// ---- Clear browsing data ---------------------------------------------------------------------------------

class ClearDataScreen : public PageScreen {
 public:
    explicit ClearDataScreen(PageHooks h) : PageScreen("Clear browsing data", h.close) {
        auto &range = section("");
        static const std::vector<std::pair<std::string, uint32_t>> kRanges = {
            {"Last hour", 3600}, {"Last 24 hours", 86400}, {"Last 7 days", 7 * 86400}, {"Last 4 weeks", 28 * 86400},
            {"All time", 0}};
        std::vector<std::string> names;
        for (const auto &r : kRanges) names.push_back(r.first);
        range.add<wnk::ui::Choice>("Time range", names, range_, [this](Ui &, int i) { range_ = i; });

        auto &what = section("");
        what.add<wnk::ui::Toggle>("Browsing history", history_, [this](Ui &, bool on) { history_ = on; })
            .set_style(wnk::ui::Toggle::Style::Check)
            .set_description("Clears the list of pages you have visited");
        what.add<wnk::ui::Toggle>("Cookies and other site data", cookies_, [this](Ui &, bool on) { cookies_ = on; })
            .set_style(wnk::ui::Toggle::Style::Check)
            .set_description("Signs you out of most sites");
        what.add<wnk::ui::Toggle>("Cached images and files", cache_, [this](Ui &, bool on) { cache_ = on; })
            .set_style(wnk::ui::Toggle::Style::Check)
            .set_description("Frees up space. Some sites may load more slowly on your next visit.");

        auto &buttons = body_->add<wnk::ui::Row>(16);
        buttons.set_padding({0, 16, 0, 0});
        buttons.set_align(wnk::ui::Align::End, wnk::ui::Align::Start);
        buttons.add<wnk::ui::Button>("Cancel", [h](Ui &ui) { h.close(ui); });
        buttons.add<wnk::ui::Button>("Clear data", [this, h](Ui &ui) {
            h.clear(ui, history_, cookies_, cache_, kRanges[static_cast<size_t>(range_)].second);
            h.close(ui);
        });
    }

 private:
    int range_ = 0;
    bool history_ = true, cookies_ = true, cache_ = true;
};

// ---- History ---------------------------------------------------------------------------------------------

class HistoryScreen : public PageScreen {
 public:
    HistoryScreen(Profile &p, PageHooks h) : PageScreen("History", h.close), p_(p), h_(std::move(h)) { fill(); }

 private:
    void fill(size_t focus_index = SIZE_MAX, Ui *ui = nullptr) {
        body_->clear_children();
        auto &top = section("");
        top.add<wnk::ui::ValueRow>("Clear browsing data").set_value("History, cookies, cache")
            .set_on_click([this](Ui &u) { h_.show_clear_data(u); });
        const auto &entries = p_.history();
        if (entries.empty()) {
            auto &empty = body_->add<wnk::ui::Label>("Pages you visit will appear here.");
            empty.set_muted(true);
            empty.set_padding({8, 24, 0, 0});
            return;
        }
        std::string day;
        wnk::ui::Column *card = nullptr;
        wnk::ui::View *to_focus = nullptr;
        const size_t n = std::min(entries.size(), kMaxHistoryRows);
        for (size_t i = 0; i < n; ++i) {
            const HistoryEntry e = entries[i];
            const std::string d = day_label(e.time);
            if (card == nullptr || d != day) {
                day = d;
                card = &section(d);
            }
            auto &row = card->add<ListRow>(
                time_label(e.time), e.title, e.url, [this, url = e.url](Ui &u) { h_.open(u, url); },
                [this, e, i](Ui &u) {
                    p_.remove_history(e.url, e.time);
                    fill(i, &u);
                });
            if (i == focus_index || (focus_index != SIZE_MAX && i + 1 == n && to_focus == nullptr)) to_focus = &row;
        }
        if (entries.size() > n) {
            auto &more = body_->add<wnk::ui::Label>("Showing the newest " + std::to_string(n) + " of " +
                                                     std::to_string(entries.size()) + " pages.", wnk::ui::TextRole::Small);
            more.set_muted(true);
        }
        if (ui != nullptr && to_focus != nullptr) ui->focus(to_focus);
    }

    Profile &p_;
    PageHooks h_;
};

// ---- Bookmarks -------------------------------------------------------------------------------------------

class BookmarksScreen : public PageScreen {
 public:
    BookmarksScreen(Profile &p, PageHooks h) : PageScreen("Bookmarks", h.close), p_(p), h_(std::move(h)) { fill(); }

 private:
    void fill(size_t focus_index = SIZE_MAX, Ui *ui = nullptr) {
        body_->clear_children();
        const auto &items = p_.bookmarks();
        if (items.empty()) {
            auto &empty = body_->add<wnk::ui::Label>("No bookmarks yet. Open a page and select the star in the address bar.");
            empty.set_muted(true);
            empty.set_padding({8, 24, 0, 0});
            return;
        }
        auto &card = section("Bookmarks bar");
        wnk::ui::View *to_focus = nullptr;
        for (size_t i = 0; i < items.size(); ++i) {
            const Bookmark b = items[i];
            auto &row = card.add<ListRow>(
                "", b.title, b.url, [this, url = b.url](Ui &u) { h_.open(u, url); },
                [this, url = b.url, i](Ui &u) {
                    p_.remove_bookmark(url);
                    if (h_.bookmarks_changed) h_.bookmarks_changed(u);
                    fill(i, &u);
                });
            if (i == std::min(focus_index, items.size() - 1)) to_focus = &row;
        }
        if (ui != nullptr && to_focus != nullptr) ui->focus(to_focus);
    }

    Profile &p_;
    PageHooks h_;
};

// ---- Menu ------------------------------------------------------------------------------------------------

class MenuRow : public wnk::ui::View {
 public:
    explicit MenuRow(MenuItem item) : item_(std::move(item)) {
        set_focusable(true);
        set_focus_ring(true, 10);
        set_width(wnk::ui::Dim::fill());
        set_height(wnk::ui::Dim::fixed(64));
        accessible_label = item_.label;
        icon_ = &add<wnk::ui::ImageView>(icon_url(item_.icon));
        icon_->set_fit(wnk::ui::ImageFit::Contain);
    }

 protected:
    void layout_content(Ui &ui, const Rect &c) override { icon_->layout(ui, {c.x + 20, c.cy() - 16, 32, 32}); }
    bool on_action(Ui &ui, const wnk::input::ActionEvent &e) override {
        if (!is_confirm(e)) return false;
        auto action = item_.action;  // the dialog (and this row) goes away on dismiss
        if (auto *d = ui.top_dialog()) d->dismiss(ui);
        if (action) action(ui);
        return true;
    }
    void draw(Ui &ui) override {
        auto &r = ui.renderer();
        const Rect f = frame();
        if (ui.is_focused(this)) r.fill_rect(f, ui.fade(palette::kHover), 10);
        const TextStyle s{ui.theme().regular, 26, ui.fade(palette::kText)};
        r.draw_text(s, {f.x + 72, f.y + (f.h - r.line_height(s)) * 0.5f + r.ascent(s)}, item_.label);
        if (!item_.hint.empty()) {
            const TextStyle hs{ui.theme().regular, 21, ui.fade(palette::kMuted)};
            const float w = r.measure_text(hs, item_.hint);
            r.draw_text(hs, {f.right() - 20 - w, f.y + (f.h - r.line_height(hs)) * 0.5f + r.ascent(hs)}, item_.hint);
        }
    }

 private:
    MenuItem item_;
    wnk::ui::ImageView *icon_;
};

}  // namespace

std::unique_ptr<wnk::ui::Screen> make_settings_screen(Profile &profile, PageHooks hooks) {
    return std::make_unique<SettingsScreen>(profile, std::move(hooks));
}
std::unique_ptr<wnk::ui::Screen> make_clear_data_screen(PageHooks hooks) {
    return std::make_unique<ClearDataScreen>(std::move(hooks));
}
std::unique_ptr<wnk::ui::Screen> make_history_screen(Profile &profile, PageHooks hooks) {
    return std::make_unique<HistoryScreen>(profile, std::move(hooks));
}
std::unique_ptr<wnk::ui::Screen> make_bookmarks_screen(Profile &profile, PageHooks hooks) {
    return std::make_unique<BookmarksScreen>(profile, std::move(hooks));
}

void show_menu(Ui &ui, std::vector<MenuItem> items, const std::string &footer) {
    // Chromium's app menu: a narrow drop-down under the ⋮ button, not a centred dialog.
    auto dialog = std::make_unique<wnk::ui::Dialog>("");
    dialog->set_panel_width(600).set_panel_padding(wnk::ui::Insets::all(12)).anchor_top_right({1920 - 16, 124});
    auto &list = dialog->body();
    list.set_width(wnk::ui::Dim::fill());
    for (auto &item : items) list.add<MenuRow>(std::move(item));
    if (!footer.empty()) {
        auto &f = list.add<wnk::ui::Label>(footer, wnk::ui::TextRole::Small);
        f.set_muted(true);
        f.set_padding({20, 12, 0, 0});
    }
    ui.show_dialog(std::move(dialog));
}

}  // namespace browser
