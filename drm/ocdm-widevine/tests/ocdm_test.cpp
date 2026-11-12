// Drives the OpenCDM API the way WebKit's Thunder backend does, against tests/fake_cdm.cpp and a local
// provisioning server. Usage: ocdm_test <path to fake CDM .so>
#include "../src/ocdm_internal.h"

#include <ocdm/open_cdm.h>

#include <arpa/inet.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <netinet/in.h>
#include <string>
#include <sys/socket.h>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <vector>

static int failures = 0;
#define CHECK(cond)                                                                 \
    do {                                                                            \
        if (!(cond)) {                                                              \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);             \
            ++failures;                                                             \
        }                                                                           \
    } while (0)

// ---- a one-endpoint HTTP server answering the provisioning POST ----
struct ProvisioningServer {
    int fd = -1;
    int port = 0;
    std::atomic<int> hits { 0 };
    std::string last_request;
    std::thread thread;
    std::mutex mutex;

    void start()
    {
        fd = socket(AF_INET, SOCK_STREAM, 0);
        int one = 1;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
        sockaddr_in addr {};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr);
        socklen_t len = sizeof addr;
        getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len);
        port = ntohs(addr.sin_port);
        listen(fd, 4);
        thread = std::thread([this] {
            for (;;) {
                int c = accept(fd, nullptr, nullptr);
                if (c < 0)
                    return;
                char buffer[8192];
                ssize_t n = read(c, buffer, sizeof buffer - 1);
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    last_request.assign(buffer, n > 0 ? n : 0);
                }
                ++hits;
                const char reply[] = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: 27\r\n"
                                     "Connection: close\r\n\r\n{\"signedResponse\": \"fake\"}\n";
                (void)!write(c, reply, sizeof reply - 1);
                close(c);
            }
        });
    }
    void stop()
    {
        shutdown(fd, SHUT_RDWR);
        close(fd);
        thread.join();
    }
};

// ---- what WebKit's callbacks would see ----
struct Observed {
    std::mutex mutex;
    std::condition_variable changed;
    std::vector<std::string> challenges;
    std::vector<std::string> updated_keys;
    int keys_updated = 0;
    int errors = 0;
};

static OpenCDMSessionCallbacks callbacks()
{
    OpenCDMSessionCallbacks c {};
    c.process_challenge_callback = [](OpenCDMSession*, void* user, const char[], const uint8_t challenge[], const uint16_t length) {
        auto* o = static_cast<Observed*>(user);
        std::lock_guard<std::mutex> lock(o->mutex);
        o->challenges.emplace_back(reinterpret_cast<const char*>(challenge), length);
        o->changed.notify_all();
    };
    c.key_update_callback = [](OpenCDMSession*, void* user, const uint8_t key[], const uint8_t length) {
        auto* o = static_cast<Observed*>(user);
        std::lock_guard<std::mutex> lock(o->mutex);
        o->updated_keys.emplace_back(reinterpret_cast<const char*>(key), length);
    };
    c.keys_updated_callback = [](const OpenCDMSession*, void* user) {
        auto* o = static_cast<Observed*>(user);
        std::lock_guard<std::mutex> lock(o->mutex);
        o->keys_updated++;
        o->changed.notify_all();
    };
    c.error_message_callback = [](OpenCDMSession*, void* user, const char[]) {
        auto* o = static_cast<Observed*>(user);
        std::lock_guard<std::mutex> lock(o->mutex);
        o->errors++;
    };
    return c;
}

template<class Pred> static bool wait_for(Observed& o, Pred pred)
{
    std::unique_lock<std::mutex> lock(o.mutex);
    return o.changed.wait_for(lock, std::chrono::seconds(3), pred);
}

int main(int argc, char** argv)
{
    if (argc < 2) {
        std::printf("usage: ocdm_test <fake CDM .so>\n");
        return 2;
    }
    char tmpl[] = "/tmp/ocdm_test.XXXXXX";
    std::string storage = std::string(mkdtemp(tmpl)) + "/widevine";

    ProvisioningServer server;
    server.start();
    std::string url = "http://127.0.0.1:" + std::to_string(server.port) + "/create?key=test";
    setenv("OCDM_WIDEVINE_CDM_PATH", argv[1], 1);
    setenv("OCDM_WIDEVINE_STORAGE", storage.c_str(), 1);
    setenv("OCDM_WIDEVINE_PROVISIONING_URL", url.c_str(), 1);

    // Key system and content type support, as CDMFactoryThunder/CDMPrivateThunder ask.
    CHECK(opencdm_is_type_supported("com.widevine.alpha", "") == ERROR_NONE);
    CHECK(opencdm_is_type_supported("com.widevine.alpha", "video/mp4; codecs=\"avc1.42E01E\"") == ERROR_NONE);
    CHECK(opencdm_is_type_supported("com.widevine.alpha", "audio/webm; codecs=\"opus\"") == ERROR_NONE);
    CHECK(opencdm_is_type_supported("com.widevine.alpha", "video/x-matroska") != ERROR_NONE);
    CHECK(opencdm_is_type_supported("org.w3.clearkey", "") != ERROR_NONE);
    CHECK(opencdm_is_type_supported("com.microsoft.playready", "") != ERROR_NONE);

    CHECK(opencdm_create_system("org.w3.clearkey") == nullptr);
    OpenCDMSystem* system = opencdm_create_system("com.widevine.alpha");
    CHECK(system != nullptr);
    if (!system) {
        server.stop();
        return 1;
    }
    CHECK(!opencdm_system_supports_server_certificate(system));

    // First session: provisions the device, then delivers the license request with WebKit's type prefix.
    Observed observed;
    OpenCDMSessionCallbacks cbs = callbacks();
    const char pssh[] = "PSSH-BOX";
    OpenCDMSession* session = nullptr;
    CHECK(opencdm_construct_session(system, Temporary, "cenc", reinterpret_cast<const uint8_t*>(pssh), 8, nullptr, 0,
              &cbs, &observed, &session) == ERROR_NONE);
    CHECK(session != nullptr);
    CHECK(server.hits == 1);
    {
        std::lock_guard<std::mutex> lock(server.mutex);
        CHECK(server.last_request.find("POST /create?key=test&signedRequest=FAKE-PROVISIONING-REQUEST") == 0);
    }
    struct stat st;
    CHECK(stat((storage + "/cert.bin").c_str(), &st) == 0);
    CHECK(observed.challenges.size() == 1 && observed.challenges[0] == "0:Type:REQ:PSSH-BOX");
    std::string id = opencdm_session_id(session);
    CHECK(id.compare(0, 4, "ksid") == 0);

    // Unsupported requests fail cleanly.
    OpenCDMSession* other = nullptr;
    CHECK(opencdm_construct_session(system, PersistentLicense, "cenc", reinterpret_cast<const uint8_t*>(pssh), 8,
              nullptr, 0, &cbs, &observed, &other) == ERROR_INTERFACE_NOT_IMPLEMENTED && !other);
    CHECK(opencdm_construct_session(system, Temporary, "mystery", reinterpret_cast<const uint8_t*>(pssh), 8,
              nullptr, 0, &cbs, &observed, &other) == ERROR_INVALID_ARG && !other);

    // License: one key_update per key, then keys_updated; status becomes Usable.
    std::string kid_a(16, 'A'), kid_b(16, 'B'), kid_c(16, 'C');
    CHECK(opencdm_session_status(session, reinterpret_cast<const uint8_t*>(kid_a.data()), 16) == StatusPending);
    std::string license = "KEYS:" + kid_a + kid_b;
    CHECK(opencdm_session_update(session, reinterpret_cast<const uint8_t*>(license.data()), license.size()) == ERROR_NONE);
    CHECK(wait_for(observed, [&] { return observed.keys_updated >= 1; }));
    {
        std::lock_guard<std::mutex> lock(observed.mutex);
        CHECK(observed.updated_keys.size() >= 2);
    }
    CHECK(opencdm_session_status(session, reinterpret_cast<const uint8_t*>(kid_a.data()), 16) == Usable);
    CHECK(opencdm_session_status(session, reinterpret_cast<const uint8_t*>(kid_c.data()), 16) == StatusPending);
    std::string bad = "garbage";
    CHECK(opencdm_session_update(session, reinterpret_cast<const uint8_t*>(bad.data()), bad.size()) != ERROR_NONE);

    // Key lookup across sessions (CDMProxyThunder::getDecryptionSession).
    OpenCDMSession* by_key = opencdm_get_system_session(system, reinterpret_cast<const uint8_t*>(kid_b.data()), 16, 100);
    CHECK(by_key == session);
    if (by_key)
        opencdm_destruct_session(by_key);
    CHECK(opencdm_get_system_session(system, reinterpret_cast<const uint8_t*>(kid_c.data()), 16, 50) == nullptr);

    // Decryption: clear bytes copied, protected bytes decrypted, 8-byte IV accepted.
    {
        uint8_t data[10] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10 };
        uint8_t iv[8] = { 9, 9, 9, 9, 9, 9, 9, 9 };
        ocdm_widevine::SampleDescription sample;
        sample.key_id = reinterpret_cast<const uint8_t*>(kid_a.data());
        sample.key_id_length = 16;
        sample.iv = iv;
        sample.iv_length = 8;
        sample.subsamples = { { 2, 3 }, { 1, 4 } };
        CHECK(ocdm_widevine::decrypt_in_place(session, sample, data, sizeof data) == ERROR_NONE);
        const uint8_t expected[10] = { 1, 2, 3 ^ 0x5A, 4 ^ 0x5A, 5 ^ 0x5A, 6, 7 ^ 0x5A, 8 ^ 0x5A, 9 ^ 0x5A, 10 ^ 0x5A };
        CHECK(memcmp(data, expected, sizeof data) == 0);

        uint8_t whole[4] = { 0, 0, 0, 0 };
        sample.subsamples.clear();
        CHECK(ocdm_widevine::decrypt_in_place(session, sample, whole, sizeof whole) == ERROR_NONE);
        CHECK(whole[0] == 0x5A && whole[3] == 0x5A);

        sample.subsamples = { { 2, 3 } }; // does not cover the buffer
        CHECK(ocdm_widevine::decrypt_in_place(session, sample, data, sizeof data) == ERROR_INVALID_DECRYPT_BUFFER);
        sample.subsamples.clear();
        sample.key_id = reinterpret_cast<const uint8_t*>(kid_c.data()); // not licensed
        CHECK(ocdm_widevine::decrypt_in_place(session, sample, whole, sizeof whole) != ERROR_NONE);
    }

    // A second session reuses the stored certificate: no further provisioning.
    Observed observed2;
    OpenCDMSession* second = nullptr;
    CHECK(opencdm_construct_session(system, Temporary, "cenc", reinterpret_cast<const uint8_t*>(pssh), 8, nullptr, 0,
              &cbs, &observed2, &second) == ERROR_NONE);
    CHECK(server.hits == 1);
    CHECK(second && std::string(opencdm_session_id(second)) != id);

    // Close, then the session can no longer decrypt; references are released.
    CHECK(opencdm_session_close(session) == ERROR_NONE);
    {
        uint8_t whole[4] = { 0 };
        uint8_t iv[16] = { 0 };
        ocdm_widevine::SampleDescription sample;
        sample.key_id = reinterpret_cast<const uint8_t*>(kid_a.data());
        sample.key_id_length = 16;
        sample.iv = iv;
        sample.iv_length = 16;
        CHECK(ocdm_widevine::decrypt_in_place(session, sample, whole, sizeof whole) == ERROR_INVALID_SESSION);
    }
    CHECK(opencdm_get_system_session(system, reinterpret_cast<const uint8_t*>(kid_a.data()), 16, 50) == nullptr);
    CHECK(opencdm_destruct_session(session) == ERROR_NONE);
    CHECK(opencdm_destruct_session(second) == ERROR_NONE);
    CHECK(opencdm_destruct_system(system) == ERROR_NONE);
    std::this_thread::sleep_for(std::chrono::milliseconds(100)); // let the dispatcher finish releasing

    server.stop();
    std::string cleanup = "rm -rf '" + storage.substr(0, storage.rfind('/')) + "'";
    if (std::system(cleanup.c_str()) != 0)
        std::printf("note: could not clean %s\n", storage.c_str());
    std::printf(failures ? "%d failure(s)\n" : "ocdm_test: all checks passed\n", failures);
    return failures ? 1 : 0;
}
