// SPDX-License-Identifier: GPL-3.0-or-later
// SDL key events (what the kit reports) -> the XKB keysym + evdev keycode pairs WebKit's WPE port expects.
#pragma once

#include <cstdint>

namespace browser {

struct WebKey {
    uint32_t keyval = 0;   // XKB keysym; 0 = not for the page
    uint32_t keycode = 0;  // evdev code + 8, or 0
};

// `sdl_keycode` is an SDL_Keycode value (SDLK_*). Printable ASCII keycodes are their own keysyms.
WebKey web_key_from_sdl(int sdl_keycode);

}  // namespace browser
