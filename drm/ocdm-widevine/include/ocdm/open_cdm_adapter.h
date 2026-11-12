/* GStreamer entry points of the OpenCDM API, as used by WebKit's Thunder decryptor (CDMProxyThunder).
 * Declaring opencdm_gstreamer_session_decrypt_buffer makes WebKit use it (THUNDER_HAS_OCDM_DECRYPT_BUFFER),
 * which reads everything from the buffer's GstProtectionMeta and so also supports 'cbcs'. */
#ifndef OCDM_OPEN_CDM_ADAPTER_H
#define OCDM_OPEN_CDM_ADAPTER_H

#include "open_cdm.h"

#include <gst/gst.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Decrypts |buffer| in place using its GstProtectionMeta ("kid", "iv"/"constant_iv", "subsamples",
 * "cipher-mode", "crypt_byte_block", "skip_byte_block"). |caps| tells audio from video. */
OpenCDMError opencdm_gstreamer_session_decrypt_buffer(struct OpenCDMSession* session, GstBuffer* buffer,
    GstCaps* caps);

/* Older form ('cenc' only): subsamples, IV and key id passed explicitly. */
OpenCDMError opencdm_gstreamer_session_decrypt(struct OpenCDMSession* session, GstBuffer* buffer,
    GstBuffer* subSample, const uint32_t subSampleCount, GstBuffer* IV, GstBuffer* keyID, uint32_t initWithLast15);

#ifdef __cplusplus
}
#endif

#endif /* OCDM_OPEN_CDM_ADAPTER_H */
