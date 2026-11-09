// SPDX-License-Identifier: GPL-3.0-or-later
// Shows the engine's frames: a GlView that keeps the page in one GLES2 texture, updated from the shared
// frame slots (damaged rows only), drawn scaled into the view. While a field in the page is being edited it
// holds the kit's text input, so the TV keyboard's text and editing keys reach the page.
#pragma once

#include "engine_client.hpp"
#include "wnk/ui/widgets.hpp"

#include <GLES2/gl2.h>

namespace browser {

class PageView : public wnk::ui::GlView {
 public:
    explicit PageView(EngineClient &engine);
    ~PageView() override;

    // Page size in CSS px the engine renders at (the texture is scaled to the view).
    void set_page_size(uint32_t w, uint32_t h) { page_w_ = w, page_h_ = h; }
    uint32_t page_width() const { return page_w_; }
    uint32_t page_height() const { return page_h_; }
    // Maps a point in logical UI units to page CSS px; false if outside the view.
    bool to_page(wnk::Point logical, int32_t &x, int32_t &y) const;

    // An editable in the page gained/lost focus: open/close the TV keyboard.
    void ime(wnk::ui::Ui &ui, bool focused);
    bool editing() const { return editing_; }
    bool has_frame() const { return tex_w_ > 0; }
    // Frames from other tabs are dropped; switching shows a blank page until the new tab's first frame.
    void show_tab(TabId tab) {
        if (tab != tab_) tab_ = tab, tex_w_ = tex_h_ = 0;
    }

 protected:
    void render_gl(const wnk::render::CustomTarget &target, double now_ms) override;
    bool on_text(wnk::ui::Ui &ui, const std::string &utf8) override;
    bool on_edit_key(wnk::ui::Ui &ui, wnk::input::EditKey key) override;
    void on_text_input_closed(wnk::ui::Ui &ui) override;

 private:
    bool build();
    void upload(const EngineFrame &f);

    EngineClient &engine_;
    uint32_t page_w_ = 1280, page_h_ = 720;
    GLuint program_ = 0, tex_ = 0;
    GLint u_tex_ = -1;
    uint32_t tex_w_ = 0, tex_h_ = 0;
    bool editing_ = false;
    TabId tab_ = 0;
};

}  // namespace browser
