// A stand-in for libwidevine_ce_cdm_shared.so with the same entry points and vtable (built from cdm.h), so the
// OpenCDM layer can be tested on the build machine. Behaviour:
//  - provisioned once storage holds "cert.bin"; the provisioning request is "FAKE-PROVISIONING-REQUEST" and a
//    reply containing "signedResponse" is accepted;
//  - generateRequest answers with onMessage(kLicenseRequest, "REQ:" + init data);
//  - update(response) with "KEYS:" + 16-byte key ids makes those keys usable (onKeyStatusesChange);
//  - decrypt XORs protected bytes with 0x5A, copies clear bytes, and checks the batch like the real CDM does.
#include <widevine/cdm.h>

#include <cstring>
#include <map>
#include <set>

using widevine::Cdm;

namespace {

Cdm::IStorage* g_storage = nullptr;
bool g_initialized = false;

// What the last decrypt() call asked for, read back by tests through fake_cdm_last_batch().
struct LastBatch {
    int scheme = -1;
    uint32_t pattern_encrypted = 0, pattern_clear = 0;
    bool is_video = false;
    uint32_t subsamples = 0;
    uint8_t iv[16] = { 0 };
} g_last;

class FakeCdm final : public Cdm {
public:
    FakeCdm(IEventListener* listener)
        : listener_(listener)
    {
    }
    void onTimerExpired(void*) override { }

    Status setServiceCertificate(ServiceRole, const std::string&) override { return kNotSupported; }
    Status getServiceCertificateRequest(std::string*) override { return kNotSupported; }
    Status parseAndLoadServiceCertificateResponse(ServiceRole, const std::string&, std::string*) override { return kNotSupported; }
    Status getRobustnessLevel(RobustnessLevel* level) override { *level = kL1; return kSuccess; }
    Status getResourceRatingTier(uint32_t* tier) override { *tier = 1; return kSuccess; }
    Status getOemCryptoBuildInfo(std::string* info) override { *info = "fake"; return kSuccess; }
    bool isProvisioned() override { return g_storage->exists("cert.bin"); }
    Status getProvisioningRequest(std::string* request) override
    {
        *request = "FAKE-PROVISIONING-REQUEST";
        return kSuccess;
    }
    Status handleProvisioningResponse(const std::string& response) override
    {
        if (response.find("signedResponse") == std::string::npos)
            return kUnexpectedError;
        return g_storage->write("cert.bin", response) ? kSuccess : kUnexpectedError;
    }
    Status removeProvisioning() override { return kNotSupported; }
    Status listStoredLicenses(std::vector<std::string>*) override { return kNotSupported; }
    Status listUsageRecords(std::vector<std::string>*) override { return kNotSupported; }
    Status deleteUsageRecord(const std::string&) override { return kNotSupported; }
    Status deleteAllUsageRecords() override { return kNotSupported; }
    Status getStatusForHdcpVersion(HdcpVersion, KeyStatus*) override { return kNotSupported; }

    Status createSession(SessionType, std::string* id) override
    {
        if (!isProvisioned())
            return kNeedsDeviceCertificate;
        *id = "ksid" + std::to_string(++counter_);
        sessions_[*id];
        return kSuccess;
    }
    Status generateRequest(const std::string& id, InitDataType, const std::string& init_data) override
    {
        if (!sessions_.count(id))
            return kSessionNotFound;
        listener_->onMessage(id, kLicenseRequest, "REQ:" + init_data);
        return kSuccess;
    }
    Status load(const std::string&) override { return kNotSupported; }
    Status update(const std::string& id, const std::string& response) override
    {
        auto it = sessions_.find(id);
        if (it == sessions_.end())
            return kSessionNotFound;
        if (response.compare(0, 5, "KEYS:") || (response.size() - 5) % 16)
            return kTypeError;
        for (size_t at = 5; at < response.size(); at += 16)
            it->second.insert(response.substr(at, 16));
        listener_->onKeyStatusesChange(id, true);
        return kSuccess;
    }
    Status loadEmbeddedKeys(const std::string&, InitDataType, const std::string&) override { return kNotSupported; }
    Status getExpiration(const std::string&, int64_t* expiration) override { *expiration = -1; return kSuccess; }
    Status getKeyStatuses(const std::string& id, KeyStatusMap* statuses) override
    {
        auto it = sessions_.find(id);
        if (it == sessions_.end())
            return kSessionNotFound;
        statuses->clear();
        for (const std::string& key : it->second)
            (*statuses)[key] = kUsable;
        return kSuccess;
    }
    void reservedSlot25() override { }
    void reservedSlot26() override { }
    Status close(const std::string& id) override
    {
        return sessions_.erase(id) ? kSuccess : kSessionNotFound;
    }
    Status remove(const std::string&) override { return kNotSupported; }
    Status forceRemove(const std::string&) override { return kNotSupported; }
    void reservedSlot30() override { }
    void reservedSlot31() override { }
    void reservedSlot32() override { }
    Status decrypt(const std::string& id, const DecryptionBatch& batch) override
    {
        auto it = sessions_.find(id);
        if (it == sessions_.end())
            return kSessionNotFound;
        if (batch.is_secure)
            return kNotSupported;
        if (!it->second.count(std::string(reinterpret_cast<const char*>(batch.key_id), batch.key_id_length)))
            return kNoKey;
        if (batch.encryption_scheme == kAesCtr && (batch.pattern_encrypted || batch.pattern_clear))
            return kTypeError;
        g_last.scheme = batch.encryption_scheme;
        g_last.pattern_encrypted = batch.pattern_encrypted;
        g_last.pattern_clear = batch.pattern_clear;
        g_last.is_video = batch.is_video;
        g_last.subsamples = batch.samples_length ? batch.samples[0].subsamples_length : 0;
        if (batch.samples_length)
            memcpy(g_last.iv, batch.samples[0].iv, 16);
        for (uint32_t i = 0; i < batch.samples_length; i++) {
            const Sample& s = batch.samples[i];
            if (s.iv_length != 16 || s.output_length - s.output_offset < s.data_length)
                return kTypeError;
            uint32_t at = 0;
            for (uint32_t j = 0; j < s.subsamples_length; j++) {
                const Subsample& sub = s.subsamples[j];
                if (at + sub.clear_bytes + sub.protected_bytes > s.data_length)
                    return kRangeError;
                memcpy(s.output + s.output_offset + at, s.data + at, sub.clear_bytes);
                at += sub.clear_bytes;
                for (uint32_t k = 0; k < sub.protected_bytes; k++, at++)
                    s.output[s.output_offset + at] = s.data[at] ^ 0x5A;
            }
        }
        return kSuccess;
    }

private:
    IEventListener* listener_;
    int counter_ = 0;
    std::map<std::string, std::set<std::string>> sessions_;
};

} // namespace

namespace widevine {

Cdm::Status Cdm::initialize(SecureOutputType, const ClientInfo& info, IStorage* storage, IClock*, ITimer*, LogLevel)
{
    if (info.product_name.empty() || info.company_name.empty() || info.model_name.empty())
        return kTypeError;
    g_storage = storage;
    g_initialized = true;
    return kSuccess;
}

const char* Cdm::version()
{
    return "fake-16.3.0";
}

Cdm* Cdm::create(IEventListener* listener, IStorage*, bool)
{
    return g_initialized ? new FakeCdm(listener) : nullptr;
}

} // namespace widevine

extern "C" void fake_cdm_last_batch(int* scheme, uint32_t* pattern_encrypted, uint32_t* pattern_clear, bool* is_video,
    uint32_t* subsamples, uint8_t iv[16])
{
    *scheme = g_last.scheme;
    *pattern_encrypted = g_last.pattern_encrypted;
    *pattern_clear = g_last.pattern_clear;
    *is_video = g_last.is_video;
    *subsamples = g_last.subsamples;
    memcpy(iv, g_last.iv, 16);
}
