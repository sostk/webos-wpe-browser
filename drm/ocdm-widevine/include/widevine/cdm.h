// Declarations for the device's Widevine CE CDM (libwidevine_ce_cdm_shared.so, v16.3.0, LG webOS 6).
//
// This is our own description of the library's C++ interface, written so we can call the CDM that
// ships on the TV. The vtable order was established against the real library on the TV (see
// docs/widevine-cdm-abi.md). Every virtual is tagged:
//   VERIFIED  - called on the TV with the expected effect
//   EXPECTED  - follows the verified ordering, not yet exercised; prove before relying on it
// Changing the order or the parameter types of any virtual changes the vtable and breaks the ABI.
#ifndef OCDM_WIDEVINE_CDM_H
#define OCDM_WIDEVINE_CDM_H

#include <stdint.h>
#include <map>
#include <string>
#include <vector>

namespace widevine {

// Base of Cdm; the CDM is its own timer client. Slot 0 of the Cdm vtable (VERIFIED by position).
class ITimerClient {
public:
    virtual void onTimerExpired(void* context) = 0;

protected:
    ITimerClient() { }
    virtual ~ITimerClient() { }
};

class Cdm : public ITimerClient {
public:
    // Enum values: VERIFIED where noted, otherwise as in Google's published CE CDM header.
    typedef enum {
        kTemporary = 0,              // VERIFIED
        kPersistentLicense = 1,
        kPersistentUsageRecord = 2,
    } SessionType;

    typedef enum {
        kLicenseRequest = 0,         // VERIFIED (generateRequest -> onMessage)
        kLicenseRenewal = 1,
        kLicenseRelease = 2,
        kIndividualizationRequest = 3,
    } MessageType;

    // v16 numbering (NOT the 2015 header's): EME-style errors are 1-5, Widevine's own are 101+.
    // Treat any value not listed as a generic failure.
    typedef enum {
        kSuccess = 0,                   // VERIFIED
        kTypeError = 1,
        kNotSupported = 2,
        kInvalidState = 3,
        kQuotaExceeded = 4,
        kRangeError = 5,
        kNeedsDeviceCertificate = 101,  // VERIFIED (createSession before provisioning)
        kSessionNotFound = 102,         // VERIFIED (generateRequest/getKeyStatuses on a missing session)
        kDecryptError = 103,
        kNoKey = 104,
        kKeyUsageBlockedByPolicy = 105,
        kUnexpectedError = 99999,       // VERIFIED (handleProvisioningResponse with an empty reply)
    } Status;

    typedef enum {
        kCenc = 0,                   // VERIFIED
        kKeyIds = 1,
        kWebM = 2,
    } InitDataType;

    typedef enum {
        kUsable = 0,
        kExpired = 1,
        kOutputRestricted = 2,
        kStatusPending = 3,
        kInternalError = 4,
        kReleased = 5,
    } KeyStatus;

    typedef enum {
        kOpaqueHandle = 0,
        kDirectRender = 1,
        kNoSecureOutput = 2,         // VERIFIED (initialize() accepts it)
    } SecureOutputType;

    typedef enum {
        kSilent = -1,
        kErrors = 0,                 // VERIFIED (initialize() accepts it)
        kWarnings = 1,
        kInfo = 2,
        kDebug = 3,
        kVerbose = 4,
    } LogLevel;

    typedef enum {
        kAllServices = 0,
        kProvisioningService = 1,
        kLicensingService = 2,
    } ServiceRole;

    typedef enum {
        kLevelDefault = 0,
        kL1 = 1,
        kL2 = 2,
        kL3 = 3,
    } RobustnessLevel;

    typedef enum {
        kHdcp1_x = 0,
        kHdcp2_0 = 1,
        kHdcp2_1 = 2,
        kHdcp2_2 = 3,
        kHdcp2_3 = 4,
    } HdcpVersion;

    typedef std::map<std::string, KeyStatus> KeyStatusMap;

    // Callbacks from the CDM. Method set from the v16 OpenCDM plugin.
    class IEventListener {
    public:
        virtual void onMessage(const std::string& session_id, MessageType message_type, const std::string& message) = 0; // 0 VERIFIED
        virtual void onKeyStatusesChange(const std::string& session_id, bool has_new_usable_key) = 0;                  // 1 VERIFIED
        virtual void onRemoveComplete(const std::string& session_id) = 0;                                               // 2 EXPECTED
        virtual void onDeferredComplete(const std::string& session_id, Status result) = 0;                              // 3 EXPECTED
        virtual void onDirectIndividualizationRequest(const std::string& session_id, const std::string& request) = 0;  // 4 EXPECTED

    protected:
        IEventListener() { }
        virtual ~IEventListener() { }
    };

    // Persistent storage, provided by us. VERIFIED: the CDM calls read/write/exists/size by these slots.
    class IStorage {
    public:
        virtual bool read(const std::string& name, std::string* data) = 0;
        virtual bool write(const std::string& name, const std::string& data) = 0;
        virtual bool exists(const std::string& name) = 0;
        virtual bool remove(const std::string& name) = 0;
        virtual int32_t size(const std::string& name) = 0;
        virtual bool list(std::vector<std::string>* names) = 0;

    protected:
        IStorage() { }
        virtual ~IStorage() { }
    };

    // Wall clock in milliseconds since 1970 UTC. VERIFIED. Must be the real time: the device
    // certificate carries its creation time and is refused if the clock is behind it.
    class IClock {
    public:
        virtual int64_t now() = 0;

    protected:
        IClock() { }
        virtual ~IClock() { }
    };

    // One-shot timers (license renewal, policy timers). Slot order EXPECTED (from the OpenCDM plugin).
    class ITimer {
    public:
        typedef ITimerClient IClient;
        virtual void setTimeout(int64_t delay_ms, IClient* client, void* context) = 0;
        virtual void cancel(IClient* client) = 0;

    protected:
        ITimer() { }
        virtual ~ITimer() { }
    };

    // Field order VERIFIED by use (initialize() rejects an empty product/company/model).
    // Values end up in license requests: never put user-identifying data here.
    struct ClientInfo {
        std::string product_name;
        std::string company_name;
        std::string device_name;
        std::string model_name;
        std::string arch_name;
        std::string build_info;
    };

    // Decrypt buffers: NOT VERIFIED for v16. This is the older published layout; v16 changed the
    // sample description (patterns/subsamples). Do not call decrypt() until the layout is proven.
    typedef enum { kClear = 0, kAesCtr = 1, kAesCbc = 2 } EncryptionScheme;
    struct Pattern {
        uint32_t encrypted_blocks;
        uint32_t clear_blocks;
    };
    struct InputBuffer;
    struct OutputBuffer;

    // ---- static entry points (exported symbols, VERIFIED by mangled name) ----
    static Status initialize(SecureOutputType secure_output_type, const ClientInfo& client_info, IStorage* storage,
        IClock* clock, ITimer* timer, LogLevel verbosity);
    static Status initialize(SecureOutputType secure_output_type, const ClientInfo& client_info, IStorage* storage,
        IClock* clock, ITimer* timer, LogLevel verbosity, const std::string& sandbox_id);
    static const char* version();
    static Cdm* create(IEventListener* listener, IStorage* storage, bool privacy_mode);
    static Cdm* create(IEventListener* listener, IStorage* storage, bool privacy_mode, bool storage_is_read_only);

    // ---- instance virtuals; the slot number is the vtable index (0 = onTimerExpired, 1-2 = dtor) ----
    virtual ~Cdm() { }
    virtual Status setServiceCertificate(ServiceRole role, const std::string& certificate) = 0;                 // 3  EXPECTED
    virtual Status getServiceCertificateRequest(std::string* message) = 0;                                      // 4  VERIFIED
    virtual Status parseAndLoadServiceCertificateResponse(ServiceRole role, const std::string& response,
        std::string* certificate) = 0;                                                                         // 5  EXPECTED
    virtual Status getRobustnessLevel(RobustnessLevel* level) = 0;                                              // 6  EXPECTED
    virtual Status getResourceRatingTier(uint32_t* tier) = 0;                                                   // 7  EXPECTED
    virtual Status getOemCryptoBuildInfo(std::string* build_info) = 0;                                          // 8  VERIFIED
    virtual bool isProvisioned() = 0;                                                                           // 9  VERIFIED
    virtual Status getProvisioningRequest(std::string* request) = 0;                                            // 10 VERIFIED
    virtual Status handleProvisioningResponse(const std::string& response) = 0;                                 // 11 VERIFIED
    virtual Status removeProvisioning() = 0;                                                                    // 12 EXPECTED
    virtual Status listStoredLicenses(std::vector<std::string>* key_set_ids) = 0;                               // 13 EXPECTED
    virtual Status listUsageRecords(std::vector<std::string>* key_set_ids) = 0;                                 // 14 EXPECTED
    virtual Status deleteUsageRecord(const std::string& key_set_id) = 0;                                        // 15 EXPECTED
    virtual Status deleteAllUsageRecords() = 0;                                                                 // 16 EXPECTED
    virtual Status getStatusForHdcpVersion(HdcpVersion hdcp, KeyStatus* key_status) = 0;                        // 17 EXPECTED
    virtual Status createSession(SessionType session_type, std::string* session_id) = 0;                        // 18 VERIFIED
    virtual Status generateRequest(const std::string& session_id, InitDataType init_data_type,
        const std::string& init_data) = 0;                                                                     // 19 VERIFIED
    virtual Status load(const std::string& session_id) = 0;                                                     // 20 EXPECTED
    virtual Status update(const std::string& session_id, const std::string& response) = 0;                      // 21 VERIFIED
    virtual Status loadEmbeddedKeys(const std::string& session_id, InitDataType init_data_type,
        const std::string& init_data) = 0;                                                                     // 22 EXPECTED
    virtual Status getExpiration(const std::string& session_id, int64_t* expiration) = 0;                       // 23 VERIFIED (-1 = none)
    virtual Status getKeyStatuses(const std::string& session_id, KeyStatusMap* key_statuses) = 0;               // 24 VERIFIED
    // Slots 25-26: two getKeyAllowedUsages overloads (EXPECTED). Not callable through this header: their
    // flag struct is unknown, so they are placeholders that only keep close() at slot 27.
    virtual void reservedSlot25() = 0;                                                                          // 25
    virtual void reservedSlot26() = 0;                                                                          // 26
    virtual Status close(const std::string& session_id) = 0;                                                    // 27 VERIFIED
    // Slots 28 and beyond (remove, forceRemove, decrypt, ...) are not declared yet: declaring them
    // wrongly would silently call the wrong method. They are added as each one is verified on the TV.

protected:
    Cdm() { }
};

} // namespace widevine

#endif // OCDM_WIDEVINE_CDM_H
