// SPDX-License-Identifier: GPL-3.0-or-later
#include "engine_client.hpp"
#include "private_loader.hpp"

#include "wnk/log.hpp"

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstdio>
#include <cstring>

#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

extern char **environ;

namespace browser {
namespace {

constexpr const char *kCat = "engine";

void set_env(std::vector<std::string> &env, const std::string &name, const std::string &value) {
    const std::string prefix = name + "=";
    for (auto &e : env) {
        if (e.compare(0, prefix.size(), prefix) == 0) {
            e = prefix + value;
            return;
        }
    }
    env.push_back(prefix + value);
}

void unset_env(std::vector<std::string> &env, const std::string &name) {
    const std::string prefix = name + "=";
    env.erase(std::remove_if(env.begin(), env.end(),
                             [&](const std::string &e) { return e.compare(0, prefix.size(), prefix) == 0; }),
              env.end());
}

uint64_t monotonic_ms() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1000 + static_cast<uint64_t>(ts.tv_nsec) / 1000000;
}

}  // namespace

EngineClient::~EngineClient() { stop(); }

bool EngineClient::start(const EngineLaunch &l, std::string &error) {
    const std::string exe = l.engine_dir + "/wpe-engine";
    if (::access(exe.c_str(), X_OK) != 0) {
        error = "engine not found at " + exe;
        return false;
    }
    const std::string ca_bundle = l.engine_dir + "/etc/ssl/certs/ca-certificates.crt";
    if (::access(ca_bundle.c_str(), R_OK) != 0) {
        error = "cannot read the bundled TLS certificates at " + ca_bundle;
        return false;
    }
    // Hard-float engine (JIT): its executables use the bundled loader through a link in the runtime folder.
    bool private_loader = false;
    if (!prepare_private_loader(exe, l.engine_dir + "/lib", l.runtime_dir, private_loader, error)) return false;
    // The TV's Mali libraries are softfp and can't load into a hard-float process: llvmpipe only. The softfp engine
    // ships without Mesa (tools/stage-engine.sh, STAGE_MESA=1 adds it): software rendering then has nothing to draw
    // with, and the GPU is used anyway.
    const bool have_mesa = ::access((l.engine_dir + "/lib/mesa/libEGL.so.1").c_str(), R_OK) == 0;
    const bool gpu = private_loader ? false : (l.gpu || !have_mesa);
    if (l.gpu && !gpu) WNK_LOGW(kCat, "GPU mode needs the softfp engine; using software rendering");
    if (!l.gpu && gpu) WNK_LOGW(kCat, "software rendering needs Mesa, which this engine does not bundle; using the GPU");

    int sv[2];
    if (::socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sv) != 0) {
        error = std::string("socketpair: ") + std::strerror(errno);
        return false;
    }
    // dup2(fd, fd) would keep FD_CLOEXEC: make sure the engine's end is not already fd 3.
    int child = sv[1];
    if (child == WPE_IPC_ENGINE_FD) {
        child = ::fcntl(sv[1], F_DUPFD_CLOEXEC, 10);
        ::close(sv[1]);
    }
    const std::string log_path = l.runtime_dir + "/engine.log";
    // Keep the previous run's log: after a crash and restart it holds what happened.
    ::rename(log_path.c_str(), (l.runtime_dir + "/engine.prev.log").c_str());
    const int log_fd = ::open(log_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0640);

    std::vector<std::string> env;
    for (char **e = environ; e && *e; ++e) env.emplace_back(*e);
    const std::string &d = l.engine_dir;
    // Only the engine and its children see these; this process keeps the libraries it started with
    // (the loader read LD_LIBRARY_PATH at our start), so the TV's Mali EGL and the bundled Mesa never meet.
    // Software mode adds engine/lib/mesa (libEGL/libGLESv2 + llvmpipe); in GPU mode the web process
    // finds the TV's libEGL.so.1/libGLESv2.so.2 on the default search path instead.
    set_env(env, "LD_LIBRARY_PATH", gpu ? d + "/lib" : d + "/lib:" + d + "/lib/mesa");
    set_env(env, "WEBKIT_EXEC_PATH", d + "/libexec");
    set_env(env, "WPE_BROWSER_CA_FILE", ca_bundle);
    if (private_loader) {
        // The bundled glibc must not pick up the TV's (softfp) iconv modules or preloads.
        set_env(env, "GCONV_PATH", d + "/lib/gconv");
        set_env(env, "LD_PRELOAD", "");
    }
    set_env(env, "GIO_MODULE_DIR", d + "/lib/gio/modules");
    // GIO scans EXTRA_MODULES first; inherited firmware paths would win over MODULE_DIR.
    set_env(env, "GIO_EXTRA_MODULES", "");
    set_env(env, "WEBKIT_INJECTED_BUNDLE_PATH", d + "/lib/injected-bundle");
    set_env(env, "GST_PLUGIN_PATH", "");
    set_env(env, "GST_PLUGIN_PATH_1_0", "");
    set_env(env, "GST_PLUGIN_SYSTEM_PATH", d + "/lib/gstreamer-1.0");
    set_env(env, "GST_PLUGIN_SYSTEM_PATH_1_0", d + "/lib/gstreamer-1.0");
    set_env(env, "GST_PLUGIN_SCANNER", d + "/libexec/gst-plugin-scanner");
    set_env(env, "GST_PLUGIN_SCANNER_1_0", d + "/libexec/gst-plugin-scanner");
    set_env(env, "GST_REGISTRY", l.cache_dir + "/gstreamer-registry.bin");
    set_env(env, "GST_REGISTRY_1_0", l.cache_dir + "/gstreamer-registry.bin");
    // webOS sets GST_REGISTRY_UPDATE=no for its own GStreamer; inherited, our cached registry would never notice
    // plugins changed by an app update. Unset, GStreamer compares the plugin files' times and sizes at start.
    unset_env(env, "GST_REGISTRY_UPDATE");
    set_env(env, "PULSE_CLIENTCONFIG", d + "/etc/pulse/client.conf");
    // Sound: webOS runs PulseAudio in system mode (auth-anonymous) at /var/run/pulse/native, which the app jail mounts.
    // libpulse would otherwise look in XDG_RUNTIME_DIR (/tmp/xdg, owned by root), refuse it and play nothing.
    // Its own runtime files go to our runtime folder instead.
    static const char kSystemPulse[] = "/var/run/pulse/native";
    if (::access(kSystemPulse, W_OK) == 0) {
        set_env(env, "PULSE_SERVER", std::string("unix:") + kSystemPulse);
        // LG's policy module routes the virtual sinks to the speakers at their own volume; a stream on the
        // hardware sink (pcm_output, the default) gets 0 %. pmedia is the one for media playback.
        set_env(env, "PULSE_SINK", "pmedia");
    }
    set_env(env, "PULSE_RUNTIME_PATH", l.runtime_dir + "/pulse");
    set_env(env, "WEBKIT_GST_ENABLE_HLS_SUPPORT", "1");
    set_env(env, "WEBKIT_GST_ENABLE_DASH_SUPPORT", "1");
    set_env(env, "FONTCONFIG_FILE", d + "/etc/fonts/fonts.conf");
    set_env(env, "XDG_DATA_DIRS", d + "/share");
    set_env(env, "XDG_CACHE_HOME", l.cache_dir);
    set_env(env, "XDG_DATA_HOME", l.data_dir);
    set_env(env, "XDG_CONFIG_HOME", l.data_dir + "/config");
    set_env(env, "HOME", l.data_dir);
    if (gpu) {
        // Skia paints on the CPU; the GPU composites and presents. WebKit's hybrid GPU+CPU painting made Wikipedia's
        // first paint 3.9 → 3.5 s, but its Skia GPU contexts cache up to 256 MB each: on a YouTube watch page the
        // web process held 407 MB of Mali buffers instead of 140 MB, and webOS killed it under memory pressure
        // during playback (TV, 2026-11-11). {"wpebEnv": {"WEBKIT_SKIA_ENABLE_CPU_RENDERING": "0"}} tries hybrid again.
        set_env(env, "WEBKIT_SKIA_ENABLE_CPU_RENDERING", "1");
        set_env(env, "WEBKIT_DISABLE_DEFAULT_EGL_DISPLAY", "0");
        // More painting threads than WebKit's default (half the cores): the main thread is the bottleneck, and the
        // sooner its paint work is taken off it the better (Wikipedia on the TV, 2026-11-11).
        set_env(env, "WEBKIT_SKIA_CPU_PAINTING_THREADS", "4");
    } else {
        set_env(env, "GALLIUM_DRIVER", "llvmpipe");
        set_env(env, "MESA_SHADER_CACHE_DIR", l.cache_dir + "/mesa");
        // Painting through llvmpipe would be slower than Skia's CPU raster (NEON) on half the cores.
        set_env(env, "WEBKIT_SKIA_ENABLE_CPU_RENDERING", "1");
    }
    set_env(env, "WPE_BROWSER_RENDERER", gpu ? "gpu" : "software");
    // JavaScript garbage collection marks in parallel on every core.
    set_env(env, "JSC_numberOfGCMarkers", "4");
    // 32-bit ARM JSC corrupts its heap now and then (SIGSEGV in JSObject::allocateMoreOutOfLineStorage, 5 times on
    // the TV on 2026-11-11; open upstream: webkit.org/b/295780, b/275099). The only reported mitigation is the
    // iterative JSON parser (b/275099 comment 3); it costs nothing measurable.
    set_env(env, "JSC_useRecursiveJSONParse", "false");
    if (gpu && !l.surface_group.empty()) {
        set_env(env, "WPE_BROWSER_PRESENT", "direct");
        set_env(env, "WPEB_SURFACE_GROUP", l.surface_group);
        set_env(env, "WPEB_APP_ID", "org.sostk.wpebrowser");  // the page surfaces' webOS window role
        set_env(env, "WPEB_SURFACE_LAYER", l.surface_layer.empty() ? "page" : l.surface_layer);
        // The full socket path, so the web processes need not share our XDG_RUNTIME_DIR / WAYLAND_DISPLAY.
        if (!l.wayland_socket.empty()) set_env(env, "WPEB_WAYLAND_SOCKET", l.wayland_socket);
        if (!l.media_socket.empty()) {
            // Video goes to LG's decoder on the video plane under the page (engine/gst/wpebmediasink.c): WebKit's
            // generic "rialto" quirks find our sinks by name and punch a hole in the page where the video is.
            // Needs playbin3 (WebKit only punches holes there) and direct presentation (a page surface with alpha
            // above the video plane).
            set_env(env, "WPEB_MEDIA_SOCKET", l.media_socket);
            set_env(env, "WEBKIT_GST_QUIRKS", "rialto");
            set_env(env, "WEBKIT_GST_HOLE_PUNCH_QUIRK", "rialto");
            set_env(env, "WEBKIT_GST_USE_PLAYBIN3", "1");
        }
    } else {
        set_env(env, "WPE_BROWSER_PRESENT", "shm");
    }
    for (const auto &[name, value] : l.extra_env) {
        if (name.rfind("WEBKIT_", 0) == 0 || name.rfind("JSC_", 0) == 0 || name.rfind("WPE_BROWSER_DEBUG", 0) == 0 ||
            name.rfind("GST_DEBUG", 0) == 0) {
            set_env(env, name, value);
            WNK_LOGI(kCat, "engine env %s=%s (launch parameter)", name.c_str(), value.c_str());
        }
    }
    set_env(env, "WPE_BROWSER_DATA_DIR", l.data_dir);
    set_env(env, "WPE_BROWSER_CACHE_DIR", l.cache_dir);
    std::vector<char *> envp;
    for (auto &e : env) envp.push_back(e.data());
    envp.push_back(nullptr);

    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, child, WPE_IPC_ENGINE_FD);
    if (log_fd >= 0) {
        posix_spawn_file_actions_adddup2(&fa, log_fd, STDOUT_FILENO);
        posix_spawn_file_actions_adddup2(&fa, log_fd, STDERR_FILENO);
    }
    char *argv[] = {const_cast<char *>(exe.c_str()), nullptr};
    const int rc = ::posix_spawn(&pid_, exe.c_str(), &fa, nullptr, argv, envp.data());
    posix_spawn_file_actions_destroy(&fa);
    ::close(child);
    if (log_fd >= 0) ::close(log_fd);
    if (rc != 0) {
        ::close(sv[0]);
        pid_ = -1;
        error = std::string("posix_spawn: ") + std::strerror(rc);
        return false;
    }
    sock_ = sv[0];
    ::fcntl(sock_, F_SETFL, ::fcntl(sock_, F_GETFL) | O_NONBLOCK);
    WNK_LOGI(kCat, "started %s (pid %d), log %s", exe.c_str(), static_cast<int>(pid_), log_path.c_str());

    WpeAppHello hello{WPE_IPC_VERSION, l.width, l.height, 0};
    return send(WPE_APP_HELLO, 0, &hello, sizeof(hello));
}

bool EngineClient::send(uint32_t type, TabId tab, const void *payload, uint32_t size) {
    if (sock_ < 0) return false;
    uint8_t packet[WPE_IPC_MAX_PACKET];
    if (sizeof(WpeMsgHeader) + size > sizeof(packet)) return false;
    const WpeMsgHeader h{type, size, tab};
    std::memcpy(packet, &h, sizeof(h));
    if (size) std::memcpy(packet + sizeof(h), payload, size);
    for (;;) {
        const ssize_t n = ::send(sock_, packet, sizeof(h) + size, MSG_NOSIGNAL);
        if (n >= 0) return true;
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            // The engine is busy (a page is loading); input can wait a few ms rather than be lost.
            ::usleep(1000);
            continue;
        }
        WNK_LOGW(kCat, "send type %u: %s", type, std::strerror(errno));
        return false;
    }
}

void EngineClient::send_text(uint32_t type, TabId tab, const std::string &text) {
    WpeText t{};
    const size_t len = std::min<size_t>(text.size(), WPE_IPC_MAX_TEXT - 1);
    t.length = static_cast<uint32_t>(len);
    std::memcpy(t.text, text.data(), len);
    t.text[len] = '\0';
    send(type, tab, &t, static_cast<uint32_t>(offsetof(WpeText, text) + len + 1));
}

void EngineClient::resize(uint32_t w, uint32_t h) {
    const WpeSize s{w, h};
    send(WPE_APP_RESIZE, 0, &s, sizeof(s));
}

void EngineClient::set_visible(bool v) {
    const WpeFlag f{v ? 1u : 0u};
    send(WPE_APP_VISIBLE, 0, &f, sizeof(f));
}

void EngineClient::set_focus(bool focus) {
    const WpeFlag f{focus ? 1u : 0u};
    send(WPE_APP_FOCUS, 0, &f, sizeof(f));
}

void EngineClient::key(bool down, uint32_t keyval, uint32_t keycode, uint32_t modifiers) {
    const WpeKey k{down ? 1u : 0u, keyval, keycode, modifiers};
    send(WPE_APP_KEY, 0, &k, sizeof(k));
}

void EngineClient::pointer(WpePointerKind kind, uint32_t button, int32_t x, int32_t y, uint32_t time_ms) {
    const WpePointer p{static_cast<uint32_t>(kind), button, x, y, time_ms};
    send(WPE_APP_POINTER, 0, &p, sizeof(p));
}

void EngineClient::scroll(int32_t x, int32_t y, int32_t dx120, int32_t dy120) {
    const WpeScroll s{x, y, dx120, dy120};
    send(WPE_APP_SCROLL, 0, &s, sizeof(s));
}

void EngineClient::clear_data(uint32_t what, uint32_t seconds) {
    const WpeClearData c{what, seconds};
    send(WPE_APP_CLEAR_DATA, 0, &c, sizeof(c));
}

void EngineClient::set_scale(uint32_t permille) {
    const WpeZoom z{permille};
    send(WPE_APP_SET_SCALE, 0, &z, sizeof(z));
}

void EngineClient::set_zoom(uint32_t permille) {
    const WpeZoom z{permille};
    send(WPE_APP_SET_ZOOM, 0, &z, sizeof(z));
}

void EngineClient::frame_done() {
    if (!have_frame_) return;
    have_frame_ = false;
    const WpeFrameDone d{frame_.slot, frame_.serial};
    send(WPE_APP_FRAME_DONE, 0, &d, sizeof(d));
}

void EngineClient::poll() {
    if (pid_ > 0) {
        int status = 0;
        if (::waitpid(pid_, &status, WNOHANG) == pid_) {
            pid_ = -1;
            char why[64];
            if (WIFSIGNALED(status))
                std::snprintf(why, sizeof(why), "killed by signal %d", WTERMSIG(status));
            else
                std::snprintf(why, sizeof(why), "exited with status %d", WEXITSTATUS(status));
            lost(why);
            return;
        }
    }
    while (sock_ >= 0) {
        alignas(WpeMsgHeader) uint8_t buf[WPE_IPC_MAX_PACKET];
        iovec iov{buf, sizeof(buf)};
        alignas(cmsghdr) char control[CMSG_SPACE(sizeof(int))];
        msghdr msg{};
        msg.msg_iov = &iov;
        msg.msg_iovlen = 1;
        msg.msg_control = control;
        msg.msg_controllen = sizeof(control);
        const ssize_t n = ::recvmsg(sock_, &msg, MSG_DONTWAIT | MSG_CMSG_CLOEXEC);
        if (n < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) return;
            lost(std::string("recv: ") + std::strerror(errno));
            return;
        }
        if (n == 0) {
            lost("connection closed");
            return;
        }
        int fd = -1;
        for (cmsghdr *c = CMSG_FIRSTHDR(&msg); c != nullptr; c = CMSG_NXTHDR(&msg, c)) {
            if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_RIGHTS) std::memcpy(&fd, CMSG_DATA(c), sizeof(int));
        }
        handle(buf, static_cast<size_t>(n), fd);
    }
}

void EngineClient::handle(const uint8_t *buf, size_t n, int fd) {
    if (n < sizeof(WpeMsgHeader)) {
        if (fd >= 0) ::close(fd);
        return;
    }
    WpeMsgHeader h;
    std::memcpy(&h, buf, sizeof(h));
    const uint8_t *p = buf + sizeof(h);
    const size_t len = std::min<size_t>(h.size, n - sizeof(h));
    auto text = [&]() -> std::string {
        if (len < offsetof(WpeText, text)) return {};
        WpeText t;
        std::memcpy(&t, p, std::min(len, sizeof(t)));
        return std::string(t.text, std::min<size_t>({t.length, len - offsetof(WpeText, text), WPE_IPC_MAX_TEXT - 1}));
    };
    switch (h.type) {
    case WPE_ENGINE_READY:
        if (len >= sizeof(WpeEngineReady) && cb_.ready) {
            WpeEngineReady r;
            std::memcpy(&r, p, sizeof(r));
            cb_.ready(r);
        }
        break;
    case WPE_ENGINE_FRAMES: {
        if (fd < 0 || len < sizeof(WpeFrames)) break;
        WpeFrames f;
        std::memcpy(&f, p, sizeof(f));
        unmap();
        const size_t size = static_cast<size_t>(f.slot_bytes) * WPE_IPC_FRAME_SLOTS;
        void *m = ::mmap(nullptr, size, PROT_READ, MAP_SHARED, fd, 0);
        ::close(fd);
        fd = -1;
        if (m == MAP_FAILED) {
            WNK_LOGE(kCat, "mmap frames (%zu bytes): %s", size, std::strerror(errno));
            break;
        }
        frames_ = static_cast<uint8_t *>(m);
        frames_size_ = size;
        slot_bytes_ = f.slot_bytes;
        WNK_LOGI(kCat, "frame slots mapped: %u x %u bytes", WPE_IPC_FRAME_SLOTS, f.slot_bytes);
        break;
    }
    case WPE_ENGINE_FRAME: {
        if (len < offsetof(WpeFrame, damage) || frames_ == nullptr) break;
        WpeFrame f{};
        std::memcpy(&f, p, std::min(len, sizeof(f)));
        if (f.slot >= WPE_IPC_FRAME_SLOTS || static_cast<uint64_t>(f.stride) * f.height > slot_bytes_ ||
            f.stride < f.width * 4) {
            WNK_LOGW(kCat, "bad frame message (slot %u, %ux%u stride %u)", f.slot, f.width, f.height, f.stride);
            break;
        }
        if (have_frame_) {
            // An older frame was never drawn (e.g. while hidden): let the engine have it back.
            const WpeFrameDone d{frame_.slot, frame_.serial};
            send(WPE_APP_FRAME_DONE, 0, &d, sizeof(d));
        }
        frame_.tab = h.tab;
        frame_.pixels = frames_ + static_cast<size_t>(f.slot) * slot_bytes_;
        frame_.width = f.width;
        frame_.height = f.height;
        frame_.stride = f.stride;
        frame_.slot = f.slot;
        frame_.serial = f.serial;
        frame_.damage.assign(f.damage, f.damage + std::min<uint32_t>(f.n_damage, WPE_IPC_MAX_DAMAGE));
        have_frame_ = true;
        break;
    }
    case WPE_ENGINE_URI:
        if (cb_.uri) cb_.uri(h.tab, text());
        break;
    case WPE_ENGINE_TITLE:
        if (cb_.title) cb_.title(h.tab, text());
        break;
    case WPE_ENGINE_ICON:
        if (cb_.icon) cb_.icon(h.tab, text());
        break;
    case WPE_ENGINE_PROGRESS:
        if (len >= sizeof(WpeProgress) && cb_.progress) {
            WpeProgress pr;
            std::memcpy(&pr, p, sizeof(pr));
            cb_.progress(h.tab, static_cast<float>(pr.permille) / 1000.0f);
        }
        break;
    case WPE_ENGINE_LOAD:
        if (len >= sizeof(WpeLoad) && cb_.load) {
            WpeLoad ld;
            std::memcpy(&ld, p, sizeof(ld));
            cb_.load(h.tab, static_cast<WpeLoadEvent>(ld.event));
        }
        break;
    case WPE_ENGINE_HISTORY:
        if (len >= sizeof(WpeHistory) && cb_.history) {
            WpeHistory hi;
            std::memcpy(&hi, p, sizeof(hi));
            cb_.history(h.tab, hi.can_go_back != 0, hi.can_go_forward != 0);
        }
        break;
    case WPE_ENGINE_TAB_CREATED:
        if (len >= sizeof(WpeTabCreated) && cb_.tab_created) {
            WpeTabCreated c;
            std::memcpy(&c, p, sizeof(c));
            cb_.tab_created(h.tab, c.opener);
        }
        break;
    case WPE_ENGINE_TAB_CLOSE:
        if (cb_.tab_close) cb_.tab_close(h.tab);
        break;
    case WPE_ENGINE_FULLSCREEN:
        if (len >= sizeof(WpeFlag) && cb_.fullscreen) {
            WpeFlag f;
            std::memcpy(&f, p, sizeof(f));
            cb_.fullscreen(h.tab, f.value != 0);
        }
        break;
    case WPE_ENGINE_SCROLL:
        if (len >= sizeof(WpeFlag) && cb_.scroll) {
            WpeFlag f;
            std::memcpy(&f, p, sizeof(f));
            cb_.scroll(h.tab, f.value != 0);
        }
        break;
    case WPE_ENGINE_EDGE:
        if (len >= sizeof(WpeFlag) && cb_.edge_up) {
            WpeFlag f;
            std::memcpy(&f, p, sizeof(f));
            if (f.value == 1) cb_.edge_up(h.tab);
        }
        break;
    case WPE_ENGINE_IME:
        if (len >= sizeof(WpeIme) && cb_.ime) {
            WpeIme m;
            std::memcpy(&m, p, sizeof(m));
            cb_.ime(h.tab, m);
        }
        break;
    case WPE_ENGINE_ERROR:
        if (len >= offsetof(WpeError, message) && cb_.error) {
            WpeError e{};
            std::memcpy(&e, p, std::min(len, sizeof(e)));
            const size_t tl = len > offsetof(WpeError, message) + offsetof(WpeText, text)
                                  ? len - offsetof(WpeError, message) - offsetof(WpeText, text)
                                  : 0;
            cb_.error(h.tab, static_cast<WpeErrorKind>(e.kind), e.code,
                      std::string(e.message.text, std::min<size_t>({e.message.length, tl, WPE_IPC_MAX_TEXT - 1})));
        }
        break;
    case WPE_ENGINE_PRESENT:
        if (len >= sizeof(WpePresent) && cb_.present) {
            WpePresent m;
            std::memcpy(&m, p, sizeof(m));
            cb_.present(h.tab, m);
        }
        break;
    case WPE_ENGINE_DATA_CLEARED:
        if (len >= sizeof(WpeDataCleared) && cb_.data_cleared) {
            WpeDataCleared d;
            std::memcpy(&d, p, sizeof(d));
            cb_.data_cleared(d.what, d.ok != 0);
        }
        break;
    default:
        WNK_LOGD(kCat, "ignoring message type %u", h.type);
    }
    if (fd >= 0) ::close(fd);
}

void EngineClient::unmap() {
    if (frames_ != nullptr) ::munmap(frames_, frames_size_);
    frames_ = nullptr;
    frames_size_ = 0;
    slot_bytes_ = 0;
    have_frame_ = false;
}

void EngineClient::lost(const std::string &why) {
    WNK_LOGW(kCat, "engine lost: %s", why.c_str());
    if (sock_ >= 0) ::close(sock_);
    sock_ = -1;
    unmap();
    if (cb_.exited) cb_.exited(why);
}

void EngineClient::stop() {
    if (sock_ >= 0) {
        send(WPE_APP_QUIT, 0, nullptr, 0);
        ::close(sock_);
        sock_ = -1;
    }
    if (pid_ > 0) {
        // The kit's shutdown watchdog allows 3 s in total: give the engine 1 s, then kill it.
        const uint64_t deadline = monotonic_ms() + 1000;
        while (monotonic_ms() < deadline) {
            if (::waitpid(pid_, nullptr, WNOHANG) == pid_) {
                pid_ = -1;
                break;
            }
            ::usleep(10000);
        }
        if (pid_ > 0) {
            ::kill(pid_, SIGKILL);
            ::waitpid(pid_, nullptr, 0);
            pid_ = -1;
        }
    }
    unmap();
}

}  // namespace browser
