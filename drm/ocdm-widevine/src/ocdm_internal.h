// Shared between the OpenCDM core (ocdm.cpp) and the GStreamer adapter (gst_adapter.cpp).
#pragma once

#include <ocdm/open_cdm.h>
#include <widevine/cdm.h>

#include <cstddef>
#include <cstdint>
#include <vector>

namespace ocdm_widevine {

// One encrypted sample as described by the container (CENC 'senc'/'tenc' or the GStreamer protection meta).
struct SampleDescription {
    const uint8_t* key_id = nullptr;
    size_t key_id_length = 0;
    const uint8_t* iv = nullptr;      // 8 or 16 bytes
    size_t iv_length = 0;
    std::vector<widevine::Cdm::Subsample> subsamples; // empty: the whole sample is protected
    widevine::Cdm::EncryptionScheme scheme = widevine::Cdm::kAesCtr;
    uint32_t pattern_encrypted = 0;   // 'cbcs'/'cens' crypt_byte_block
    uint32_t pattern_clear = 0;       // skip_byte_block
    bool is_video = true;
};

// Decrypts |data| in place with the session's keys.
OpenCDMError decrypt_in_place(OpenCDMSession* session, const SampleDescription& sample, uint8_t* data, size_t length);

} // namespace ocdm_widevine
