/* OpenCDM C API, as used by WebKit's Thunder EME backend (WebCore/platform/graphics/gstreamer/eme/CDMThunder*).
 *
 * This is our own declaration of the subset WebKit calls; the names and types follow WebKit's usage so WebKit
 * builds against it unchanged with ENABLE_THUNDER=ON. The implementation (libocdm.so) is not Thunder: it drives
 * the TV's Widevine CE CDM in-process (see docs/widevine-cdm-abi.md). */
#ifndef OCDM_OPEN_CDM_H
#define OCDM_OPEN_CDM_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct OpenCDMSystem;
struct OpenCDMSession;

typedef enum {
    Temporary = 0,
    PersistentUsageRecord,
    PersistentLicense,
} LicenseType;

typedef enum {
    Usable = 0,
    Expired,
    Released,
    OutputRestricted,
    OutputDownscaled,
    StatusPending,
    InternalError,
    HWError,
} KeyStatus;
typedef KeyStatus ThunderKeyStatus; /* WebKit's name for it */

typedef enum {
    ERROR_NONE = 0,
    ERROR_UNKNOWN = 1,
    ERROR_MORE_DATA_AVAILBALE = 2,
    ERROR_INTERFACE_NOT_IMPLEMENTED = 3,
    ERROR_BUFFER_TOO_SMALL = 4,
    ERROR_INVALID_ACCESSOR = 0x80000001,
    ERROR_KEYSYSTEM_NOT_SUPPORTED = 0x80000002,
    ERROR_INVALID_SESSION = 0x80000003,
    ERROR_INVALID_DECRYPT_BUFFER = 0x80000004,
    ERROR_OUT_OF_MEMORY = 0x80000005,
    ERROR_FAIL = 0x80004005,
    ERROR_INVALID_ARG = 0x80070057,
} OpenCDMError;

/* Callbacks may run on any thread (WebKit posts them to its main thread).
 * process_challenge_callback: |challenge| starts with "<n>:Type:", n being the EME MediaKeyMessageType
 * (0 license-request, 1 license-renewal, 2 license-release, 3 individualization-request).
 * After a license update: key_update_callback once per key, then keys_updated_callback once. */
typedef struct {
    void (*process_challenge_callback)(struct OpenCDMSession* session, void* userData, const char url[],
        const uint8_t challenge[], const uint16_t challengeLength);
    void (*key_update_callback)(struct OpenCDMSession* session, void* userData, const uint8_t keyId[],
        const uint8_t length);
    void (*error_message_callback)(struct OpenCDMSession* session, void* userData, const char message[]);
    void (*keys_updated_callback)(const struct OpenCDMSession* session, void* userData);
} OpenCDMSessionCallbacks;

/* ERROR_NONE if |keySystem| (and |mimeType|, unless empty) can be handled. */
OpenCDMError opencdm_is_type_supported(const char keySystem[], const char mimeType[]);

struct OpenCDMSystem* opencdm_create_system(const char keySystem[]);
OpenCDMError opencdm_destruct_system(struct OpenCDMSystem* system);

bool opencdm_system_supports_server_certificate(struct OpenCDMSystem* system);
OpenCDMError opencdm_system_set_server_certificate(struct OpenCDMSystem* system, const uint8_t serverCertificate[],
    const uint16_t serverCertificateLength);

/* A new reference to the session holding a usable |keyId|, waiting up to |waitTime| ms; NULL if none.
 * Release it with opencdm_destruct_session(). */
struct OpenCDMSession* opencdm_get_system_session(struct OpenCDMSystem* system, const uint8_t keyId[],
    const uint8_t length, const uint32_t waitTime);

/* Creates a session and generates its license request (delivered through process_challenge_callback).
 * |*session| holds one reference; release it with opencdm_destruct_session(). */
OpenCDMError opencdm_construct_session(struct OpenCDMSystem* system, const LicenseType licenseType,
    const char initDataType[], const uint8_t initData[], const uint16_t initDataLength, const uint8_t CDMData[],
    const uint16_t CDMDataLength, OpenCDMSessionCallbacks* callbacks, void* userData,
    struct OpenCDMSession** session);

/* Drops one reference; the session is closed and freed with the last one. */
OpenCDMError opencdm_destruct_session(struct OpenCDMSession* session);

const char* opencdm_session_id(const struct OpenCDMSession* session);
KeyStatus opencdm_session_status(const struct OpenCDMSession* session, const uint8_t keyId[], const uint8_t length);

OpenCDMError opencdm_session_load(struct OpenCDMSession* session);
OpenCDMError opencdm_session_update(struct OpenCDMSession* session, const uint8_t keyMessage[],
    const uint16_t keyLength);
OpenCDMError opencdm_session_remove(struct OpenCDMSession* session);
OpenCDMError opencdm_session_close(struct OpenCDMSession* session);

#ifdef __cplusplus
}
#endif

#endif /* OCDM_OPEN_CDM_H */
