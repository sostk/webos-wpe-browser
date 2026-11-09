// SPDX-License-Identifier: GPL-3.0-or-later
// The browser's own pages, pushed on the kit's Navigator over the browser screen (Back pops them):
// Settings, Clear browsing data, History and Bookmarks, laid out like Chromium's chrome:// pages (a header
// with a back arrow, then a centred column of rounded cards). Also the ⋮ menu.
#pragma once

#include "chrome.hpp"
#include "profile.hpp"

#include "wnk/ui/navigation.hpp"

#include <functional>
#include <string>

namespace browser {

// What the pages need from the app.
struct PageHooks {
    std::function<void(wnk::ui::Ui &)> close;                        // pop this page
    std::function<void(wnk::ui::Ui &, const std::string &url)> open;  // open in the current tab, then close
    std::function<void(wnk::ui::Ui &, const std::string &url)> open_new_tab;
    std::function<void(wnk::ui::Ui &)> settings_changed;              // after any setting changed
    std::function<void(wnk::ui::Ui &)> bookmarks_changed;
    std::function<void(wnk::ui::Ui &)> show_clear_data;
    std::function<void(wnk::ui::Ui &)> show_history;
    std::function<void(wnk::ui::Ui &)> show_bookmarks;
    // Deletes the chosen data: history in the app, cookies/site data/cache in the engine.
    std::function<void(wnk::ui::Ui &, bool history, bool cookies, bool cache, uint32_t seconds)> clear;
    std::function<std::string()> engine_version;  // "" until the engine is ready
};

std::unique_ptr<wnk::ui::Screen> make_settings_screen(Profile &profile, PageHooks hooks);
std::unique_ptr<wnk::ui::Screen> make_clear_data_screen(PageHooks hooks);
std::unique_ptr<wnk::ui::Screen> make_history_screen(Profile &profile, PageHooks hooks);
std::unique_ptr<wnk::ui::Screen> make_bookmarks_screen(Profile &profile, PageHooks hooks);

// The ⋮ menu: a vertical list of actions with icons, in a dialog.
struct MenuItem {
    std::string icon, label, hint;  // hint: e.g. the colour key that does the same
    std::function<void(wnk::ui::Ui &)> action;
};
void show_menu(wnk::ui::Ui &ui, std::vector<MenuItem> items, const std::string &footer);

}  // namespace browser
