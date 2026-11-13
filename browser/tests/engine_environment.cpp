// SPDX-License-Identifier: GPL-3.0-or-later
// Exercise the real spawn path: firmware and bundled plugin ABIs must never mix.
#include "engine_client.hpp"
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>
#include <thread>
#include <sys/socket.h>
#include <unistd.h>

static const char* names[] = {
    "LD_LIBRARY_PATH", "WPE_BROWSER_CA_FILE", "WEBKIT_SKIA_ENABLE_CPU_RENDERING", "WPE_BROWSER_RENDERER",
    "GST_PLUGIN_PATH", "GST_PLUGIN_PATH_1_0", "GST_PLUGIN_SYSTEM_PATH", "GST_PLUGIN_SYSTEM_PATH_1_0",
    "GST_PLUGIN_SCANNER", "GST_PLUGIN_SCANNER_1_0", "GST_REGISTRY", "GST_REGISTRY_1_0",
    "GIO_MODULE_DIR", "GIO_EXTRA_MODULES", "WEBKIT_INJECTED_BUNDLE_PATH", "PULSE_CLIENTCONFIG", "PULSE_RUNTIME_PATH", "GST_REGISTRY_UPDATE", "WEBKIT_GST_ENABLE_HLS_SUPPORT", "WEBKIT_GST_ENABLE_DASH_SUPPORT"
};

static void require(bool condition, const std::string& why) {
    if (!condition) throw std::runtime_error(why);
}

static int fake_engine() {
    {
        std::ofstream out(std::getenv("WPE_TEST_CAPTURE"));
        for (auto name : names) {
            auto value = std::getenv(name);
            out << name << '=' << (value ? value : "<unset>") << '\n';
        }
    }
    char packet[WPE_IPC_MAX_PACKET];
    while (true) {
        auto size = recv(WPE_IPC_ENGINE_FD, packet, sizeof(packet), 0);
        if (size < static_cast<ssize_t>(sizeof(WpeMsgHeader))) return 0;
        WpeMsgHeader header;
        std::memcpy(&header, packet, sizeof(header));
        if (header.type == WPE_APP_QUIT) return 0;
    }
}

int main(int argc, char** argv) {
    (void)argc;
    if (std::filesystem::path(argv[0]).filename() == "wpe-engine") return fake_engine();
    char pattern[] = "/tmp/wpe-env-test-XXXXXX";
    auto dir = mkdtemp(pattern);
    if (!dir) return 1;
    const std::filesystem::path base(dir);
    try {
        std::filesystem::create_symlink(std::filesystem::read_symlink("/proc/self/exe"), base / "wpe-engine");
        std::filesystem::create_directories(base / "etc/ssl/certs");
        std::ofstream(base / "etc/ssl/certs/ca-certificates.crt") << "test fixture\n";
        // Software rendering is only chosen when the engine bundles Mesa (else the GPU is used anyway).
        std::filesystem::create_directories(base / "lib/mesa");
        std::ofstream(base / "lib/mesa/libEGL.so.1") << "test fixture\n";
        const auto capture = base / "environment";
        setenv("WPE_TEST_CAPTURE", capture.c_str(), 1);
        for (auto name : names) setenv(name, "/firmware-or-parent-must-not-leak", 1);
        for (bool gpu : {false, true}) {
            std::filesystem::remove(capture);
            browser::EngineLaunch launch;
            launch.engine_dir = base;
            launch.runtime_dir = base;
            launch.data_dir = base / "data";
            launch.cache_dir = base / "cache";
            launch.gpu = gpu;
            browser::EngineClient client({});
            std::string error;
            require(client.start(launch, error), error);
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
            while (!std::filesystem::exists(capture) && std::chrono::steady_clock::now() < deadline)
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            // stop joins the child after it has closed the capture file.
            client.stop();
            std::ifstream in(capture);
            require(in.good(), "spawned engine did not capture its environment");
            std::map<std::string, std::string> env;
            for (std::string line; std::getline(in, line);) {
                auto equal = line.find('=');
                env[line.substr(0, equal)] = line.substr(equal + 1);
            }
            auto expect = [&](const char* key, const std::string& value) {
                require(env.at(key) == value, std::string(key) + " was " + env.at(key));
            };
            expect("LD_LIBRARY_PATH", launch.engine_dir + "/lib" + (gpu ? "" : ":" + launch.engine_dir + "/lib/mesa"));
            expect("WPE_BROWSER_CA_FILE", launch.engine_dir + "/etc/ssl/certs/ca-certificates.crt");
            expect("WPE_BROWSER_RENDERER", gpu ? "gpu" : "software");
            // Skia paints on the CPU in both modes (GPU painting's caches cost ~270 MB on YouTube).
            expect("WEBKIT_SKIA_ENABLE_CPU_RENDERING", "1");
            for (auto key : {"GST_PLUGIN_PATH", "GST_PLUGIN_PATH_1_0"}) expect(key, "");
            for (auto key : {"GST_PLUGIN_SYSTEM_PATH", "GST_PLUGIN_SYSTEM_PATH_1_0"}) expect(key, launch.engine_dir + "/lib/gstreamer-1.0");
            for (auto key : {"GST_PLUGIN_SCANNER", "GST_PLUGIN_SCANNER_1_0"}) expect(key, launch.engine_dir + "/libexec/gst-plugin-scanner");
            for (auto key : {"GST_REGISTRY", "GST_REGISTRY_1_0"}) expect(key, launch.cache_dir + "/gstreamer-registry.bin");
            expect("GIO_MODULE_DIR", launch.engine_dir + "/lib/gio/modules");
            expect("GIO_EXTRA_MODULES", "");
            expect("WEBKIT_INJECTED_BUNDLE_PATH", launch.engine_dir + "/lib/injected-bundle");
            expect("PULSE_CLIENTCONFIG", launch.engine_dir + "/etc/pulse/client.conf");
            expect("PULSE_RUNTIME_PATH", launch.runtime_dir + "/pulse");
            expect("GST_REGISTRY_UPDATE", "<unset>");
            expect("WEBKIT_GST_ENABLE_HLS_SUPPORT", "1");
            expect("WEBKIT_GST_ENABLE_DASH_SUPPORT", "1");
        }
        std::filesystem::remove_all(base);
        std::cout << "software/GPU process environments isolated from parent and firmware plugins\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        std::filesystem::remove_all(base);
        return 1;
    }
}
