// SPDX-License-Identifier: GPL-3.0-or-later
#include "page_view.hpp"

#include "wnk/log.hpp"
#include "wnk/ui/ui.hpp"

#include <algorithm>

namespace browser {
namespace {

constexpr const char *kCat = "page";

GLuint compile(GLenum type, const char *src) {
    const GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    return s;
}

}  // namespace

PageView::PageView(EngineClient &engine) : engine_(engine) {
    set_focusable(true);
    name = "page";
}

PageView::~PageView() {
    if (tex_ != 0) glDeleteTextures(1, &tex_);
    if (program_ != 0) glDeleteProgram(program_);
}

bool PageView::build() {
    // WebKit's pixels are B,G,R,A bytes; GLES2 has no BGRA upload format everywhere, so the texture is
    // filled as RGBA and the shader swaps the channels back.
    const char *vs =
        "attribute vec2 p; attribute vec2 t; varying vec2 vt;"
        "void main() { vt = t; gl_Position = vec4(p, 0.0, 1.0); }";
    const char *fs =
        "precision mediump float; varying vec2 vt; uniform sampler2D tex;"
        "void main() { gl_FragColor = vec4(texture2D(tex, vt).bgr, 1.0); }";
    program_ = glCreateProgram();
    const GLuint v = compile(GL_VERTEX_SHADER, vs), f = compile(GL_FRAGMENT_SHADER, fs);
    glAttachShader(program_, v);
    glAttachShader(program_, f);
    glBindAttribLocation(program_, 0, "p");
    glBindAttribLocation(program_, 1, "t");
    glLinkProgram(program_);
    glDeleteShader(v);
    glDeleteShader(f);
    GLint ok = 0;
    glGetProgramiv(program_, GL_LINK_STATUS, &ok);
    if (ok == 0) {
        WNK_LOGE(kCat, "page shader did not link");
        glDeleteProgram(program_);
        program_ = 0;
        return false;
    }
    u_tex_ = glGetUniformLocation(program_, "tex");
    glGenTextures(1, &tex_);
    glBindTexture(GL_TEXTURE_2D, tex_);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    return true;
}

void PageView::upload(const EngineFrame &f) {
    glBindTexture(GL_TEXTURE_2D, tex_);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    const bool packed = f.stride == f.width * 4;
    if (f.width != tex_w_ || f.height != tex_h_) {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, static_cast<GLsizei>(f.width), static_cast<GLsizei>(f.height), 0,
                     GL_RGBA, GL_UNSIGNED_BYTE, packed ? f.pixels : nullptr);
        if (!packed) {
            for (uint32_t y = 0; y < f.height; ++y)
                glTexSubImage2D(GL_TEXTURE_2D, 0, 0, static_cast<GLint>(y), static_cast<GLsizei>(f.width), 1, GL_RGBA,
                                GL_UNSIGNED_BYTE, f.pixels + static_cast<size_t>(y) * f.stride);
        }
        tex_w_ = f.width;
        tex_h_ = f.height;
        return;
    }
    // GLES2 cannot upload a sub-rectangle of a wider row (no UNPACK_ROW_LENGTH), so upload the full-width
    // band of rows covering all damage: one call when rows are packed.
    uint32_t y0 = 0, y1 = f.height;
    if (!f.damage.empty()) {
        y0 = f.height;
        y1 = 0;
        for (const WpeRect &r : f.damage) {
            const int32_t top = std::max(0, r.y), bottom = std::min<int32_t>(static_cast<int32_t>(f.height), r.y + r.height);
            if (bottom <= top) continue;
            y0 = std::min(y0, static_cast<uint32_t>(top));
            y1 = std::max(y1, static_cast<uint32_t>(bottom));
        }
        if (y1 <= y0) return;
    }
    if (packed) {
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, static_cast<GLint>(y0), static_cast<GLsizei>(f.width),
                        static_cast<GLsizei>(y1 - y0), GL_RGBA, GL_UNSIGNED_BYTE, f.pixels + static_cast<size_t>(y0) * f.stride);
    } else {
        for (uint32_t y = y0; y < y1; ++y)
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, static_cast<GLint>(y), static_cast<GLsizei>(f.width), 1, GL_RGBA,
                            GL_UNSIGNED_BYTE, f.pixels + static_cast<size_t>(y) * f.stride);
    }
}

void PageView::render_gl(const wnk::render::CustomTarget &, double) {
    if (direct_) {
        // Alpha 0 lets the page surface below the window show; the draw call's scissor limits it to this view.
        const bool hole = tab_ != 0 && direct_tab_ == tab_;
        glClearColor(hole ? 0 : 1, hole ? 0 : 1, hole ? 0 : 1, hole ? 0 : 1);
        glClear(GL_COLOR_BUFFER_BIT);
        return;
    }
    if (program_ == 0 && !build()) return;
    if (const EngineFrame *f = engine_.frame()) {
        if (f->tab == tab_) upload(*f);
        engine_.frame_done();
    }
    glClearColor(1, 1, 1, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    if (tex_w_ == 0) return;

    // Full-view quad; texture row 0 is the top of the page.
    static const GLfloat quad[] = {
        -1, -1, 0, 1,  //
        1,  -1, 1, 1,  //
        -1, 1,  0, 0,  //
        1,  1,  1, 0,
    };
    glUseProgram(program_);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tex_);
    glUniform1i(u_tex_, 0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat), quad);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat), quad + 2);
    glDisable(GL_BLEND);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glDisableVertexAttribArray(0);
    glDisableVertexAttribArray(1);
}

bool PageView::to_page(wnk::Point l, int32_t &x, int32_t &y) const {
    const wnk::Rect r = screen_rect();
    if (r.w <= 0 || r.h <= 0 || l.x < r.x || l.y < r.y || l.x >= r.x + r.w || l.y >= r.y + r.h) return false;
    x = static_cast<int32_t>((l.x - r.x) * static_cast<float>(page_w_) / r.w);
    y = static_cast<int32_t>((l.y - r.y) * static_cast<float>(page_h_) / r.h);
    return true;
}

void PageView::ime(wnk::ui::Ui &ui, bool focused) {
    if (focused && !editing_) {
        editing_ = true;
        ui.begin_text_input(this);
    } else if (!focused && editing_) {
        editing_ = false;
        ui.end_text_input(this);
    }
}

bool PageView::on_text(wnk::ui::Ui &, const std::string &utf8) {
    engine_.commit_text(utf8);
    return true;
}

bool PageView::on_edit_key(wnk::ui::Ui &, wnk::input::EditKey key) {
    using K = wnk::input::EditKey;
    uint32_t keysym = 0, evdev = 0;
    switch (key) {
    case K::Backspace: keysym = 0xff08, evdev = 14; break;
    case K::Delete: keysym = 0xffff, evdev = 111; break;
    case K::Left: keysym = 0xff51, evdev = 105; break;
    case K::Right: keysym = 0xff53, evdev = 106; break;
    case K::Home: keysym = 0xff50, evdev = 102; break;
    case K::End: keysym = 0xff57, evdev = 107; break;
    case K::Enter: keysym = 0xff0d, evdev = 28; break;
    case K::Clear: return true;  // no page equivalent
    }
    engine_.key(true, keysym, evdev + 8, 0);
    engine_.key(false, keysym, evdev + 8, 0);
    return true;
}

void PageView::on_text_input_closed(wnk::ui::Ui &) {
    // The viewer closed the TV keyboard; the page field keeps focus until the user moves on.
    editing_ = false;
}

}  // namespace browser
