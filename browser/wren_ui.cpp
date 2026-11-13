// SPDX-License-Identifier: GPL-3.0-or-later
#include "wren_ui.hpp"

#include "wnk/log.hpp"
#include "wnk/ui/ui.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <ctime>
#include <map>

namespace browser::wren {

using wnk::Rect;
using wnk::Size;
using wnk::render::TextStyle;
using wnk::ui::Ui;

namespace {

constexpr Color kWhite = Color::hex(0xFFFFFFFF);
constexpr Color kBlack = Color::hex(0x000000FF);

constexpr Color kTileColors[] = {
    Color::hex(0xB9D3FFFF), Color::hex(0x9FF0D2FF), Color::hex(0xF3D29BFF), Color::hex(0xD9C2FFFF),
    Color::hex(0xFFB4AEFF), Color::hex(0xA6E6F2FF), Color::hex(0xFFC6A8FF), Color::hex(0xE8ECF2FF),
};

bool is_confirm(const wnk::input::ActionEvent &e) {
    return e.action == wnk::input::Action::Confirm && e.phase == wnk::input::Phase::Press;
}

TextStyle style(wnk::render::FontId font, float size, Color color) { return TextStyle{font, size, color}; }

float baseline(wnk::render::Renderer &r, const TextStyle &s, const Rect &box) {
    return box.y + (box.h - r.line_height(s)) * 0.5f + r.ascent(s);
}

// The background: three soft glows over near-black, computed once at low resolution (the GPU's bilinear scaling keeps
// it smooth) with a little noise against banding.
struct Ambient {
    wnk::render::Renderer *renderer = nullptr;
    wnk::render::TextureId texture = 0;
};
Ambient g_ambient;

wnk::render::TextureId ambient_texture(wnk::render::Renderer &r) {
    if (g_ambient.renderer == &r && g_ambient.texture != 0) return g_ambient.texture;
    constexpr int W = 384, H = 216;
    struct Glow {
        float cx, cy, rx, ry, stop, r, g, b, a;
    };
    // CSS radial-gradient(<rx> <ry> at <cx> <cy>, colour, transparent <stop>), on a 1280x720 frame.
    const Glow glows[] = {
        {0.62f, 1.15f, 900.f / 1280, 600.f / 720, 0.62f, 255, 164, 92, 0.20f},
        {0.92f, 0.22f, 700.f / 1280, 560.f / 720, 0.60f, 178, 104, 255, 0.24f},
        {0.12f, 0.08f, 760.f / 1280, 520.f / 720, 0.62f, 88, 120, 255, 0.38f},
    };
    std::vector<uint8_t> px(static_cast<size_t>(W) * H * 4);
    uint32_t seed = 0x9E3779B9u;
    for (int y = 0; y < H; ++y) {
        for (int x = 0; x < W; ++x) {
            float cr = 6, cg = 8, cb = 13;
            const float u = (x + 0.5f) / W, v = (y + 0.5f) / H;
            for (const Glow &g : glows) {
                const float dx = (u - g.cx) / g.rx, dy = (v - g.cy) / g.ry;
                float t = std::sqrt(dx * dx + dy * dy) / g.stop;
                if (t >= 1) continue;
                t = 1 - t;
                const float a = g.a * t * t * (3 - 2 * t);  // smoothstep
                cr += (g.r - cr) * a;
                cg += (g.g - cg) * a;
                cb += (g.b - cb) * a;
            }
            seed = seed * 1664525u + 1013904223u;
            const float n = static_cast<float>((seed >> 24) & 0xFF) / 255.0f - 0.5f;
            uint8_t *p = &px[(static_cast<size_t>(y) * W + x) * 4];
            p[0] = static_cast<uint8_t>(std::clamp(cr + n, 0.0f, 255.0f));
            p[1] = static_cast<uint8_t>(std::clamp(cg + n, 0.0f, 255.0f));
            p[2] = static_cast<uint8_t>(std::clamp(cb + n, 0.0f, 255.0f));
            p[3] = 255;
        }
    }
    g_ambient.renderer = &r;
    g_ambient.texture = r.create_texture(W, H, px.data());
    return g_ambient.texture;
}

// Site icons, loaded through the kit's image service (HTTP, disk cache, PNG/JPEG/GIF/WebP/SVG) and drawn when ready.
struct IconEntry {
    enum State : uint8_t { Idle, Loading, Ready, Failed } state = Idle;
    std::string key;
};
std::map<std::string, IconEntry> g_icons;

wnk::render::TextureId icon_texture(Ui &ui, const std::string &icon) {
    wnk::ui::ImageSource *src = ui.images();
    if (src == nullptr || icon.empty()) return wnk::render::kNoTexture;
    IconEntry &e = g_icons[icon];
    if (e.state == IconEntry::Ready) {
        if (const auto t = src->texture(e.key); t != wnk::render::kNoTexture) return t;
        e.state = IconEntry::Idle;  // evicted: load again
    }
    if (e.state == IconEntry::Idle) {
        e.state = IconEntry::Loading;
        Ui *u = &ui;
        src->load(icon, 128, 128, [u, icon](wnk::ui::ImageSource::Loaded l) {
            IconEntry &done = g_icons[icon];
            done.state = l.ok ? IconEntry::Ready : IconEntry::Failed;
            done.key = l.key;
            if (!l.ok) WNK_LOGI("wren", "icon %s: %s", icon.c_str(), l.error.c_str());
            u->invalidate_layout();  // draw again with it
        });
    }
    return wnk::render::kNoTexture;
}

std::string clock_now() {
    std::time_t t = std::time(nullptr);
    std::tm tm{};
    localtime_r(&t, &tm);
    char buf[16];
    std::strftime(buf, sizeof buf, "%I:%M", &tm);
    std::string s = buf;
    if (!s.empty() && s[0] == '0') s.erase(0, 1);
    return s;
}

// A key cap ("OK") or a colour dot and its text, as laid out in a hint pill.
float hint_width(wnk::render::Renderer &r, const Hint &h) {
    const TextStyle cap = style(fonts().semibold, 20, kText), text = style(fonts().regular, 21, kSoft);
    const float lead = h.key.empty() ? 18 : r.measure_text(cap, h.key) + 24;
    return lead + 12 + r.measure_text(text, h.text);
}

// ---- Home page pieces ---------------------------------------------------------------------------------------

class ShortcutTile : public wnk::ui::View {
 public:
    ShortcutTile(HomeData::Item item, std::function<void(Ui &, const std::string &)> open)
        : item_(std::move(item)), open_(std::move(open)) {
        set_focusable(true);
        set_focus_ring(false);
        accessible_label = item_.title;
    }

 protected:
    Size measure_content(Ui &, Size available) override { return {available.w, 171}; }
    bool on_action(Ui &ui, const wnk::input::ActionEvent &e) override {
        if (!is_confirm(e)) return false;
        open_(ui, item_.url);
        return true;
    }
    void draw(Ui &ui) override {
        auto &r = ui.renderer();
        const Rect f = frame();
        const bool focused = ui.is_focused(this);
        draw_glass(ui, f, 36);
        if (focused) {
            r.fill_rect(f, ui.fade(kWhite.with_alpha(0.10f)), 36);
            draw_focus(ui, f, 36);
        }
        const float d = 88;
        draw_tile_mark(ui, {f.cx() - d * 0.5f, f.y + 26, d, d}, item_.url, item_.title, 38);
        const TextStyle s = style(fonts().semibold, 22, ui.fade(kText));
        draw_text_centered(ui, s, {f.x + 8, f.y + 26 + d + 12, f.w - 16, 32}, ellipsize(r, s, item_.title, f.w - 20));
    }

 private:
    HomeData::Item item_;
    std::function<void(Ui &, const std::string &)> open_;
};

class RecentCard : public wnk::ui::View {
 public:
    RecentCard(HomeData::Item item, std::function<void(Ui &, const std::string &)> open)
        : item_(std::move(item)), open_(std::move(open)) {
        set_focusable(true);
        set_focus_ring(false);
        accessible_label = item_.title;
    }

 protected:
    Size measure_content(Ui &, Size available) override { return {available.w, 108}; }
    bool on_action(Ui &ui, const wnk::input::ActionEvent &e) override {
        if (!is_confirm(e)) return false;
        open_(ui, item_.url);
        return true;
    }
    void draw(Ui &ui) override {
        auto &r = ui.renderer();
        const Rect f = frame();
        draw_glass(ui, f, 30);
        if (ui.is_focused(this)) {
            r.fill_rect(f, ui.fade(kWhite.with_alpha(0.10f)), 30);
            draw_focus(ui, f, 30);
        }
        const Rect thumb{f.x + 18, f.y + 18, 120, f.h - 36};
        const Color c = tile_color(item_.url, item_.title);
        r.fill_rect(thumb, ui.fade(c.with_alpha(0.28f)), 18);
        r.stroke_rect(thumb, ui.fade(kWhite.with_alpha(0.10f)), 1.5f, 18);
        draw_tile_mark(ui, {thumb.cx() - 22, thumb.cy() - 22, 44, 44}, item_.url, item_.title, 20);
        const float x = thumb.right() + 20, w = f.right() - 20 - x;
        const TextStyle t = style(fonts().semibold, 21, ui.fade(kText)), h = style(fonts().regular, 18, ui.fade(kMuted));
        r.draw_text(t, {x, f.y + 40}, ellipsize(r, t, item_.title.empty() ? host_of(item_.url) : item_.title, w));
        r.draw_text(h, {x, f.y + 74}, ellipsize(r, h, host_of(item_.url), w));
    }

 private:
    HomeData::Item item_;
    std::function<void(Ui &, const std::string &)> open_;
};

// ---- overlay pieces ----------------------------------------------------------------------------------------

class TabCard : public wnk::ui::View {
 public:
    TabCard(TabInfo info, bool active, TabHandlers h) : info_(std::move(info)), active_(active), h_(std::move(h)) {
        set_focusable(true);
        set_focus_ring(false);
        accessible_label = info_.title.empty() ? "New tab" : info_.title;
        const uint32_t id = info_.id;
        auto close = h_.close;
        close_ = &add<GlassButton>("close", "", [close, id](Ui &) { close(id); }, 60);
        close_->accessible_label = "Close tab";
    }

 protected:
    Size measure_content(Ui &, Size available) override { return {available.w, 330}; }
    void layout_content(Ui &ui, const Rect &c) override { close_->layout(ui, {c.right() - 60 - 18, c.bottom() - 60 - 21, 60, 60}); }
    bool on_action(Ui &ui, const wnk::input::ActionEvent &e) override {
        if (!is_confirm(e) || ui.is_focused(close_)) return false;
        h_.activate(info_.id);
        return true;
    }
    void draw(Ui &ui) override {
        auto &r = ui.renderer();
        const Rect f = frame();
        draw_glass(ui, f, 39);
        const bool hot = ui.is_focused(this);
        if (hot) {
            r.fill_rect(f, ui.fade(kWhite.with_alpha(0.08f)), 39);
            draw_focus(ui, f, 39);
        }
        const Rect preview{f.x + 15, f.y + 15, f.w - 30, 222};
        const Color c = info_.url.empty() ? kAccent : tile_color(info_.url, info_.title);
        r.fill_rect(preview, ui.fade(c.with_alpha(0.20f)), 27);
        r.stroke_rect(preview, ui.fade(kWhite.with_alpha(0.08f)), 1.5f, 27);
        if (info_.url.empty())
            draw_text_centered(ui, style(fonts().light, 40, ui.fade(kText)), preview, "New tab");
        else
            draw_tile_mark(ui, {preview.cx() - 48, preview.cy() - 48, 96, 96}, info_.url, info_.title, 42);
        if (active_) {
            const TextStyle s = style(fonts().semibold, 18, ui.fade(kText));
            const float w = r.measure_text(s, "SHOWN") + 32;
            const Rect badge{preview.x + 18, preview.y + 18, w, 42};
            draw_glass(ui, badge, 21, Glass::Dark, false);
            r.fill_rect({badge.x + 14, badge.cy() - 5, 10, 10}, ui.fade(kSecure), 5);
            r.draw_text(s, {badge.x + 30, baseline(r, s, badge)}, "SHOWN");
        }
        const Rect mark{f.x + 27, f.bottom() - 30 - 51 - 6, 51, 51};
        if (!info_.url.empty()) draw_tile_mark(ui, mark, info_.url, info_.title, 22);
        const float x = info_.url.empty() ? f.x + 27 : mark.right() + 18, w = f.right() - 96 - x;
        const TextStyle t = style(fonts().semibold, 24, ui.fade(kText)), h = style(fonts().regular, 19, ui.fade(kMuted));
        const std::string title = info_.title.empty() ? (info_.url.empty() ? "New tab" : host_of(info_.url)) : info_.title;
        r.draw_text(t, {x, f.bottom() - 60}, ellipsize(r, t, title, w));
        r.draw_text(h, {x, f.bottom() - 30}, ellipsize(r, h, info_.url.empty() ? "Wren" : host_of(info_.url), w));
    }

 private:
    TabInfo info_;
    bool active_;
    TabHandlers h_;
    GlassButton *close_;
};

// A row in the side panel's list: mark, title, host, chevron.
class PanelRow : public wnk::ui::View {
 public:
    PanelRow(std::string title, std::string url, std::string sub, wnk::ui::ClickHandler on_click, std::string icon = {})
        : title_(std::move(title)), url_(std::move(url)), sub_(std::move(sub)), on_click_(std::move(on_click)) {
        set_focusable(true);
        set_focus_ring(false);
        set_height(wnk::ui::Dim::fixed(90));
        accessible_label = title_;
        if (!icon.empty()) {
            icon_ = &add<wnk::ui::ImageView>(icon_url(icon));
            icon_->set_fit(wnk::ui::ImageFit::Contain);
        }
    }

 protected:
    void layout_content(Ui &ui, const Rect &c) override {
        if (icon_ != nullptr) icon_->layout(ui, {c.x + 24 + 6, c.cy() - 22, 44, 44});
    }
    bool on_action(Ui &ui, const wnk::input::ActionEvent &e) override {
        if (!is_confirm(e)) return false;
        if (on_click_) on_click_(ui);
        return true;
    }
    void draw(Ui &ui) override {
        auto &r = ui.renderer();
        const Rect f = frame();
        if (ui.is_focused(this)) {
            r.fill_rect(f, ui.fade(kWhite.with_alpha(0.12f)), 27);
            r.stroke_rect(f, ui.fade(kWhite.with_alpha(0.18f)), 1.5f, 27);
            draw_focus(ui, f, 27);
        }
        if (icon_ == nullptr) draw_tile_mark(ui, {f.x + 24, f.cy() - 28, 56, 56}, url_, title_, 24);
        const float x = f.x + 24 + 56 + 21, w = f.right() - 60 - x;
        const TextStyle t = style(fonts().semibold, 24, ui.fade(kText)), h = style(fonts().regular, 19, ui.fade(kMuted));
        if (sub_.empty()) {
            r.draw_text(t, {x, baseline(r, t, f)}, ellipsize(r, t, title_, w));
        } else {
            r.draw_text(t, {x, f.y + 41}, ellipsize(r, t, title_, w));
            r.draw_text(h, {x, f.y + 70}, ellipsize(r, h, sub_, w));
        }
    }

 private:
    std::string title_, url_, sub_;
    wnk::ui::ClickHandler on_click_;
    wnk::ui::ImageView *icon_ = nullptr;
};

// A section button in the side panel's rail: icon over a short label.
class RailButton : public wnk::ui::View {
 public:
    RailButton(std::string icon, std::string label, bool selected, wnk::ui::ClickHandler on_click)
        : label_(std::move(label)), selected_(selected), on_click_(std::move(on_click)) {
        set_focusable(true);
        set_focus_ring(false);
        set_size(102, 99);
        accessible_label = label_;
        image_ = &add<wnk::ui::ImageView>(icon_url(icon));
        image_->set_fit(wnk::ui::ImageFit::Contain);
    }

 protected:
    void layout_content(Ui &ui, const Rect &c) override { image_->layout(ui, {c.cx() - 17, c.y + 18, 34, 34}); }
    bool on_action(Ui &ui, const wnk::input::ActionEvent &e) override {
        if (!is_confirm(e)) return false;
        if (on_click_) on_click_(ui);
        return true;
    }
    void draw(Ui &ui) override {
        auto &r = ui.renderer();
        const Rect f = frame();
        if (selected_ || ui.is_focused(this)) {
            r.fill_rect(f, ui.fade(kWhite.with_alpha(0.12f)), 30);
            r.stroke_rect(f, ui.fade(kWhite.with_alpha(0.18f)), 1.5f, 30);
        }
        if (ui.is_focused(this)) draw_focus(ui, f, 30);
        draw_text_centered(ui, style(fonts().semibold, 17, ui.fade(selected_ ? kAccent : kSoft)),
                           {f.x, f.y + 60, f.w, 28}, label_);
    }

 private:
    std::string label_;
    bool selected_;
    wnk::ui::ClickHandler on_click_;
    wnk::ui::ImageView *image_;
};

// A big square action in the quick actions grid.
class ActionTile : public wnk::ui::View {
 public:
    ActionTile(std::string icon, std::string label, wnk::ui::ClickHandler on_click)
        : label_(std::move(label)), on_click_(std::move(on_click)) {
        set_focusable(true);
        set_focus_ring(false);
        accessible_label = label_;
        image_ = &add<wnk::ui::ImageView>(icon_url(icon));
        image_->set_fit(wnk::ui::ImageFit::Contain);
    }

 protected:
    Size measure_content(Ui &, Size available) override { return {available.w, 156}; }
    void layout_content(Ui &ui, const Rect &c) override { image_->layout(ui, {c.cx() - 21, c.y + 36, 42, 42}); }
    bool on_action(Ui &ui, const wnk::input::ActionEvent &e) override {
        if (!is_confirm(e)) return false;
        if (on_click_) on_click_(ui);
        return true;
    }
    void draw(Ui &ui) override {
        auto &r = ui.renderer();
        const Rect f = frame();
        const bool focused = ui.is_focused(this);
        r.fill_rect(f, ui.fade(kWhite.with_alpha(focused ? 0.14f : 0.05f)), 33);
        r.stroke_rect(f, ui.fade(kWhite.with_alpha(focused ? 0.20f : 0.08f)), 1.5f, 33);
        if (focused) draw_focus(ui, f, 33);
        draw_text_centered(ui, style(fonts().semibold, 21, ui.fade(kText)), {f.x + 6, f.y + 96, f.w - 12, 34}, label_);
    }

 private:
    std::string label_;
    wnk::ui::ClickHandler on_click_;
    wnk::ui::ImageView *image_;
};

}  // namespace

// ---- shared -------------------------------------------------------------------------------------------------

Fonts &fonts() {
    static Fonts f;
    return f;
}

void load_extra_fonts(wnk::render::Renderer &r, const std::string &app_dir) {
    std::string err;
    Fonts &f = fonts();
    f.light = r.load_font_file(app_dir + "/fonts/Manrope-Light.ttf", &err);
    if (f.light == wnk::render::kNoFont) WNK_LOGW("wren", "Manrope Light: %s", err.c_str());
    f.bold = r.load_font_file(app_dir + "/fonts/Manrope-Bold.ttf", &err);
    if (f.bold == wnk::render::kNoFont) WNK_LOGW("wren", "Manrope Bold: %s", err.c_str());
    if (f.light == wnk::render::kNoFont) f.light = f.regular;
    if (f.bold == wnk::render::kNoFont) f.bold = f.semibold;
}

std::function<std::string(const std::string &)> &icon_lookup() {
    static std::function<std::string(const std::string &)> f;
    return f;
}

bool &colour_keys_seen() {
    static bool seen = false;
    return seen;
}

void draw_ambient(Ui &ui, const Rect &r) { ui.renderer().draw_image(ambient_texture(ui.renderer()), r, ui.fade(kWhite)); }

void draw_glass(Ui &ui, const Rect &f, float radius, Glass kind, bool shadow) {
    auto &r = ui.renderer();
    if (shadow) {
        const float steps[3][3] = {{4, 10, 0.06f}, {12, 20, 0.04f}, {22, 32, 0.025f}};  // spread, drop, alpha
        for (const auto &s : steps)
            r.fill_rect({f.x - s[0], f.y - s[0] + s[1], f.w + 2 * s[0], f.h + 2 * s[0]}, ui.fade(kBlack.with_alpha(s[2])),
                        radius + s[0]);
    }
    if (kind == Glass::Light)
        r.fill_rect(f, ui.fade(kWhite.with_alpha(0.07f)), radius);
    else
        r.fill_rect(f, ui.fade(Color::hex(0x0C1018FF).with_alpha(0.84f)), radius);
    r.stroke_rect(f, ui.fade(kWhite.with_alpha(kind == Glass::Light ? 0.13f : 0.15f)), 1.5f, radius);
    // the top highlight
    const float inset = std::max(radius * 0.6f, 12.0f);
    if (f.w > 2 * inset) r.fill_rect({f.x + inset, f.y + 1.5f, f.w - 2 * inset, 1.5f}, ui.fade(kWhite.with_alpha(0.16f)));
}

void draw_focus(Ui &ui, const Rect &f, float radius) {
    auto &r = ui.renderer();
    r.stroke_rect({f.x - 8, f.y - 8, f.w + 16, f.h + 16}, ui.fade(kAccent.with_alpha(0.32f)), 8, radius + 8);
    r.stroke_rect({f.x - 3, f.y - 3, f.w + 6, f.h + 6}, ui.fade(kWhite.with_alpha(0.92f)), 3, radius + 3);
}

Color tile_color(const std::string &url, const std::string &title) {
    const std::string host = host_of(url);
    const std::string &key = host.empty() ? title : host;
    uint32_t h = 2166136261u;  // FNV-1a: a stable colour per site
    for (unsigned char c : key) h = (h ^ c) * 16777619u;
    return kTileColors[h % (sizeof(kTileColors) / sizeof(kTileColors[0]))];
}

void draw_text_centered(Ui &ui, const TextStyle &s, const Rect &box, const std::string &text) {
    auto &r = ui.renderer();
    const float w = r.measure_text(s, text);
    r.draw_text(s, {box.cx() - w * 0.5f, baseline(r, s, box)}, text);
}

void draw_tile_mark(Ui &ui, const Rect &box, const std::string &url, const std::string &title, float text_size) {
    auto &r = ui.renderer();
    if (!url.empty() && icon_lookup()) {
        if (const auto tex = icon_texture(ui, icon_lookup()(url)); tex != wnk::render::kNoTexture) {
            // On a light tile: icons are made for light browser tabs, and many are dark on transparent.
            r.fill_rect(box, ui.fade(Color::hex(0xF4F6FAFF)), box.w * 0.33f);
            const float inset = box.w * 0.17f;
            r.draw_image(tex, {box.x + inset, box.y + inset, box.w - 2 * inset, box.h - 2 * inset}, ui.fade(kWhite));
            return;
        }
    }
    const Monogram m = monogram_for(url, title);
    r.fill_rect(box, ui.fade(tile_color(url, title)), box.w * 0.33f);
    r.fill_rect({box.x + box.w * 0.25f, box.y + 1, box.w * 0.5f, 1.5f}, ui.fade(kWhite.with_alpha(0.5f)));
    draw_text_centered(ui, style(fonts().bold, text_size, ui.fade(kOnAccent)), box, m.letter);
}

// ---- HintPill -----------------------------------------------------------------------------------------------

void HintPill::set_hints(std::vector<Hint> hints) {
    hints_ = std::move(hints);
    invalidate();
}

Size HintPill::measure_content(Ui &ui, Size) {
    float w = 36;
    for (size_t i = 0; i < hints_.size(); ++i) w += hint_width(ui.renderer(), hints_[i]) + (i ? 39 : 0);
    return {w, 69};
}

void HintPill::draw(Ui &ui) {
    if (hints_.empty()) return;
    auto &r = ui.renderer();
    const Rect f = frame();
    draw_glass(ui, f, f.h * 0.5f, kind_);
    const TextStyle cap = style(fonts().semibold, 20, ui.fade(kText)), text = style(fonts().regular, 21, ui.fade(kSoft));
    float x = f.x + 18;
    for (const Hint &h : hints_) {
        if (h.key.empty()) {
            r.fill_rect({x, f.cy() - 9, 18, 18}, ui.fade(h.dot), 9);
            x += 18;
        } else {
            const float w = r.measure_text(cap, h.key) + 24;
            const Rect box{x, f.cy() - 18, w, 36};
            r.fill_rect(box, ui.fade(kWhite.with_alpha(0.06f)), 10);
            r.stroke_rect(box, ui.fade(kWhite.with_alpha(0.24f)), 1.5f, 10);
            draw_text_centered(ui, cap, box, h.key);
            x += w;
        }
        x += 12;
        x += r.draw_text(text, {x, baseline(r, text, f)}, h.text) + 39;
    }
}

// ---- GlassButton --------------------------------------------------------------------------------------------

GlassButton::GlassButton(std::string icon, std::string label, wnk::ui::ClickHandler on_click, float height, bool primary)
    : label_(std::move(label)), on_click_(std::move(on_click)), height_(height), primary_(primary) {
    set_focusable(true);
    set_focus_ring(false);
    accessible_label = label_;
    if (!icon.empty()) {
        image_ = &add<wnk::ui::ImageView>(icon_url(icon));
        image_->set_fit(wnk::ui::ImageFit::Contain);
    }
}

void GlassButton::set_label(std::string label) {
    label_ = std::move(label);
    accessible_label = label_;
    invalidate();
}

Size GlassButton::measure_content(Ui &ui, Size) {
    if (label_.empty() || label_ == "+" || label_ == "−") return {height_, height_};
    const float text = ui.renderer().measure_text(style(fonts().semibold, height_ * 0.33f, kText), label_);
    return {(image_ != nullptr ? height_ * 0.33f + height_ * 0.42f + 12 : height_ * 0.38f) + text + height_ * 0.38f, height_};
}

void GlassButton::layout_content(Ui &ui, const Rect &c) {
    if (image_ == nullptr) return;
    const float s = std::round(height_ * 0.42f);
    const float x = label_.empty() ? c.cx() - s * 0.5f : c.x + height_ * 0.33f;
    image_->layout(ui, {x, c.cy() - s * 0.5f, s, s});
}

bool GlassButton::on_action(Ui &ui, const wnk::input::ActionEvent &e) {
    if (!is_confirm(e)) return false;
    if (on_click_) on_click_(ui);
    return true;
}

void GlassButton::draw(Ui &ui) {
    auto &r = ui.renderer();
    const Rect f = frame();
    const bool focused = ui.is_focused(this);
    const float rad = f.h * 0.5f;
    if (primary_) {
        r.fill_rect(f, ui.fade(focused ? kText : kAccent), rad);
    } else {
        r.fill_rect(f, ui.fade(kWhite.with_alpha(focused ? 0.16f : 0.06f)), rad);
        r.stroke_rect(f, ui.fade(kWhite.with_alpha(focused ? 0.22f : 0.14f)), 1.5f, rad);
    }
    if (focused) draw_focus(ui, f, rad);
    if (label_.empty()) return;
    const bool glyph = label_ == "+" || label_ == "−";  // zoom steps: a large sign
    const TextStyle s = style(glyph ? fonts().regular : fonts().semibold, height_ * (glyph ? 0.55f : 0.33f),
                              ui.fade(primary_ ? kOnAccent : kText));
    if (glyph) {
        draw_text_centered(ui, s, f, label_);
        return;
    }
    const float x = image_ != nullptr ? f.x + height_ * 0.33f + height_ * 0.42f + 12 : f.x + height_ * 0.38f;
    r.draw_text(s, {x, baseline(r, s, f)}, label_);
}

// ---- TabChip / TabRow ---------------------------------------------------------------------------------------

TabChip::TabChip(TabInfo info, bool active, bool compact, std::function<void(uint32_t)> on_activate)
    : info_(std::move(info)), active_(active), compact_(compact), on_activate_(std::move(on_activate)) {
    set_focusable(true);
    set_focus_ring(false);
    accessible_label = info_.title.empty() ? (info_.url.empty() ? "New tab" : host_of(info_.url)) : info_.title;
}

Size TabChip::measure_content(Ui &ui, Size available) {
    const float h = compact_ ? 63 : 66;
    if (compact_ && !active_) return {h, h};
    if (compact_) {
        const TextStyle s = style(fonts().semibold, 21, kText);
        return {12 + 42 + 12 + ui.renderer().measure_text(s, accessible_label) + 21, h};
    }
    return {available.w, h};
}

bool TabChip::on_action(Ui &, const wnk::input::ActionEvent &e) {
    if (!is_confirm(e)) return false;
    on_activate_(info_.id);
    return true;
}

void TabChip::draw(Ui &ui) {
    auto &r = ui.renderer();
    const Rect f = frame();
    const float rad = f.h * 0.5f;
    const bool focused = ui.is_focused(this);
    if (active_) {
        r.fill_rect(f, ui.fade(kWhite.with_alpha(0.16f)), rad);
        r.stroke_rect(f, ui.fade(kWhite.with_alpha(0.22f)), 1.5f, rad);
    } else if (focused) {
        r.fill_rect(f, ui.fade(kWhite.with_alpha(0.12f)), rad);
    }
    if (focused) draw_focus(ui, f, rad);
    const float d = compact_ ? 39 : 45;
    const Rect mark{compact_ && !active_ ? f.cx() - d * 0.5f : f.x + 11, f.cy() - d * 0.5f, d, d};
    if (info_.url.empty()) {
        r.fill_rect(mark, ui.fade(kWhite.with_alpha(0.85f)), d * 0.33f);
        draw_text_centered(ui, style(fonts().bold, d * 0.5f, ui.fade(kOnAccent)), mark, "+");
    } else {
        draw_tile_mark(ui, mark, info_.url, info_.title, d * 0.44f);
    }
    if (info_.loading) {
        const float a = 0.45f + 0.45f * std::sin(static_cast<float>(ui.now_ms()) / 180.0f);
        r.stroke_rect(mark.inset(-4), ui.fade(kAccent.with_alpha(a)), 3, d * 0.33f + 4);
    }
    if (compact_ && !active_) return;
    const TextStyle s = style(fonts().semibold, compact_ ? 21 : 22, ui.fade(active_ || focused ? kText : kSoft));
    const float x = mark.right() + 12;
    r.draw_text(s, {x, baseline(r, s, f)}, ellipsize(r, s, accessible_label, f.right() - 21 - x));
}

TabRow::TabRow(TabHandlers h) : Row(12), h_(std::move(h)) {
    name = "tabrow";
    set_width(wnk::ui::Dim::fill());
    set_height(wnk::ui::Dim::fixed(66));
}

void TabRow::rebuild(Ui &, const std::vector<TabInfo> &tabs, uint32_t active) {
    clear_children();
    const float available = 1800 - 30 - (66 + 12) - (240 + 12) - 12;
    const float w = std::clamp(available / static_cast<float>(std::max<size_t>(tabs.size(), 1)) - 12, 150.0f, 390.0f);
    for (const TabInfo &t : tabs) add<TabChip>(t, t.id == active, false, h_.activate).set_width(wnk::ui::Dim::fixed(w));
    add<GlassButton>("add", "", [this](Ui &) { h_.new_tab(); }, 66).accessible_label = "New tab";
    add<wnk::ui::View>().set_width(wnk::ui::Dim::fill());
    add<GlassButton>("tabs", "All tabs", [this](Ui &) { h_.show_all(); }, 66);
}

TabChip *TabRow::chip(uint32_t id) {
    for (auto &c : children())
        if (auto *chip = dynamic_cast<TabChip *>(c.get()); chip != nullptr && chip->tab_id() == id) return chip;
    return nullptr;
}

// ---- ChromePanel --------------------------------------------------------------------------------------------

ChromePanel::ChromePanel() : Column(12) {
    name = "chromepanel";
    set_padding({15, 15, 15, 15});
    set_width(wnk::ui::Dim::fill());
}

void ChromePanel::set_progress(bool loading, float value) {
    if (!loading_ && loading) shown_ = 0.05f;
    loading_ = loading;
    target_ = std::max(value, 0.05f);
}

void ChromePanel::draw(Ui &ui) { draw_glass(ui, frame(), 45, Glass::Dark); }

void ChromePanel::draw_over(Ui &ui) {
    if (!loading_) return;
    auto &r = ui.renderer();
    const Rect f = frame();
    shown_ += (target_ - shown_) * 0.25f;
    const Rect track{f.x + 54, f.bottom() - 4, f.w - 108, 3};
    r.fill_rect(track, ui.fade(kWhite.with_alpha(0.10f)), 1.5f);
    const float w = track.w * std::clamp(shown_, 0.0f, 1.0f);
    r.fill_rect({track.x, track.y - 3, w, 9}, ui.fade(kAccent.with_alpha(0.25f)), 4.5f);
    r.fill_rect({track.x, track.y, w, 3}, ui.fade(kAccent), 1.5f);
}

// ---- HomePage -----------------------------------------------------------------------------------------------

HomePage::HomePage(HomeData data, TabHandlers tabs, std::function<void(Ui &, const std::string &)> open,
                   wnk::ui::ClickHandler menu)
    : open_(std::move(open)), tabs_h_(std::move(tabs)) {
    name = "home";
    mark_ = &add<wnk::ui::ImageView>(icon_dir_url() + "wren-mark.png");
    mark_->set_fit(wnk::ui::ImageFit::Contain);
    name_ = &add<wnk::ui::Label>("Wren");
    name_->set_font_size(34).set_bold(true);
    tabs_ = &add<wnk::ui::Row>(6);
    tabs_->set_padding({6, 6, 6, 6});
    menu_ = &add<IconButton>("menu", "Menu", std::move(menu), 69);
    clock_ = &add<wnk::ui::Label>(clock_now());
    clock_->set_font_size(39);
    search_label_ = &add<wnk::ui::Label>("SEARCH OR ENTER AN ADDRESS", wnk::ui::TextRole::Small);
    search_label_->set_font_size(18).set_bold(true).set_color(kMuted);
    search_ = &add<Omnibox>(open_, "Search or type a URL", 111, kWhite.with_alpha(0.07f), kWhite.with_alpha(0.13f));
    search_->field().name = "ntp-search";
    tiles_ = &add<wnk::ui::Grid>(8, 27);
    for (auto &s : data.shortcuts) tiles_->add<ShortcutTile>(s, open_);
    recent_label_ = &add<wnk::ui::Label>("CONTINUE BROWSING", wnk::ui::TextRole::Small);
    recent_label_->set_font_size(18).set_bold(true).set_color(kMuted);
    recents_ = &add<wnk::ui::Grid>(4, 27);
    hints_ = &add<HintPill>();
    set_recents(std::move(data.recents));
    refresh_hints();
}

void HomePage::set_tabs(const std::vector<TabInfo> &tabs, uint32_t active) {
    tabs_->clear_children();
    for (const TabInfo &t : tabs) tabs_->add<TabChip>(t, t.id == active, true, tabs_h_.activate);
    invalidate();
}

void HomePage::set_recents(std::vector<HomeData::Item> recents) {
    recents_->clear_children();
    if (recents.size() > 4) recents.resize(4);
    for (auto &r : recents) recents_->add<RecentCard>(r, open_);
    recent_label_->set_visible(!recents.empty());
    invalidate();
}

void HomePage::refresh_hints() {
    if (colour_keys_seen())
        hints_->set_hints({{"◀ ▶ ▲ ▼", {}, "Move"}, {"OK", {}, "Open"}, {"", kRed, "Tabs"}, {"", kGreen, "Bookmarks"},
                           {"BACK", {}, "Exit"}});
    else
        hints_->set_hints({{"◀ ▶ ▲ ▼", {}, "Move"}, {"OK", {}, "Open"}, {"Hold OK", {}, "More"}, {"CH ±", {}, "Tabs"},
                           {"BACK", {}, "Exit"}});
}

void HomePage::show_hints(bool on) {
    if (hints_->visible() != on) hints_->set_visible(on);
}

void HomePage::layout_content(Ui &ui, const Rect &c) {
    const float left = c.x + 96, right = c.right() - 96, top = c.y + 51;
    mark_->layout(ui, {left, top + 7, 57, 57});
    const Size ns = name_->measure(ui, {400, 72});
    name_->layout(ui, {left + 57 + 18, top + (72 - ns.h) * 0.5f, ns.w, ns.h});
    const Size cs = clock_->measure(ui, {300, 72});
    clock_->layout(ui, {right - cs.w, top + (72 - cs.h) * 0.5f, cs.w, cs.h});
    const float menu_x = right - cs.w - 24 - 69;
    menu_->layout(ui, {menu_x, top + 1.5f, 69, 69});
    const Size ts = tabs_->measure(ui, {1000, 75});
    tabs_->layout(ui, {menu_x - 18 - ts.w, top - 1.5f, ts.w, 75});

    const float sw = std::min(1140.0f, c.w - 200);
    const Size ls = search_label_->measure(ui, {sw, 40});
    search_label_->layout(ui, {c.cx() - ls.w * 0.5f, top + 72 + 63, ls.w, ls.h});
    search_->layout(ui, {c.cx() - sw * 0.5f, top + 72 + 63 + ls.h + 18, sw, 111});
    const float tiles_y = top + 72 + 63 + ls.h + 18 + 111 + 66;
    const Size gs = tiles_->measure(ui, {right - left, 400});
    tiles_->layout(ui, {left, tiles_y, right - left, gs.h});
    const float recent_y = tiles_y + gs.h + 51;
    const Size rl = recent_label_->measure(ui, {600, 40});
    recent_label_->layout(ui, {left, recent_y, rl.w, rl.h});
    const Size rs = recents_->measure(ui, {right - left, 200});
    recents_->layout(ui, {left, recent_y + rl.h + 18, right - left, rs.h});
    const Size hs = hints_->measure(ui, {c.w, 69});
    hints_->layout(ui, {c.cx() - hs.w * 0.5f, c.bottom() - 33 - 69, hs.w, 69});
}

void HomePage::update(Ui &ui, float dt) {
    View::update(ui, dt);
    const std::string now = clock_now();
    if (now != clock_->text()) clock_->set_text(now);
}

void HomePage::draw(Ui &ui) {
    draw_ambient(ui, frame());
    if (!tabs_->children().empty()) draw_glass(ui, tabs_->frame(), tabs_->frame().h * 0.5f);
}

// ---- Overlay ------------------------------------------------------------------------------------------------

Overlay::Overlay() {
    set_width(wnk::ui::Dim::fill()).set_height(wnk::ui::Dim::fill());
    set_focus_scope(true, false);
    set_visible(false);
}

void Overlay::close(Ui &ui) {
    if (!visible()) return;
    set_visible(false);
    if (on_close) on_close(ui);
}

bool Overlay::on_action(Ui &ui, const wnk::input::ActionEvent &e) {
    if (e.action == wnk::input::Action::Back && e.phase == wnk::input::Phase::Press && visible()) {
        close(ui);
        return true;
    }
    return false;
}

// ---- TabOverview --------------------------------------------------------------------------------------------

TabOverview::TabOverview(TabHandlers h) : h_(std::move(h)) {
    name = "taboverview";
    title_ = &add<wnk::ui::Label>("Tabs");
    title_->set_font_size(48).set_bold(true);
    count_ = &add<wnk::ui::Label>("", wnk::ui::TextRole::Small);
    count_->set_font_size(18).set_bold(true).set_color(kMuted);
    buttons_ = &add<wnk::ui::Row>(18);
    buttons_->add<GlassButton>("add-ink", "New tab", [this](Ui &) { h_.new_tab(); }, 75, true);
    buttons_->add<GlassButton>("", "Close all", [this](Ui &) { h_.close_all(); }, 75);
    grid_ = &add<wnk::ui::Grid>(4, 36);
    hints_ = &add<HintPill>();
}

void TabOverview::rebuild(Ui &ui, const std::vector<TabInfo> &tabs, uint32_t active) {
    grid_->clear_children();
    TabCard *shown = nullptr;
    for (const TabInfo &t : tabs) {
        auto &card = grid_->add<TabCard>(t, t.id == active, h_);
        if (t.id == active) shown = &card;
    }
    count_->set_text(std::to_string(tabs.size()) + (tabs.size() == 1 ? " OPEN" : " OPEN"));
    if (colour_keys_seen())
        hints_->set_hints({{"OK", {}, "Switch"}, {"", kRed, "Close tab"}, {"CH ±", {}, "Next / previous"}, {"BACK", {}, "Return"}});
    else
        hints_->set_hints({{"OK", {}, "Switch"}, {"▶", {}, "Close button"}, {"CH ±", {}, "Next / previous"}, {"BACK", {}, "Return"}});
    if (visible() && shown != nullptr) ui.focus(shown);
    invalidate();
}

void TabOverview::layout_content(Ui &ui, const Rect &c) {
    const float left = c.x + 96, right = c.right() - 96, top = c.y + 60;
    const Size ts = title_->measure(ui, {600, 80});
    title_->layout(ui, {left, top + (75 - ts.h) * 0.5f, ts.w, ts.h});
    const Size cs = count_->measure(ui, {300, 40});
    count_->layout(ui, {left + ts.w + 21, top + (75 - cs.h) * 0.5f + 6, cs.w, cs.h});
    const Size bs = buttons_->measure(ui, {900, 75});
    buttons_->layout(ui, {right - bs.w, top, bs.w, 75});
    const Size gs = grid_->measure(ui, {right - left, 900});
    grid_->layout(ui, {left, top + 75 + 54, right - left, gs.h});
    const Size hs = hints_->measure(ui, {c.w, 69});
    hints_->layout(ui, {c.cx() - hs.w * 0.5f, c.bottom() - 33 - 69, hs.w, 69});
}

void TabOverview::draw(Ui &ui) { draw_ambient(ui, frame()); }

// ---- SidePanel ----------------------------------------------------------------------------------------------

SidePanel::SidePanel(PanelHandlers h) : h_(std::move(h)) {
    name = "sidepanel";
    rail_ = &add<wnk::ui::Column>(12);
    body_ = &add<wnk::ui::Column>(9);
}

void SidePanel::show(Ui &ui, Section s, const std::vector<Bookmark> &bookmarks, const std::vector<HistoryEntry> &history) {
    section_ = s;
    bookmarks_ = bookmarks;
    history_ = history;
    fill(ui);
    set_visible(true);
    ui.focus_within(body_);
}

void SidePanel::refresh_zoom() {
    if (zoom_label_ != nullptr && h_.zoom_text) zoom_label_->set_text(h_.zoom_text());
}

void SidePanel::fill(Ui &ui) {
    rail_->clear_children();
    body_->clear_children();
    zoom_label_ = nullptr;
    struct R {
        const char *icon, *label;
        Section s;
    };
    const R sections[] = {{"bookmarks", "Saved", Section::Saved}, {"history", "History", Section::History}, {"zoom", "Zoom", Section::Zoom}};
    for (const R &r : sections) {
        const Section s = r.s;
        rail_->add<RailButton>(r.icon, r.label, s == section_, [this, s](Ui &u) {
            section_ = s;
            fill(u);
            u.focus_within(body_);
        });
    }
    rail_->add<RailButton>("settings", "Settings", false, [this](Ui &u) { h_.settings(u); });
    rail_->add<RailButton>("exit", "Exit", false, [this](Ui &u) { h_.exit(u); });

    auto caption = [&](const std::string &over, const std::string &title, const std::string &sub) {
        auto &o = body_->add<wnk::ui::Label>(over, wnk::ui::TextRole::Small);
        o.set_font_size(18).set_bold(true).set_color(kMuted);
        auto &t = body_->add<wnk::ui::Label>(title);
        t.set_font_size(45).set_bold(true);
        auto &s = body_->add<wnk::ui::Label>(sub, wnk::ui::TextRole::Small);
        s.set_font_size(21).set_color(kMuted).set_padding({0, 0, 0, 18});
    };
    auto open = h_.open;
    switch (section_) {
    case Section::Saved: {
        caption("LIBRARY", "Bookmarks",
                bookmarks_.empty() ? "Nothing saved yet · Hold OK on a page to bookmark it"
                                   : std::to_string(bookmarks_.size()) + " saved · Hold OK on a page to bookmark it");
        size_t n = 0;
        for (const Bookmark &b : bookmarks_) {
            if (++n > 6) break;
            const std::string url = b.url;
            body_->add<PanelRow>(b.title, b.url, host_of(b.url), [open, url](Ui &u) { open(u, url); });
        }
        if (bookmarks_.size() > 6 || !bookmarks_.empty())
            body_->add<PanelRow>("All bookmarks", "", "", h_.bookmarks_page, "chevron");
        break;
    }
    case Section::History: {
        caption("LIBRARY", "History", history_.empty() ? "No pages visited yet" : "Recently visited");
        size_t n = 0;
        for (const HistoryEntry &e : history_) {
            if (++n > 6) break;
            const std::string url = e.url;
            body_->add<PanelRow>(e.title.empty() ? host_of(e.url) : e.title, e.url, host_of(e.url),
                                 [open, url](Ui &u) { open(u, url); });
        }
        body_->add<PanelRow>("All history", "", "", h_.history_page, "chevron");
        break;
    }
    case Section::Zoom: {
        caption("PAGE", "Zoom", "Makes every page larger or smaller");
        auto &row = body_->add<wnk::ui::Row>(18);
        row.set_padding({0, 12, 0, 0});
        row.add<GlassButton>("", "−", [this](Ui &) { h_.zoom(-1); refresh_zoom(); }, 84).accessible_label = "Zoom out";
        zoom_label_ = &row.add<wnk::ui::Label>(h_.zoom_text ? h_.zoom_text() : "100%");
        zoom_label_->set_font_size(36).set_bold(true).set_text_align(wnk::ui::Align::Center).set_width(wnk::ui::Dim::fixed(180));
        zoom_label_->set_align(wnk::ui::Align::Start, wnk::ui::Align::Center);
        row.add<GlassButton>("", "+", [this](Ui &) { h_.zoom(+1); refresh_zoom(); }, 84).accessible_label = "Zoom in";
        break;
    }
    }
    invalidate();
    (void)ui;
}

void SidePanel::layout_content(Ui &ui, const Rect &c) {
    panel_ = {c.right() - 27 - 840, c.y + 27, 840, c.h - 54};
    rail_->layout(ui, {panel_.x + 21, panel_.y + 45, 102, panel_.h - 90});
    body_->layout(ui, {panel_.x + 144 + 36, panel_.y + 48, panel_.w - 144 - 36 - 39, panel_.h - 96});
}

void SidePanel::draw(Ui &ui) {
    auto &r = ui.renderer();
    r.fill_rect(frame(), ui.fade(kBlack.with_alpha(0.50f)));
    draw_glass(ui, panel_, 45, Glass::Dark);
    r.fill_rect({panel_.x + 144, panel_.y + 30, 1.5f, panel_.h - 60}, ui.fade(kWhite.with_alpha(0.08f)));
}

// ---- QuickActions -------------------------------------------------------------------------------------------

QuickActions::QuickActions(QuickHandlers h) : h_(std::move(h)) {
    name = "quickactions";
    grid_ = &add<wnk::ui::Grid>(4, 18);
    auto act = [this](const char *icon, const char *label, std::function<void(Ui &)> QuickHandlers::*f) {
        grid_->add<ActionTile>(icon, label, [this, f](Ui &u) {
            close(u);
            if (h_.*f) (h_.*f)(u);
        });
    };
    act("newtab", "New tab", &QuickHandlers::new_tab);
    act("star", "Bookmark", &QuickHandlers::bookmark);
    act("tabs", "Tabs", &QuickHandlers::tabs);
    act("reload", "Reload", &QuickHandlers::reload);
    act("close", "Close tab", &QuickHandlers::close_tab);
    act("history", "History", &QuickHandlers::history);
    act("home", "Home", &QuickHandlers::home);
    act("settings", "Settings", &QuickHandlers::settings);
    zoom_row_ = &add<wnk::ui::Row>(15);
    zoom_row_->add<GlassButton>("", "−", [this](Ui &) {
        h_.zoom(-1);
        zoom_label_->set_text(h_.zoom_text());
    }, 66).accessible_label = "Zoom out";
    zoom_label_ = &zoom_row_->add<wnk::ui::Label>("100%");
    zoom_label_->set_font_size(26).set_bold(true).set_text_align(wnk::ui::Align::Center).set_width(wnk::ui::Dim::fixed(105));
    zoom_label_->set_align(wnk::ui::Align::Start, wnk::ui::Align::Center);
    zoom_in_ = &zoom_row_->add<GlassButton>("", "+", [this](Ui &) {
        h_.zoom(+1);
        zoom_label_->set_text(h_.zoom_text());
    }, 66);
    zoom_in_->accessible_label = "Zoom in";
    hints_ = &add<HintPill>(Glass::Dark);
    hints_->set_hints({{"◀ ▶ ▲ ▼", {}, "Choose"}, {"OK", {}, "Do it"}, {"BACK", {}, "Close"}});
}

void QuickActions::show(Ui &ui, const std::string &title, const std::string &url, bool bookmarked, bool focus_zoom) {
    title_ = title.empty() ? (url.empty() ? "New tab" : host_of(url)) : title;
    url_ = url;
    if (h_.zoom_text) zoom_label_->set_text(h_.zoom_text());
    if (auto *star = dynamic_cast<ActionTile *>(grid_->children()[1].get())) star->accessible_label = bookmarked ? "Remove bookmark" : "Bookmark";
    set_visible(true);
    ui.focus(focus_zoom ? zoom_in_ : grid_->children().front().get());
}

void QuickActions::layout_content(Ui &ui, const Rect &c) {
    const float w = 1110;
    const Size gs = grid_->measure(ui, {w - 90, 600});
    const float h = 45 + 66 + 33 + gs.h + 27 + 87 + 42;
    panel_ = {c.cx() - w * 0.5f, c.y + (c.h - h) * 0.47f, w, h};
    grid_->layout(ui, {panel_.x + 45, panel_.y + 45 + 66 + 33, w - 90, gs.h});
    const Size zs = zoom_row_->measure(ui, {600, 66});
    zoom_row_->layout(ui, {panel_.right() - 45 - 12 - zs.w, grid_->frame().bottom() + 27 + 10.5f, zs.w, 66});
    const Size hs = hints_->measure(ui, {c.w, 69});
    hints_->layout(ui, {c.cx() - hs.w * 0.5f, c.bottom() - 39 - 69, hs.w, 69});
}

void QuickActions::draw(Ui &ui) {
    auto &r = ui.renderer();
    r.fill_rect(frame(), ui.fade(kBlack.with_alpha(0.40f)));
    draw_glass(ui, panel_, 48, Glass::Dark);
    const Rect mark{panel_.x + 45, panel_.y + 45, 66, 66};
    if (url_.empty()) {
        r.fill_rect(mark, ui.fade(kWhite.with_alpha(0.85f)), 22);
        draw_text_centered(ui, style(fonts().bold, 33, ui.fade(kOnAccent)), mark, "+");
    } else {
        draw_tile_mark(ui, mark, url_, title_, 29);
    }
    const float x = mark.right() + 21, w = panel_.right() - 45 - x;
    const TextStyle t = style(fonts().semibold, 30, ui.fade(kText)), u = style(fonts().regular, 21, ui.fade(kMuted));
    r.draw_text(t, {x, mark.y + 31}, ellipsize(r, t, title_, w));
    if (!url_.empty()) r.draw_text(u, {x, mark.y + 60}, ellipsize(r, u, url_.rfind("https://", 0) == 0 ? url_.substr(8) : url_, w));
    const Rect zoom{panel_.x + 45, grid_->frame().bottom() + 27, panel_.w - 90, 87};
    r.fill_rect(zoom, ui.fade(kWhite.with_alpha(0.06f)), 30);
    r.stroke_rect(zoom, ui.fade(kWhite.with_alpha(0.10f)), 1.5f, 30);
    const TextStyle cap = style(fonts().semibold, 18, ui.fade(kSoft));
    r.draw_text(cap, {zoom.x + 30, baseline(r, cap, zoom)}, "PAGE ZOOM");
}

}  // namespace browser::wren
