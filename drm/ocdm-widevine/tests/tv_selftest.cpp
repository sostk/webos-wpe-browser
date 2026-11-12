// End-to-end check of libocdm.so with the TV's real Widevine CDM, through the same entry points WebKit uses:
// construct_session -> challenge callback -> license server -> session_update -> key callbacks ->
// opencdm_gstreamer_session_decrypt_buffer on GstBuffers carrying a CENC GstProtectionMeta.
//
//   tv_selftest <pssh.bin> <license-url> <samples.vec> <expected-clear.bin> [video]
//
// samples.vec: kid(16) count(u32 LE) then per sample: iv_len(u8) iv  n(u16 LE) {clear u32 LE, protected u32 LE}*n
// len(u32 LE) data. expected-clear.bin: the decrypted samples back to back (independently verified).
// The license request is posted with the TV's curl binary. Run as the browser's jail user.
#include <gst/gst.h>
#include <ocdm/open_cdm_adapter.h>

#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <unistd.h>
#include <vector>

static std::string slurp(const char* path)
{
    std::string data;
    FILE* f = fopen(path, "rb");
    if (!f)
        return data;
    char buffer[8192];
    size_t n;
    while ((n = fread(buffer, 1, sizeof buffer, f)) > 0)
        data.append(buffer, n);
    fclose(f);
    return data;
}

static std::string hex(const std::string& s, size_t n = 8)
{
    std::string out;
    char t[4];
    for (size_t i = 0; i < s.size() && i < n; i++) {
        snprintf(t, sizeof t, "%02x", static_cast<unsigned char>(s[i]));
        out += t;
    }
    return out;
}

struct Events {
    std::mutex mutex;
    std::condition_variable changed;
    std::string challenge;
    int keys = 0;
    int keys_updated = 0;
    std::string error;
};

struct Sample {
    std::string iv;
    std::vector<std::pair<uint32_t, uint32_t>> subsamples;
    std::string data;
};

int main(int argc, char** argv)
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc < 5) {
        printf("usage: tv_selftest pssh.bin license-url samples.vec expected.bin [video]\n");
        return 2;
    }
    bool video = argc > 5 && !strcmp(argv[5], "video");
    gst_init(nullptr, nullptr);
    std::string pssh = slurp(argv[1]);
    std::string vectors = slurp(argv[3]);
    std::string expected = slurp(argv[4]);

    OpenCDMSystem* system = opencdm_create_system("com.widevine.alpha");
    printf("create_system: %s\n", system ? "ok" : "FAILED");
    if (!system)
        return 1;

    Events events;
    OpenCDMSessionCallbacks callbacks {};
    callbacks.process_challenge_callback = [](OpenCDMSession*, void* user, const char[], const uint8_t c[], const uint16_t n) {
        auto* e = static_cast<Events*>(user);
        std::lock_guard<std::mutex> lock(e->mutex);
        e->challenge.assign(reinterpret_cast<const char*>(c), n);
        e->changed.notify_all();
    };
    callbacks.key_update_callback = [](OpenCDMSession*, void* user, const uint8_t[], const uint8_t) {
        auto* e = static_cast<Events*>(user);
        std::lock_guard<std::mutex> lock(e->mutex);
        e->keys++;
    };
    callbacks.keys_updated_callback = [](const OpenCDMSession*, void* user) {
        auto* e = static_cast<Events*>(user);
        std::lock_guard<std::mutex> lock(e->mutex);
        e->keys_updated++;
        e->changed.notify_all();
    };
    callbacks.error_message_callback = [](OpenCDMSession*, void* user, const char m[]) {
        auto* e = static_cast<Events*>(user);
        std::lock_guard<std::mutex> lock(e->mutex);
        e->error = m;
        e->changed.notify_all();
    };

    OpenCDMSession* session = nullptr;
    OpenCDMError error = opencdm_construct_session(system, Temporary, "cenc", reinterpret_cast<const uint8_t*>(pssh.data()),
        pssh.size(), nullptr, 0, &callbacks, &events, &session);
    printf("construct_session: %d, id %s\n", error, session ? opencdm_session_id(session) : "-");
    if (error || !session)
        return 1;
    std::string challenge;
    {
        std::lock_guard<std::mutex> lock(events.mutex);
        challenge = events.challenge;
    }
    printf("challenge: %zu bytes, prefix \"%s\"\n", challenge.size(), challenge.substr(0, 7).c_str());
    if (challenge.compare(0, 7, "0:Type:"))
        return 1;

    // The page's job in a browser: send the license request, hand back the license.
    FILE* f = fopen("/tmp/wvp/selftest_challenge.bin", "wb");
    fwrite(challenge.data() + 7, 1, challenge.size() - 7, f);
    fclose(f);
    std::string command = std::string("curl -s -m 20 -X POST -H 'Content-Type: application/octet-stream' "
                                      "--data-binary @/tmp/wvp/selftest_challenge.bin -o /tmp/wvp/selftest_license.bin '")
        + argv[2] + "'";
    if (std::system(command.c_str()) != 0)
        printf("curl failed\n");
    std::string license = slurp("/tmp/wvp/selftest_license.bin");
    unlink("/tmp/wvp/selftest_challenge.bin");
    unlink("/tmp/wvp/selftest_license.bin");
    printf("license: %zu bytes\n", license.size());

    error = opencdm_session_update(session, reinterpret_cast<const uint8_t*>(license.data()), license.size());
    {
        std::unique_lock<std::mutex> lock(events.mutex);
        events.changed.wait_for(lock, std::chrono::seconds(5), [&] { return events.keys_updated > 0; });
        printf("session_update: %d, key_update x%d, keys_updated x%d\n", error, events.keys, events.keys_updated);
    }

    // Parse the samples.
    size_t at = 16;
    std::string kid = vectors.substr(0, 16);
    uint32_t count;
    memcpy(&count, &vectors[at], 4);
    at += 4;
    std::vector<Sample> samples;
    for (uint32_t i = 0; i < count; i++) {
        Sample s;
        uint8_t iv_length = vectors[at++];
        s.iv = vectors.substr(at, iv_length);
        at += iv_length;
        uint16_t n;
        memcpy(&n, &vectors[at], 2);
        at += 2;
        for (int j = 0; j < n; j++) {
            uint32_t c, p;
            memcpy(&c, &vectors[at], 4);
            memcpy(&p, &vectors[at + 4], 4);
            at += 8;
            s.subsamples.push_back({ c, p });
        }
        uint32_t length;
        memcpy(&length, &vectors[at], 4);
        at += 4;
        s.data = vectors.substr(at, length);
        at += length;
        samples.push_back(s);
    }
    KeyStatus status = opencdm_session_status(session, reinterpret_cast<const uint8_t*>(kid.data()), 16);
    printf("key %s status %d (0 = Usable)\n", hex(kid, 16).c_str(), status);
    OpenCDMSession* by_key = opencdm_get_system_session(system, reinterpret_cast<const uint8_t*>(kid.data()), 16, 1000);
    printf("get_system_session: %s\n", by_key == session ? "same session" : "WRONG");

    // Decrypt every sample as a GstBuffer with the protection meta qtdemux would attach.
    GstBuffer* kid_buffer = gst_buffer_new_wrapped(g_memdup2(kid.data(), 16), 16);
    GstCaps* caps = gst_caps_from_string(video ? "application/x-cenc, original-media-type=(string)video/x-h264"
                                               : "application/x-cenc, original-media-type=(string)audio/mpeg");
    std::string decrypted;
    int failed = 0;
    for (const Sample& s : samples) {
        GstBuffer* iv = gst_buffer_new_wrapped(g_memdup2(s.iv.data(), s.iv.size()), s.iv.size());
        GstStructure* info = gst_structure_new("application/x-cenc", "kid", GST_TYPE_BUFFER, kid_buffer, "iv_size", G_TYPE_UINT,
            static_cast<guint>(s.iv.size()), "iv", GST_TYPE_BUFFER, iv, "encrypted", G_TYPE_BOOLEAN, TRUE, nullptr);
        GstBuffer* subsamples = nullptr;
        if (!s.subsamples.empty()) {
            std::string packed;
            for (auto& sub : s.subsamples) {
                uint8_t e[6] = { uint8_t(sub.first >> 8), uint8_t(sub.first), uint8_t(sub.second >> 24), uint8_t(sub.second >> 16),
                    uint8_t(sub.second >> 8), uint8_t(sub.second) };
                packed.append(reinterpret_cast<char*>(e), 6);
            }
            subsamples = gst_buffer_new_wrapped(g_memdup2(packed.data(), packed.size()), packed.size());
            gst_structure_set(info, "subsample_count", G_TYPE_UINT, static_cast<guint>(s.subsamples.size()), "subsamples",
                GST_TYPE_BUFFER, subsamples, nullptr);
        }
        GstBuffer* buffer = gst_buffer_new_wrapped(g_memdup2(s.data.data(), s.data.size()), s.data.size());
        gst_buffer_add_protection_meta(buffer, info);
        if (opencdm_gstreamer_session_decrypt_buffer(by_key ? by_key : session, buffer, caps) != ERROR_NONE)
            failed++;
        GstMapInfo map;
        gst_buffer_map(buffer, &map, GST_MAP_READ);
        decrypted.append(reinterpret_cast<const char*>(map.data), map.size);
        gst_buffer_unmap(buffer, &map);
        gst_buffer_unref(buffer);
        gst_buffer_unref(iv);
        if (subsamples)
            gst_buffer_unref(subsamples);
    }
    bool match = decrypted == expected;
    printf("decrypt_buffer: %zu %s samples, %d failed, output %zu bytes, %s the verified decryption\n", samples.size(),
        video ? "video" : "audio", failed, decrypted.size(), match ? "MATCHES" : "DIFFERS FROM");

    gst_caps_unref(caps);
    gst_buffer_unref(kid_buffer);
    if (by_key)
        opencdm_destruct_session(by_key);
    opencdm_session_close(session);
    opencdm_destruct_session(session);
    opencdm_destruct_system(system);
    usleep(200 * 1000);
    printf(match && !failed ? "TV SELFTEST PASSED\n" : "TV SELFTEST FAILED\n");
    return match && !failed ? 0 : 1;
}
