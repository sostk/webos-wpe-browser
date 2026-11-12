// GStreamer entry points: decrypt a buffer described by its GstProtectionMeta (as qtdemux/matroskademux and
// WebKit's MSE append pipeline attach it).
#include "ocdm_internal.h"

#include <ocdm/open_cdm_adapter.h>

#include <gst/base/gstbytereader.h>
#include <cstring>

using namespace ocdm_widevine;

namespace {

bool caps_are_video(GstCaps* caps)
{
    if (!caps || gst_caps_is_empty(caps))
        return true;
    const GstStructure* s = gst_caps_get_structure(caps, 0);
    const char* original = gst_structure_get_string(s, "original-media-type");
    const char* name = original ? original : gst_structure_get_name(s);
    return !name || strncmp(name, "audio/", 6) != 0;
}

// CENC subsample entries: 16-bit clear bytes, 32-bit protected bytes, big-endian.
bool parse_subsamples(GstBuffer* buffer, unsigned count, std::vector<widevine::Cdm::Subsample>& out)
{
    GstMapInfo map;
    if (!gst_buffer_map(buffer, &map, GST_MAP_READ))
        return false;
    GstByteReader reader;
    gst_byte_reader_init(&reader, map.data, map.size);
    bool ok = true;
    for (unsigned i = 0; i < count && ok; i++) {
        guint16 clear = 0;
        guint32 protected_bytes = 0;
        ok = gst_byte_reader_get_uint16_be(&reader, &clear) && gst_byte_reader_get_uint32_be(&reader, &protected_bytes);
        if (ok)
            out.push_back(widevine::Cdm::Subsample { clear, protected_bytes });
    }
    gst_buffer_unmap(buffer, &map);
    return ok;
}

struct Mapped {
    GstBuffer* buffer = nullptr;
    GstMapInfo info {};
    bool ok = false;
    Mapped(GstBuffer* b, GstMapFlags flags)
        : buffer(b)
    {
        ok = b && gst_buffer_map(b, &info, flags);
    }
    ~Mapped()
    {
        if (ok)
            gst_buffer_unmap(buffer, &info);
    }
};

OpenCDMError decrypt(OpenCDMSession* session, GstBuffer* buffer, GstBuffer* key_id, GstBuffer* iv,
    std::vector<widevine::Cdm::Subsample>&& subsamples, SampleDescription description)
{
    Mapped kid(key_id, GST_MAP_READ);
    Mapped vector(iv, GST_MAP_READ);
    Mapped data(buffer, GST_MAP_READWRITE);
    if (!kid.ok || !vector.ok || !data.ok)
        return ERROR_INVALID_DECRYPT_BUFFER;
    description.key_id = kid.info.data;
    description.key_id_length = kid.info.size;
    description.iv = vector.info.data;
    description.iv_length = vector.info.size;
    description.subsamples = std::move(subsamples);
    return decrypt_in_place(session, description, data.info.data, data.info.size);
}

} // namespace

extern "C" {

OpenCDMError opencdm_gstreamer_session_decrypt_buffer(struct OpenCDMSession* session, GstBuffer* buffer, GstCaps* caps)
{
    if (!session || !buffer)
        return ERROR_INVALID_ARG;
    GstProtectionMeta* meta = gst_buffer_get_protection_meta(buffer);
    if (!meta)
        return ERROR_NONE; // clear buffer
    const GstStructure* info = meta->info;

    gboolean encrypted = TRUE;
    if (gst_structure_has_field(info, "encrypted"))
        gst_structure_get_boolean(info, "encrypted", &encrypted);
    if (!encrypted)
        return ERROR_NONE;

    SampleDescription description;
    description.is_video = caps_are_video(caps);
    const char* mode = gst_structure_get_string(info, "cipher-mode");
    if (mode && (!strcmp(mode, "cbcs") || !strcmp(mode, "cbc1")))
        description.scheme = widevine::Cdm::kAesCbc;
    guint value = 0;
    if (gst_structure_get_uint(info, "crypt_byte_block", &value))
        description.pattern_encrypted = value;
    if (gst_structure_get_uint(info, "skip_byte_block", &value))
        description.pattern_clear = value;

    const GValue* kid = gst_structure_get_value(info, "kid");
    guint iv_size = 0;
    gst_structure_get_uint(info, "iv_size", &iv_size);
    const GValue* iv = gst_structure_get_value(info, iv_size ? "iv" : "constant_iv");
    if (!kid || !iv)
        return ERROR_INVALID_DECRYPT_BUFFER;

    std::vector<widevine::Cdm::Subsample> subsamples;
    guint count = 0;
    gst_structure_get_uint(info, "subsample_count", &count);
    if (count) {
        const GValue* list = gst_structure_get_value(info, "subsamples");
        if (!list || !parse_subsamples(gst_value_get_buffer(list), count, subsamples))
            return ERROR_INVALID_DECRYPT_BUFFER;
    }
    return decrypt(session, buffer, gst_value_get_buffer(kid), gst_value_get_buffer(iv), std::move(subsamples), description);
}

OpenCDMError opencdm_gstreamer_session_decrypt(struct OpenCDMSession* session, GstBuffer* buffer, GstBuffer* subSample,
    const uint32_t subSampleCount, GstBuffer* IV, GstBuffer* keyID, uint32_t)
{
    if (!session || !buffer || !IV || !keyID)
        return ERROR_INVALID_ARG;
    std::vector<widevine::Cdm::Subsample> subsamples;
    if (subSampleCount && (!subSample || !parse_subsamples(subSample, subSampleCount, subsamples)))
        return ERROR_INVALID_DECRYPT_BUFFER;
    SampleDescription description;
    return decrypt(session, buffer, keyID, IV, std::move(subsamples), description);
}

} // extern "C"
