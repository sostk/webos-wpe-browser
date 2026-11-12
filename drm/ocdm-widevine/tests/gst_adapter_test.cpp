// Decrypts GstBuffers carrying a GstProtectionMeta, as WebKit's decryptor hands them to libocdm, against the
// fake CDM. Usage: gst_adapter_test <path to fake CDM .so>
#include <ocdm/open_cdm_adapter.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <string>
#include <sys/stat.h>

static int failures = 0;
#define CHECK(cond)                                                                 \
    do {                                                                            \
        if (!(cond)) {                                                              \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);             \
            ++failures;                                                             \
        }                                                                           \
    } while (0)

typedef void (*LastBatchFn)(int*, uint32_t*, uint32_t*, bool*, uint32_t*, uint8_t[16]);

static GstBuffer* bytes(const void* data, size_t size)
{
    return gst_buffer_new_wrapped(g_memdup2(data, size), size);
}

static GstBuffer* sample(const uint8_t* data, size_t size, GstStructure* info)
{
    GstBuffer* buffer = bytes(data, size);
    gst_buffer_add_protection_meta(buffer, info);
    return buffer;
}

int main(int argc, char** argv)
{
    if (argc < 2)
        return 2;
    gst_init(nullptr, nullptr);
    char tmpl[] = "/tmp/gst_adapter_test.XXXXXX";
    std::string storage = std::string(mkdtemp(tmpl));
    // Already provisioned, so no network is needed.
    FILE* f = fopen((storage + "/cert.bin").c_str(), "w");
    fputs("{\"signedResponse\": \"fake\"}", f);
    fclose(f);
    setenv("OCDM_WIDEVINE_CDM_PATH", argv[1], 1);
    setenv("OCDM_WIDEVINE_STORAGE", storage.c_str(), 1);
    void* fake = dlopen(argv[1], RTLD_NOW);
    LastBatchFn last = fake ? reinterpret_cast<LastBatchFn>(dlsym(fake, "fake_cdm_last_batch")) : nullptr;
    CHECK(last != nullptr);
    if (!last)
        return 1;

    OpenCDMSystem* system = opencdm_create_system("com.widevine.alpha");
    OpenCDMSessionCallbacks callbacks {};
    OpenCDMSession* session = nullptr;
    CHECK(opencdm_construct_session(system, Temporary, "cenc", reinterpret_cast<const uint8_t*>("PSSH"), 4, nullptr, 0,
              &callbacks, nullptr, &session) == ERROR_NONE);
    std::string kid(16, 'K');
    std::string license = "KEYS:" + kid;
    CHECK(opencdm_session_update(session, reinterpret_cast<const uint8_t*>(license.data()), license.size()) == ERROR_NONE);

    GstBuffer* kid_buffer = bytes(kid.data(), 16);
    const uint8_t iv8[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
    int scheme;
    uint32_t pe, pc, subs;
    bool video;
    uint8_t iv[16];

    // 'cenc' video with subsamples (2 clear + 3 protected, 1 clear + 2 protected), 8-byte IV.
    {
        const uint8_t entries[] = { 0, 2, 0, 0, 0, 3, 0, 1, 0, 0, 0, 2 }; // big-endian u16 clear, u32 protected
        GstBuffer* subsamples = bytes(entries, sizeof entries);
        GstBuffer* iv_buffer = bytes(iv8, 8);
        GstStructure* info = gst_structure_new("application/x-cenc", "kid", GST_TYPE_BUFFER, kid_buffer, "iv_size",
            G_TYPE_UINT, 8u, "iv", GST_TYPE_BUFFER, iv_buffer, "encrypted", G_TYPE_BOOLEAN, TRUE, "subsample_count",
            G_TYPE_UINT, 2u, "subsamples", GST_TYPE_BUFFER, subsamples, nullptr);
        const uint8_t data[8] = { 10, 11, 12, 13, 14, 15, 16, 17 };
        GstBuffer* buffer = sample(data, sizeof data, info);
        GstCaps* caps = gst_caps_from_string("application/x-cenc, original-media-type=(string)video/x-h264");
        CHECK(opencdm_gstreamer_session_decrypt_buffer(session, buffer, caps) == ERROR_NONE);
        GstMapInfo map;
        gst_buffer_map(buffer, &map, GST_MAP_READ);
        const uint8_t expected[8] = { 10, 11, 12 ^ 0x5A, 13 ^ 0x5A, 14 ^ 0x5A, 15, 16 ^ 0x5A, 17 ^ 0x5A };
        CHECK(map.size == 8 && memcmp(map.data, expected, 8) == 0);
        gst_buffer_unmap(buffer, &map);
        last(&scheme, &pe, &pc, &video, &subs, iv);
        CHECK(scheme == 1 && pe == 0 && pc == 0 && video && subs == 2);
        CHECK(memcmp(iv, iv8, 8) == 0 && iv[8] == 0 && iv[15] == 0);
        gst_caps_unref(caps);
        gst_buffer_unref(buffer);
        gst_buffer_unref(subsamples);
        gst_buffer_unref(iv_buffer);
    }

    // 'cbcs' audio, whole sample, constant IV, 1:9 pattern.
    {
        const uint8_t civ[16] = { 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7 };
        GstBuffer* iv_buffer = bytes(civ, 16);
        GstStructure* info = gst_structure_new("application/x-cenc", "kid", GST_TYPE_BUFFER, kid_buffer, "iv_size",
            G_TYPE_UINT, 0u, "constant_iv_size", G_TYPE_UINT, 16u, "constant_iv", GST_TYPE_BUFFER, iv_buffer,
            "encrypted", G_TYPE_BOOLEAN, TRUE, "cipher-mode", G_TYPE_STRING, "cbcs", "crypt_byte_block", G_TYPE_UINT, 1u,
            "skip_byte_block", G_TYPE_UINT, 9u, nullptr);
        const uint8_t data[4] = { 1, 2, 3, 4 };
        GstBuffer* buffer = sample(data, sizeof data, info);
        GstCaps* caps = gst_caps_from_string("application/x-cenc, original-media-type=(string)audio/mpeg");
        CHECK(opencdm_gstreamer_session_decrypt_buffer(session, buffer, caps) == ERROR_NONE);
        last(&scheme, &pe, &pc, &video, &subs, iv);
        CHECK(scheme == 2 && pe == 1 && pc == 9 && !video && subs == 1 && iv[0] == 7);
        gst_caps_unref(caps);
        gst_buffer_unref(buffer);
        gst_buffer_unref(iv_buffer);
    }

    // Clear buffers pass through untouched: no meta, or meta with encrypted=false.
    {
        const uint8_t data[3] = { 5, 6, 7 };
        GstBuffer* plain = bytes(data, 3);
        CHECK(opencdm_gstreamer_session_decrypt_buffer(session, plain, nullptr) == ERROR_NONE);
        GstStructure* info = gst_structure_new("application/x-cenc", "encrypted", G_TYPE_BOOLEAN, FALSE, nullptr);
        GstBuffer* flagged = sample(data, 3, info);
        CHECK(opencdm_gstreamer_session_decrypt_buffer(session, flagged, nullptr) == ERROR_NONE);
        GstMapInfo map;
        gst_buffer_map(flagged, &map, GST_MAP_READ);
        CHECK(memcmp(map.data, data, 3) == 0);
        gst_buffer_unmap(flagged, &map);
        gst_buffer_unref(plain);
        gst_buffer_unref(flagged);
    }

    // Subsamples that do not cover the buffer are rejected, the buffer left as it was.
    {
        const uint8_t entries[] = { 0, 1, 0, 0, 0, 1 };
        GstBuffer* subsamples = bytes(entries, sizeof entries);
        GstBuffer* iv_buffer = bytes(iv8, 8);
        GstStructure* info = gst_structure_new("application/x-cenc", "kid", GST_TYPE_BUFFER, kid_buffer, "iv_size",
            G_TYPE_UINT, 8u, "iv", GST_TYPE_BUFFER, iv_buffer, "encrypted", G_TYPE_BOOLEAN, TRUE, "subsample_count",
            G_TYPE_UINT, 1u, "subsamples", GST_TYPE_BUFFER, subsamples, nullptr);
        const uint8_t data[5] = { 1, 2, 3, 4, 5 };
        GstBuffer* buffer = sample(data, sizeof data, info);
        CHECK(opencdm_gstreamer_session_decrypt_buffer(session, buffer, nullptr) == ERROR_INVALID_DECRYPT_BUFFER);
        GstMapInfo map;
        gst_buffer_map(buffer, &map, GST_MAP_READ);
        CHECK(memcmp(map.data, data, 5) == 0);
        gst_buffer_unmap(buffer, &map);
        gst_buffer_unref(buffer);
        gst_buffer_unref(subsamples);
        gst_buffer_unref(iv_buffer);
    }

    // The legacy explicit-arguments entry point ('cenc').
    {
        const uint8_t entries[] = { 0, 1, 0, 0, 0, 2 };
        GstBuffer* subsamples = bytes(entries, sizeof entries);
        GstBuffer* iv_buffer = bytes(iv8, 8);
        const uint8_t data[3] = { 1, 2, 3 };
        GstBuffer* buffer = bytes(data, 3);
        CHECK(opencdm_gstreamer_session_decrypt(session, buffer, subsamples, 1, iv_buffer, kid_buffer, 0) == ERROR_NONE);
        GstMapInfo map;
        gst_buffer_map(buffer, &map, GST_MAP_READ);
        CHECK(map.data[0] == 1 && map.data[1] == (2 ^ 0x5A) && map.data[2] == (3 ^ 0x5A));
        gst_buffer_unmap(buffer, &map);
        gst_buffer_unref(buffer);
        gst_buffer_unref(subsamples);
        gst_buffer_unref(iv_buffer);
    }

    gst_buffer_unref(kid_buffer);
    opencdm_session_close(session);
    opencdm_destruct_session(session);
    opencdm_destruct_system(system);
    std::string cleanup = "rm -rf '" + storage + "'";
    if (std::system(cleanup.c_str()) != 0)
        std::printf("note: could not clean %s\n", storage.c_str());
    std::printf(failures ? "%d failure(s)\n" : "gst_adapter_test: all checks passed\n", failures);
    return failures ? 1 : 0;
}
