// SPDX-License-Identifier: GPL-3.0-or-later
#include "keys.hpp"

#include <SDL.h>

namespace browser {
namespace {

// evdev codes (linux/input-event-codes.h); XKB keycodes are these + 8.
constexpr uint32_t kEvdevOffset = 8;

struct Entry {
    SDL_Keycode sdl;
    uint32_t keysym;
    uint32_t evdev;
};

constexpr Entry kTable[] = {
    {SDLK_UP, 0xff52, 103},       {SDLK_DOWN, 0xff54, 108},     {SDLK_LEFT, 0xff51, 105},
    {SDLK_RIGHT, 0xff53, 106},    {SDLK_RETURN, 0xff0d, 28},    {SDLK_KP_ENTER, 0xff8d, 96},
    {SDLK_BACKSPACE, 0xff08, 14}, {SDLK_TAB, 0xff09, 15},       {SDLK_ESCAPE, 0xff1b, 1},
    {SDLK_DELETE, 0xffff, 111},   {SDLK_HOME, 0xff50, 102},     {SDLK_END, 0xff57, 107},
    {SDLK_PAGEUP, 0xff55, 104},   {SDLK_PAGEDOWN, 0xff56, 109}, {SDLK_SPACE, 0x0020, 57},
    {SDLK_F5, 0xffc2, 63},
};

// evdev codes for printable keys, so pages reading KeyboardEvent.code see sensible values.
uint32_t evdev_for_ascii(int c) {
    static const char kRow1[] = "qwertyuiop";  // 16..25
    static const char kRow2[] = "asdfghjkl";   // 30..38
    static const char kRow3[] = "zxcvbnm";     // 44..50
    for (int i = 0; kRow1[i]; ++i)
        if (kRow1[i] == c) return 16u + static_cast<uint32_t>(i);
    for (int i = 0; kRow2[i]; ++i)
        if (kRow2[i] == c) return 30u + static_cast<uint32_t>(i);
    for (int i = 0; kRow3[i]; ++i)
        if (kRow3[i] == c) return 44u + static_cast<uint32_t>(i);
    if (c >= '1' && c <= '9') return 2u + static_cast<uint32_t>(c - '1');
    if (c == '0') return 11;
    return 0;
}

}  // namespace

WebKey web_key_from_sdl(int sdl_keycode) {
    for (const Entry &e : kTable) {
        if (e.sdl == sdl_keycode) return {e.keysym, e.evdev + kEvdevOffset};
    }
    if (sdl_keycode >= 0x20 && sdl_keycode <= 0x7e) {
        const uint32_t ev = evdev_for_ascii(sdl_keycode);
        return {static_cast<uint32_t>(sdl_keycode), ev ? ev + kEvdevOffset : 0};
    }
    return {};
}

}  // namespace browser
